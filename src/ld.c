/*
 * ld.c - Simple periodic directory size cache daemon
 *
 * Periodically scans directories and caches sizes for large directories.
 * Skips network filesystems automatically.
 */

#include "common.h"
#include "cache.h"
#include "scan.h"
#include <stdarg.h>
#include <signal.h>
#include <time.h>

#define LOG_FILE "/tmp/l-cached.log"

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_refresh = 0;

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

static void write_status(const char *status) {
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.cache/l/status", home ? home : "/tmp");
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", status);
        fclose(f);
    }
}

/* ============================================================================
 * Store Callback
 * ============================================================================ */

static void store_callback(const char *path, off_t size, long count) {
    if (cache_daemon_store(path, size, count) == 0)
        log_info("cached %s (%ld files)", path, count);
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
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);
    signal(SIGUSR1, handle_refresh);

    int scan_interval = config_get_interval();
    long threshold = config_get_threshold();
    log_info("starting (scan interval: %ds)", scan_interval);

    while (!g_shutdown) {
        rotate_log();
        time_t start = time(NULL);

        /* Create fresh temp database for this scan */
        if (cache_daemon_init() != 0) {
            log_error("cache init failed");
            write_status("error");
            sleep(60);
            continue;
        }

        write_status("scanning");
        log_info("scanning /...");
        ScanResult r = scan_directory("/", store_callback, NULL,
                                       (volatile int *)&g_shutdown, threshold);
        log_info("  /: %ld files, %lld bytes", r.file_count, (long long)r.size);

        /* Get count before save (save closes the db) */
        int cached = cache_daemon_count();

        /* Atomic replace: temp db -> final db */
        if (cache_daemon_save() != 0)
            log_error("cache save failed");

        time_t elapsed = time(NULL) - start;
        log_info("scan complete (%lds, %d cached)", elapsed, cached);
        write_status("idle");

        for (int i = 0; i < scan_interval && !g_shutdown && !g_refresh; i++)
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
