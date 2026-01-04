/*
 * git.h - Git status cache and retrieval
 */

#ifndef L_GIT_H
#define L_GIT_H

#include "common.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct GitStatusNode {
    char *path;
    char status[3];
    int lines_added;
    int lines_removed;
    struct GitStatusNode *next;
} GitStatusNode;

typedef struct {
    GitStatusNode *buckets[L_HASH_SIZE];
#ifdef _OPENMP
    omp_lock_t lock;
#endif
} GitCache;

/* Git status summary for directories */
typedef struct {
    int modified;
    int untracked;
    int staged;
    int deleted;
} GitSummary;

/* ============================================================================
 * GitCache Functions
 * ============================================================================ */

/* Initialize git cache */
void git_cache_init(GitCache *cache);

/* Free git cache resources */
void git_cache_free(GitCache *cache);

/* Add a status entry to the cache (thread-safe) */
void git_cache_add(GitCache *cache, const char *path, const char *status);

/* Set diff stats for a cached path */
void git_cache_set_diff(GitCache *cache, const char *path, int added, int removed);

/* Look up status for a path (returns NULL if not found) */
const char *git_cache_get(GitCache *cache, const char *path);

/* Look up diff stats for a path (returns node, or NULL if not found) */
GitStatusNode *git_cache_get_node(GitCache *cache, const char *path);

/* ============================================================================
 * Git Repository Functions
 * ============================================================================ */

/* Find enclosing git repo root for a path (if any)
 * Returns 1 if found, 0 otherwise */
int git_find_root(const char *path, char *root, size_t root_len);

/* Populate cache with all file statuses from a repository.
 * If include_diff_stats is true, also populate lines added/removed. */
void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats);

/* Get aggregated git status for all files under a directory */
GitSummary git_get_dir_summary(GitCache *cache, const char *dir_path);

/* Count deleted files directly in a directory (not recursive) */
int git_count_deleted_direct(GitCache *cache, const char *dir_path);

/* Sum deleted lines directly in a directory (not recursive) */
int git_deleted_lines_direct(GitCache *cache, const char *dir_path);

/* ============================================================================
 * Git Branch Functions
 * ============================================================================ */

/* Get current git branch for a repo root. Returns allocated string or NULL. */
char *git_get_branch(const char *repo_path);

/* Read a git ref hash from loose ref file or packed-refs
 * Returns 1 if found, 0 otherwise */
int git_read_ref(const char *repo_path, const char *ref_name, char *hash, size_t hash_len);

/* ============================================================================
 * Shell Escape (for non-libgit2 fallback)
 * ============================================================================ */

#ifndef HAVE_LIBGIT2
/* Escape a path for safe use in shell single quotes: ' -> '\''
 * Returns: Newly allocated string (caller must free), or NULL on overflow. */
char *shell_escape(const char *path);
#endif

#endif /* L_GIT_H */
