/*
 * cache.h - SQLite-based directory size cache API
 *
 * Uses SQLite for:
 * - ACID transactions (crash-safe)
 * - WAL mode (readers don't block writers)
 * - Automatic schema management
 */

#ifndef L_CACHE_H
#define L_CACHE_H

#include "common.h"

/* ============================================================================
 * Types
 * ============================================================================ */

/* Cache entry structure */
typedef struct {
    int64_t size;
    int64_t file_count;
} CacheEntry;

/* Directory statistics result */
typedef struct {
    off_t size;        /* Total size in bytes (-1 if error) */
    long file_count;   /* Total file count (-1 if error) */
} DirStats;

/* Cache lookup function type for dir_stats traversal */
typedef int (*dir_stats_cache_fn)(const char *path, off_t *size, long *count);

/* ============================================================================
 * Client API (for l.c) - read-only operations
 * ============================================================================ */

/* Get cache database path */
void cache_get_path(char *buf, size_t len);

/* Load cache (read-only) - returns 0 on success, -1 on failure */
int cache_load(void);

/* Look up a path in the cache (thread-safe) - returns 1 if found, 0 otherwise */
int cache_lookup(const char *path, CacheEntry *out);

/* Wrapper that returns pointer (for compatibility) */
const CacheEntry *cache_lookup_entry(const char *path);

/* Close the cache */
void cache_unload(void);

/* ============================================================================
 * Directory Statistics
 * ============================================================================ */

/*
 * Compute directory statistics (size and file count) recursively.
 * Uses OpenMP for parallel subdirectory processing.
 *
 * Parameters:
 *   path     - Directory path to scan
 *   cache_fn - Optional cache lookup function (NULL to disable)
 *
 * Returns:
 *   DirStats with size and file_count (-1 for each on error)
 */
DirStats dir_stats_get(const char *path, dir_stats_cache_fn cache_fn);

/* Cache lookup wrapper (for use with dir_stats_get) */
int cache_lookup_wrapper(const char *path, off_t *size, long *count);

/* Get directory stats with cache lookup */
DirStats get_dir_stats_cached(const char *path);

/* ============================================================================
 * Daemon API (for ld.c) - read-write operations
 *
 * The daemon writes to a temp database during each scan, then atomically
 * replaces the main database when complete. This ensures clients always
 * see a consistent snapshot.
 * ============================================================================ */

/* Create fresh temp database for a new scan - returns 0 on success */
int cache_daemon_init(void);

/* Store a cache entry - returns 0 on success */
int cache_daemon_store(const char *path, off_t size, long file_count);

/* Get entry count (for status display) */
int cache_daemon_count(void);

/* Finalize and atomically replace main database with temp */
int cache_daemon_save(void);

/* Clean up (removes temp db if save wasn't called) */
void cache_daemon_close(void);

#endif /* L_CACHE_H */
