/*
 * cache.c - Client-side cache operations and directory statistics
 */

#include "cache.h"
#include <sqlite3.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* ============================================================================
 * Client-side Cache (read-only)
 * ============================================================================ */

static sqlite3 *g_db = NULL;
static sqlite3_stmt *g_lookup_stmt = NULL;
static pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;

int cache_load(void) {
    char path[PATH_MAX];
    cache_get_path(path, sizeof(path));

    /* Use READWRITE to allow WAL recovery, but we only read */
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(path, &g_db, flags, NULL) != SQLITE_OK) {
        /* Fall back to readonly if file doesn't exist */
        if (sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            g_db = NULL;
            return -1;
        }
    }

    /* Set busy timeout to avoid conflicts with daemon */
    sqlite3_busy_timeout(g_db, 1000);

    /* Prepare lookup statement */
    const char *sql = "SELECT size, file_count, dir_mtime FROM sizes WHERE path = ?";
    if (sqlite3_prepare_v2(g_db, sql, -1, &g_lookup_stmt, NULL) != SQLITE_OK) {
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    return 0;
}

int cache_lookup(const char *path, CacheEntry *out) {
    if (!g_db || !g_lookup_stmt) return 0;

    pthread_mutex_lock(&g_db_lock);

    sqlite3_reset(g_lookup_stmt);
    sqlite3_bind_text(g_lookup_stmt, 1, path, -1, SQLITE_STATIC);

    int found = 0;
    if (sqlite3_step(g_lookup_stmt) == SQLITE_ROW) {
        out->size = sqlite3_column_int64(g_lookup_stmt, 0);
        out->file_count = sqlite3_column_int64(g_lookup_stmt, 1);
        out->dir_mtime = sqlite3_column_int64(g_lookup_stmt, 2);
        found = 1;
    }

    pthread_mutex_unlock(&g_db_lock);
    return found;
}

const CacheEntry *cache_lookup_entry(const char *path) {
    static __thread CacheEntry entry;  /* Thread-local storage for OpenMP safety */
    if (cache_lookup(path, &entry)) {
        return &entry;
    }
    return NULL;
}

void cache_unload(void) {
    if (g_lookup_stmt) {
        sqlite3_finalize(g_lookup_stmt);
        g_lookup_stmt = NULL;
    }
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/* ============================================================================
 * Directory Statistics
 * ============================================================================ */

/* Internal: recursive task function using directory fd for efficiency */
static DirStats ds_get_stats_impl(const char *path, dir_stats_cache_fn cache_fn, dev_t root_dev) {
    DirStats result = {-1, -1};

    /* For .git directories, don't count files (but still compute size) */
    int skip_file_count = path_is_git_dir(path);

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return result;

    /* Get device ID if this is the root call */
    struct stat dir_st;
    if (fstat(dirfd, &dir_st) != 0) {
        close(dirfd);
        return result;
    }
    if (root_dev == 0) {
        root_dev = dir_st.st_dev;
    } else if (dir_st.st_dev != root_dev) {
        /* Different filesystem - skip to avoid double-counting */
        close(dirfd);
        return (DirStats){0, 0};
    }

    DIR *dir = fdopendir(dirfd);
    if (!dir) {
        close(dirfd);
        return result;
    }

    /* Collect entries first (sequential phase) */
    char **subdirs = NULL;
    size_t subdir_count = 0;
    size_t subdir_cap = 0;
    off_t file_size_total = 0;
    long file_count_total = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (PATH_IS_DOT_OR_DOTDOT(entry->d_name))
            continue;

        int is_dir = 0;
        int need_stat = 0;

#ifdef DT_DIR
        /* Use d_type if available (avoids stat for type check) */
        if (entry->d_type == DT_DIR) {
            is_dir = 1;
        } else if (entry->d_type == DT_REG) {
            /* Regular file - still need stat for size */
            need_stat = 1;
        } else if (entry->d_type == DT_LNK) {
            /* Symlink - count it (size is negligible) */
            file_count_total++;
        } else if (entry->d_type == DT_UNKNOWN) {
            /* Filesystem doesn't support d_type, fall back to stat */
            need_stat = 1;
        }
        /* Skip devices, sockets, etc. */
#else
        need_stat = 1;
#endif

        if (need_stat) {
            struct stat st;
            if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    is_dir = 1;
                } else if (S_ISREG(st.st_mode)) {
                    file_size_total += st.st_size;
                    file_count_total++;
                } else if (S_ISLNK(st.st_mode)) {
                    /* Symlink - count it (size is negligible) */
                    file_count_total++;
                }
                /* Skip devices, sockets, etc. */
            }
        }

        if (is_dir) {
            /* Build full path for subdirectory */
            size_t path_len = strlen(path);
            size_t name_len = strlen(entry->d_name);
            /* Avoid double slash when path is "/" */
            int need_slash = (path_len > 0 && path[path_len - 1] != '/');
            size_t subpath_len = path_len + need_slash + name_len + 1;
            char *subpath = malloc(subpath_len);
            if (!subpath) {
                for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
                free(subdirs);
                closedir(dir);
                return result;
            }
            if (need_slash) {
                memcpy(subpath, path, path_len);
                subpath[path_len] = '/';
                memcpy(subpath + path_len + 1, entry->d_name, name_len + 1);
            } else {
                memcpy(subpath, path, path_len);
                memcpy(subpath + path_len, entry->d_name, name_len + 1);
            }

            /* Skip paths that cause double-counting (macOS firmlinks) */
            if (path_should_skip_firmlink(subpath)) {
                free(subpath);
                continue;
            }

            if (subdir_count >= subdir_cap) {
                subdir_cap = subdir_cap ? subdir_cap * 2 : 16;
                char **new_subdirs = realloc(subdirs, subdir_cap * sizeof(char *));
                if (!new_subdirs) {
                    free(subpath);
                    for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
                    free(subdirs);
                    closedir(dir);
                    return result;
                }
                subdirs = new_subdirs;
            }
            subdirs[subdir_count++] = subpath;
        }
    }
    closedir(dir);  /* Also closes dirfd */

    /* Process subdirectories (parallel phase with OMP tasks) */
    DirStats *sub_stats = NULL;
    if (subdir_count > 0) {
        sub_stats = malloc(subdir_count * sizeof(DirStats));
        if (!sub_stats) {
            for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
            free(subdirs);
            return result;
        }

        for (size_t i = 0; i < subdir_count; i++) {
            /* Check cache if available */
            off_t cached_size;
            long cached_count;
            if (cache_fn && cache_fn(subdirs[i], &cached_size, &cached_count)) {
                sub_stats[i].size = cached_size;
                sub_stats[i].file_count = cached_count;
            } else {
                #pragma omp task shared(sub_stats) firstprivate(i)
                sub_stats[i] = ds_get_stats_impl(subdirs[i], cache_fn, root_dev);
            }
        }
        #pragma omp taskwait
    }

    /* Sum results */
    off_t dir_size_total = 0;
    long dir_count_total = 0;
    for (size_t i = 0; i < subdir_count; i++) {
        if (sub_stats[i].size >= 0) dir_size_total += sub_stats[i].size;
        if (sub_stats[i].file_count >= 0) dir_count_total += sub_stats[i].file_count;
        free(subdirs[i]);
    }
    free(subdirs);
    free(sub_stats);

    result.size = file_size_total + dir_size_total;
    result.file_count = skip_file_count ? -1 : (file_count_total + dir_count_total);
    return result;
}

DirStats dir_stats_get(const char *path, dir_stats_cache_fn cache_fn) {
    DirStats result;
    #pragma omp parallel
    #pragma omp single
    {
        result = ds_get_stats_impl(path, cache_fn, 0);  /* 0 = determine root_dev from path */
    }
    return result;
}

/* Cache lookup wrapper for dir_stats with mtime validation */
int cache_lookup_wrapper(const char *path, off_t *size, long *count) {
    const CacheEntry *cached = cache_lookup_entry(path);
    if (cached && cached->size >= 0 && cached->file_count >= 0) {
        /* Validate mtime if available (skip if dir_mtime is 0 = not set) */
        if (cached->dir_mtime > 0) {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_mtime != cached->dir_mtime) {
                return 0;  /* mtime changed, cache is stale */
            }
        }
        *size = (off_t)cached->size;
        *count = (long)cached->file_count;
        return 1;
    }
    return 0;
}

DirStats get_dir_stats_cached(const char *path) {
    /* Skip virtual filesystems (proc, sysfs, etc.) - they report fake sizes */
    if (path_is_virtual_fs(path)) {
        return (DirStats){-1, -1};
    }

    /* Resolve symlinks for cache lookup (cache stores real paths) */
    char resolved[PATH_MAX];
    const char *lookup_path = path;
    if (realpath(path, resolved) != NULL) {
        lookup_path = resolved;
    }

    /* Check cache first for the top-level call */
    off_t size;
    long count;
    if (cache_lookup_wrapper(lookup_path, &size, &count)) {
        return (DirStats){size, count};
    }
    /* Use resolved path so subdirectory cache lookups match stored paths */
    return dir_stats_get(lookup_path, cache_lookup_wrapper);
}
