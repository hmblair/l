/*
 * cache_daemon.c - Daemon-side cache operations (read-write)
 */

#include "cache.h"
#include <sqlite3.h>
#include <sys/stat.h>

/* ============================================================================
 * Daemon-side Cache (read-write)
 * ============================================================================ */

static sqlite3 *d_db = NULL;
static sqlite3_stmt *d_insert_stmt = NULL;
static sqlite3_stmt *d_lookup_stmt = NULL;

/* Internal: try to open and initialize the database */
static int cache_init_internal(const char *path) {
    if (sqlite3_open(path, &d_db) != SQLITE_OK) {
        d_db = NULL;
        return -1;
    }

    /* Configure for safety and performance */
    sqlite3_exec(d_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(d_db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(d_db, "PRAGMA busy_timeout=5000", NULL, NULL, NULL);

    /* Verify database integrity */
    sqlite3_stmt *check;
    if (sqlite3_prepare_v2(d_db, "PRAGMA integrity_check", -1, &check, NULL) == SQLITE_OK) {
        if (sqlite3_step(check) == SQLITE_ROW) {
            const char *result = (const char *)sqlite3_column_text(check, 0);
            if (result && strcmp(result, "ok") != 0) {
                sqlite3_finalize(check);
                sqlite3_close(d_db);
                d_db = NULL;
                return -1;  /* Database is corrupt */
            }
        }
        sqlite3_finalize(check);
    }

    /* Create table if not exists */
    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS sizes ("
        "  path TEXT PRIMARY KEY NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  file_count INTEGER NOT NULL,"
        "  dir_mtime INTEGER NOT NULL"
        ") WITHOUT ROWID";
    if (sqlite3_exec(d_db, create_sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(d_db);
        d_db = NULL;
        return -1;
    }

    /* Prepare statements */
    const char *insert_sql =
        "INSERT OR REPLACE INTO sizes (path, size, file_count, dir_mtime) "
        "VALUES (?, ?, ?, ?)";
    if (sqlite3_prepare_v2(d_db, insert_sql, -1, &d_insert_stmt, NULL) != SQLITE_OK) {
        sqlite3_close(d_db);
        d_db = NULL;
        return -1;
    }

    const char *lookup_sql = "SELECT size, file_count, dir_mtime FROM sizes WHERE path = ?";
    if (sqlite3_prepare_v2(d_db, lookup_sql, -1, &d_lookup_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(d_insert_stmt);
        sqlite3_close(d_db);
        d_db = NULL;
        return -1;
    }

    return 0;
}

int cache_daemon_init(void) {
    char path[PATH_MAX];
    cache_get_path(path, sizeof(path));

    /* Ensure directory exists */
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/.cache/l", home ? home : "/tmp");
    mkdir(dir, 0755);

    /* Try to open existing database */
    if (cache_init_internal(path) == 0) {
        return 0;
    }

    /* Failed - delete corrupt database and retry */
    char wal_path[PATH_MAX + 8], shm_path[PATH_MAX + 8];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", path);
    unlink(path);
    unlink(wal_path);
    unlink(shm_path);

    /* Try again with fresh database */
    return cache_init_internal(path);
}

int cache_daemon_store(const char *path, off_t size, long file_count, time_t dir_mtime) {
    if (!d_db || !d_insert_stmt) return -1;

    sqlite3_reset(d_insert_stmt);
    sqlite3_bind_text(d_insert_stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(d_insert_stmt, 2, size);
    sqlite3_bind_int64(d_insert_stmt, 3, file_count);
    sqlite3_bind_int64(d_insert_stmt, 4, dir_mtime);

    return sqlite3_step(d_insert_stmt) == SQLITE_DONE ? 0 : -1;
}

const CacheEntry *cache_daemon_lookup(const char *path) {
    static CacheEntry entry;

    if (!d_db || !d_lookup_stmt) return NULL;

    sqlite3_reset(d_lookup_stmt);
    sqlite3_bind_text(d_lookup_stmt, 1, path, -1, SQLITE_STATIC);

    if (sqlite3_step(d_lookup_stmt) == SQLITE_ROW) {
        entry.size = sqlite3_column_int64(d_lookup_stmt, 0);
        entry.file_count = sqlite3_column_int64(d_lookup_stmt, 1);
        entry.dir_mtime = sqlite3_column_int64(d_lookup_stmt, 2);
        return &entry;
    }

    return NULL;
}

int cache_daemon_save(void) {
    if (!d_db) return -1;
    sqlite3_wal_checkpoint_v2(d_db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
    return 0;
}

int cache_daemon_count(void) {
    if (!d_db) return 0;
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(d_db, "SELECT COUNT(*) FROM sizes", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int cache_daemon_prune_stale(void) {
    if (!d_db) return 0;

    sqlite3_stmt *select_stmt;
    const char *select_sql = "SELECT path FROM sizes";
    if (sqlite3_prepare_v2(d_db, select_sql, -1, &select_stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    /* Collect stale paths */
    char **stale = NULL;
    int stale_count = 0;
    int stale_cap = 0;

    while (sqlite3_step(select_stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(select_stmt, 0);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            /* Path no longer exists or is not a directory */
            if (stale_count >= stale_cap) {
                stale_cap = stale_cap ? stale_cap * 2 : 64;
                stale = realloc(stale, stale_cap * sizeof(char *));
                if (!stale) break;
            }
            stale[stale_count++] = strdup(path);
        }
    }
    sqlite3_finalize(select_stmt);

    /* Delete stale entries */
    if (stale_count > 0) {
        sqlite3_stmt *delete_stmt;
        const char *delete_sql = "DELETE FROM sizes WHERE path = ?";
        if (sqlite3_prepare_v2(d_db, delete_sql, -1, &delete_stmt, NULL) == SQLITE_OK) {
            for (int i = 0; i < stale_count; i++) {
                sqlite3_reset(delete_stmt);
                sqlite3_bind_text(delete_stmt, 1, stale[i], -1, SQLITE_STATIC);
                sqlite3_step(delete_stmt);
                free(stale[i]);
            }
            sqlite3_finalize(delete_stmt);
        }
        free(stale);
    }

    return stale_count;
}

void cache_daemon_close(void) {
    if (d_insert_stmt) {
        sqlite3_finalize(d_insert_stmt);
        d_insert_stmt = NULL;
    }
    if (d_lookup_stmt) {
        sqlite3_finalize(d_lookup_stmt);
        d_lookup_stmt = NULL;
    }
    if (d_db) {
        /* Final checkpoint before close */
        sqlite3_wal_checkpoint_v2(d_db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
        sqlite3_close(d_db);
        d_db = NULL;
    }
}
