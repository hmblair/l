/*
 * scan.h - Shared directory scanning with parallel processing
 */

#ifndef SCAN_H
#define SCAN_H

#include <sys/types.h>

typedef struct {
    off_t size;
    long file_count;
} ScanResult;

/* Callback to store results for a directory (can be NULL) */
typedef void (*scan_store_fn)(const char *path, off_t size, long count);

/* Scan a directory tree and return total size/count.
 * Uses OMP task-based parallelism for speed.
 *
 * store_fn: called for each directory meeting threshold (can be NULL)
 * shutdown: pointer to flag checked for early termination (can be NULL)
 * threshold: minimum file count to trigger store_fn (0 = store all)
 */
ScanResult scan_directory(const char *path,
                          scan_store_fn store_fn,
                          volatile int *shutdown,
                          long threshold);

#endif /* SCAN_H */
