/*
 * ld.c - Simple periodic directory size cache daemon
 *
 * Periodically scans directories and caches sizes for large directories.
 * Uses directory mtime to skip unchanged subtrees efficiently.
 */

#include "common.h"
#include "cache.h"
#include <stdarg.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>

#define LOG_FILE "/tmp/l-cached.log"

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_refresh = 0;
static char g_roots[L_MAX_ROOTS][PATH_MAX];
static int g_root_count = 0;

/* ============================================================================
 * Logging
 * ============================================================================ */

static void log_msg(const char *level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    fprintf(stderr, "[%02d:%02d:%02d] %s: ",
            tm->tm_hour, tm->tm_min, tm->tm_sec, level);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

#define log_error(...) log_msg("ERROR", __VA_ARGS__)
#define log_info(...)  log_msg("INFO", __VA_ARGS__)

static void rotate_log(void) {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > L_MAX_LOG_SIZE) {
        if (truncate(LOG_FILE, 0) == 0)
            log_info("log rotated");
    }
}

/* ============================================================================
 * Directory Scanning
 * ============================================================================ */

static int is_other_root(const char *path, int current_idx) {
    for (int i = 0; i < g_root_count; i++) {
        if (i != current_idx && strcmp(path, g_roots[i]) == 0)
            return 1;
    }
    return 0;
}

typedef struct {
    off_t size;
    long file_count;
} ScanResult;

static ScanResult scan_dir(const char *path, dev_t root_dev, int root_idx) {
    ScanResult result = {0, 0};
    if (g_shutdown) return result;

    struct stat dir_st;
    if (stat(path, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode))
        return (ScanResult){-1, -1};

    if (root_dev == 0) {
        root_dev = dir_st.st_dev;
    } else if (dir_st.st_dev != root_dev) {
        return (ScanResult){0, 0};
    }

    /* Check cache - skip if mtime unchanged */
    const CacheEntry *cached = cache_daemon_lookup(path);
    if (cached && cached->dir_mtime == dir_st.st_mtime &&
        cached->size >= 0 && cached->file_count >= 0) {
        return (ScanResult){cached->size, cached->file_count};
    }

    int skip_file_count = path_is_git_dir(path);

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return (ScanResult){-1, -1};

    DIR *dir = fdopendir(dirfd);
    if (!dir) {
        close(dirfd);
        return (ScanResult){-1, -1};
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (g_shutdown) break;

        if (PATH_IS_DOT_OR_DOTDOT(entry->d_name))
            continue;

        int is_dir = 0, is_file = 0;
        off_t file_size = 0;

#ifdef DT_DIR
        if (entry->d_type == DT_DIR) {
            is_dir = 1;
        } else if (entry->d_type == DT_REG) {
            is_file = 1;
            struct stat st;
            if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0)
                file_size = st.st_size;
        } else if (entry->d_type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_file = S_ISREG(st.st_mode);
                file_size = st.st_size;
            }
        }
#else
        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
            file_size = st.st_size;
        }
#endif

        if (is_file) {
            result.size += file_size;
            if (!skip_file_count) result.file_count++;
        } else if (is_dir) {
            char full[PATH_MAX];
            size_t plen = strlen(path);
            int need_slash = (plen > 0 && path[plen - 1] != '/');
            snprintf(full, sizeof(full), need_slash ? "%s/%s" : "%s%s",
                     path, entry->d_name);

            if (path_should_skip_firmlink(full)) continue;

            if (is_other_root(full, root_idx)) {
                const CacheEntry *other = cache_daemon_lookup(full);
                if (other && other->size >= 0) {
                    result.size += other->size;
                    if (!skip_file_count && other->file_count >= 0)
                        result.file_count += other->file_count;
                }
                continue;
            }

            ScanResult sub = scan_dir(full, root_dev, root_idx);
            if (sub.size >= 0) result.size += sub.size;
            if (!skip_file_count && sub.file_count >= 0)
                result.file_count += sub.file_count;
        }
    }
    closedir(dir);

    /* Cache if meets threshold */
    if (!skip_file_count && result.file_count >= L_FILE_COUNT_THRESHOLD) {
        if (cache_daemon_store(path, result.size, result.file_count, dir_st.st_mtime) == 0)
            log_info("cached %s (%ld files)", path, result.file_count);
    }

    if (skip_file_count) result.file_count = -1;
    return result;
}

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

static void handle_shutdown(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void handle_refresh(int sig) {
    (void)sig;
    g_refresh = 1;
}

/* ============================================================================
 * Root Management
 * ============================================================================ */

static int add_root(const char *path) {
    if (g_root_count >= L_MAX_ROOTS) return -1;
    if (!realpath(path, g_roots[g_root_count])) return -1;
    for (int i = 0; i < g_root_count; i++) {
        if (strcmp(g_roots[i], g_roots[g_root_count]) == 0)
            return 0;
    }
    log_info("root: %s", g_roots[g_root_count]);
    g_root_count++;
    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fprintf(stderr, "Usage: %s [root...]\n", argv[0]);
        return 0;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; i++)
            add_root(argv[i]);
    } else {
        add_root("/");
        const char *home = getenv("HOME");
        if (home) add_root(home);
    }

    if (g_root_count == 0) {
        log_error("no valid roots");
        return 1;
    }

    if (cache_daemon_init() != 0) {
        log_error("cache init failed");
        return 1;
    }

    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGUSR1, handle_refresh);

    log_info("starting (scan interval: %ds)", L_SCAN_INTERVAL);

    while (!g_shutdown) {
        rotate_log();
        time_t start = time(NULL);

        for (int i = g_root_count - 1; i >= 0 && !g_shutdown; i--) {
            log_info("scanning %s...", g_roots[i]);
            ScanResult r = scan_dir(g_roots[i], 0, i);
            log_info("  %s: %ld files, %lld bytes",
                     g_roots[i], r.file_count, (long long)r.size);
        }

        int pruned = cache_daemon_prune_stale();
        if (pruned > 0)
            log_info("pruned %d stale entries", pruned);

        if (cache_daemon_save() != 0)
            log_error("cache save failed");

        time_t elapsed = time(NULL) - start;
        log_info("scan complete (%lds, %d cached)", elapsed, cache_daemon_count());

        for (int i = 0; i < L_SCAN_INTERVAL && !g_shutdown && !g_refresh; i++)
            sleep(1);

        if (g_refresh) {
            log_info("manual refresh requested");
            g_refresh = 0;
        }
    }

    cache_daemon_close();
    log_info("shutdown");
    return 0;
}
