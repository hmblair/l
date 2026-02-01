/*
 * cache_daemon.c - Daemon-side cache operations (read-write)
 *
 * Writes to a temp database during scan, then atomically replaces the
 * main database when complete. This ensures clients always see a
 * consistent snapshot.
 */

#include "cache.h"
#include <sqlite3.h>
#include <sys/stat.h>

/* ============================================================================
 * Daemon-side Cache (read-write)
 * ============================================================================ */

static sqlite3 *d_db = NULL;
static sqlite3_stmt *d_insert_stmt = NULL;
static char d_final_path[PATH_MAX];
static char d_temp_path[PATH_MAX + 8];  /* +8 for ".tmp" suffix */

int cache_daemon_init(void) {
    /* Close any existing database */
    if (d_db) {
        if (d_insert_stmt) {
            sqlite3_finalize(d_insert_stmt);
            d_insert_stmt = NULL;
        }
        sqlite3_close(d_db);
        d_db = NULL;
    }

    /* Get paths */
    cache_get_path(d_final_path, sizeof(d_final_path));
    snprintf(d_temp_path, sizeof(d_temp_path), "%s.tmp", d_final_path);

    /* Ensure directory exists */
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/.cache/l", home ? home : "/tmp");
    mkdir(dir, 0755);

    /* Remove old temp files */
    unlink(d_temp_path);
    char wal_path[PATH_MAX + 16], shm_path[PATH_MAX + 16];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", d_temp_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", d_temp_path);
    unlink(wal_path);
    unlink(shm_path);

    /* Create fresh temp database */
    if (sqlite3_open(d_temp_path, &d_db) != SQLITE_OK) {
        d_db = NULL;
        return -1;
    }

    /* Configure for performance (temp db, so less safety needed) */
    sqlite3_exec(d_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(d_db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);

    /* Create table */
    const char *create_sql =
        "CREATE TABLE sizes ("
        "  path TEXT PRIMARY KEY NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  file_count INTEGER NOT NULL"
        ") WITHOUT ROWID";
    if (sqlite3_exec(d_db, create_sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(d_db);
        d_db = NULL;
        return -1;
    }

    /* Prepare insert statement */
    const char *insert_sql =
        "INSERT INTO sizes (path, size, file_count) VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(d_db, insert_sql, -1, &d_insert_stmt, NULL) != SQLITE_OK) {
        sqlite3_close(d_db);
        d_db = NULL;
        return -1;
    }

    return 0;
}

int cache_daemon_store(const char *path, off_t size, long file_count) {
    if (!d_db || !d_insert_stmt) return -1;

    sqlite3_reset(d_insert_stmt);
    sqlite3_bind_text(d_insert_stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(d_insert_stmt, 2, size);
    sqlite3_bind_int64(d_insert_stmt, 3, file_count);

    return sqlite3_step(d_insert_stmt) == SQLITE_DONE ? 0 : -1;
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

int cache_daemon_save(void) {
    if (!d_db) return -1;

    /* Finalize statements and close database */
    if (d_insert_stmt) {
        sqlite3_finalize(d_insert_stmt);
        d_insert_stmt = NULL;
    }
    sqlite3_wal_checkpoint_v2(d_db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    sqlite3_close(d_db);
    d_db = NULL;

    /* Clean up WAL/SHM files from temp database */
    char wal_path[PATH_MAX + 16], shm_path[PATH_MAX + 16];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", d_temp_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", d_temp_path);
    unlink(wal_path);
    unlink(shm_path);

    /* Atomic replace: rename temp to final */
    if (rename(d_temp_path, d_final_path) != 0) {
        unlink(d_temp_path);
        return -1;
    }

    return 0;
}

void cache_daemon_close(void) {
    if (d_insert_stmt) {
        sqlite3_finalize(d_insert_stmt);
        d_insert_stmt = NULL;
    }
    if (d_db) {
        sqlite3_close(d_db);
        d_db = NULL;
    }
    /* Clean up temp files if save wasn't called */
    unlink(d_temp_path);
    char wal_path[PATH_MAX + 16], shm_path[PATH_MAX + 16];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", d_temp_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", d_temp_path);
    unlink(wal_path);
    unlink(shm_path);
}
