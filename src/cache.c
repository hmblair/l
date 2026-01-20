/*
 * cache.c - Client-side cache operations and directory statistics
 */

#include "cache.h"
#include "scan.h"
#include <sqlite3.h>
#include <pthread.h>

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
    const char *sql = "SELECT size, file_count FROM sizes WHERE path = ?";
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
 * Directory Statistics (uses shared scan.c implementation)
 * ============================================================================ */

DirStats dir_stats_get(const char *path, dir_stats_cache_fn cache_fn) {
    ScanResult r = scan_directory(path, NULL, cache_fn, NULL, 0);
    return (DirStats){r.size, r.file_count};
}

/* Cache lookup wrapper for dir_stats */
int cache_lookup_wrapper(const char *path, off_t *size, long *count) {
    const CacheEntry *cached = cache_lookup_entry(path);
    if (cached && cached->size >= 0 && cached->file_count >= 0) {
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
