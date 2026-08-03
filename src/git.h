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

/* Normalized git status bits. The two-char porcelain code is classified once
 * at populate time (git_flags_from_porcelain); everything downstream reads
 * these flags instead of re-parsing the code. */
enum {
    GITF_IGNORED        = 1u << 0,  /* !! */
    GITF_UNTRACKED      = 1u << 1,  /* ?? */
    GITF_STAGED         = 1u << 2,  /* index A/M/R/T/C/U */
    GITF_STAGED_DELETED = 1u << 3,  /* index D (e.g. git rm) */
    GITF_WT_MODIFIED    = 1u << 4,  /* worktree M */
    GITF_WT_DELETED     = 1u << 5,  /* worktree D */
    GITF_WT_RENAMED     = 1u << 6,  /* worktree R (no icon/summary, but "changed") */
    GITF_WT_TYPECHANGE  = 1u << 7,  /* worktree T (no icon/summary, but "changed") */
};

/* An entry counts as a change (for -m and directory roll-ups) iff it has
 * flags and is not ignored. */
#define GITF_IS_CHANGE(flags) ((flags) != 0 && !((flags) & GITF_IGNORED))

unsigned git_flags_from_porcelain(const char *status);

typedef struct GitStatusNode {
    char *path;
    unsigned flags;
    int lines_added;
    int lines_removed;
    struct GitStatusNode *next;
} GitStatusNode;

#define L_MAX_GIT_ROOTS 256

typedef struct {
    GitStatusNode *buckets[L_HASH_SIZE];
    struct GitDirAggregate *agg_buckets[L_HASH_SIZE];
    char *repo_roots[L_MAX_GIT_ROOTS];
    int repo_root_count;
#ifdef _OPENMP
    omp_lock_t lock;
#endif
} GitCache;

/* Aggregated git status for all changed files under a directory. Every field is
 * a roll-up over the directory's descendants; the view-summary pipeline (see
 * view_summary_remove_shown_child) subtracts descendants shown on their own row
 * so a directory row reports only what its subtree hides. */
typedef struct {
    int modified;
    int untracked;
    int staged;
    int deleted;        /* Unstaged (working-tree) deletions */
    int staged_deleted; /* Staged deletions (e.g. git rm) */
    int diff_added;     /* Lines added across descendants */
    int diff_removed;   /* Lines removed across descendants */
} GitSummary;

/* Per-directory roll-up of every change strictly beneath it, rebuilt from the
 * status nodes at the end of each git_populate_repo (git aggregates walk each
 * changed entry's ancestors up to the outermost enclosing repo root). Makes
 * git_get_dir_summary an O(1) lookup instead of a full-cache prefix scan. */
struct GitDirAggregate {
    char *path;
    GitSummary sum;
    struct GitDirAggregate *next;
};

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

/* Add diff stats to existing values for a cached path (accumulates) */
void git_cache_add_diff(GitCache *cache, const char *path, int added, int removed);

/* Look up normalized status flags for a path (0 if not in the cache) */
unsigned git_cache_get_flags(GitCache *cache, const char *path);

/* Look up the full status node for a path (flags + diff stats), or NULL */
GitStatusNode *git_cache_get_node(GitCache *cache, const char *path);

/* ============================================================================
 * Git Repository Functions
 * ============================================================================ */

/* Find enclosing git repo root for a path (if any)
 * Returns 1 if found, 0 otherwise */
int git_find_root(const char *path, char *root, size_t root_len);

/* Populate cache with all file statuses from a repository.
 * If include_diff_stats is true, also populate lines added/removed. */
/* nested: the repo was discovered inside another repo's listing — its
 * populate reports on the superproject's behalf and honors the declared
 * submodule.<name>.ignore policy (untracked skipped, dirty/all not
 * populated at all). Pass 0 when the repo is the listing's own root. */
void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats, int nested);

/* Get aggregated git status for all files under a directory (O(1) lookup of
 * the aggregate built at populate time; zero summary if none) */
GitSummary git_get_dir_summary(GitCache *cache, const char *dir_path);

/* Apply normalized status flags to a directory summary (+1 to add, -1 to
 * remove). Single source of truth for the flags -> bucket classification. */
void git_summary_apply_flags(GitSummary *s, unsigned flags, int sign);

/* Check if a path is inside an ignored directory (walks up ancestors) */
int git_path_in_ignored(GitCache *cache, const char *path, const char *git_root);

/* True if path is inside (or equal to) any known repo root. Containment is
 * downward-closed: once an ancestor is outside every root, all higher
 * ancestors are too. */
int git_cache_path_in_any_repo(GitCache *cache, const char *path);

/* Iterate every status node under the cache lock. cb must not call locking
 * cache functions (git_cache_path_in_any_repo is safe). */
typedef void (*git_change_cb)(const char *path, unsigned flags,
                              int lines_added, int lines_removed, void *ud);
void git_cache_foreach_change(GitCache *cache, git_change_cb cb, void *ud);

/* ============================================================================
 * Git Branch Functions
 * ============================================================================ */

/* Get current git branch for a repo root. Returns allocated string or NULL. */
char *git_get_branch(const char *repo_path);

/* Read a git ref hash from loose ref file or packed-refs
 * Returns 1 if found, 0 otherwise */
int git_read_ref(const char *repo_path, const char *ref_name, char *hash, size_t hash_len);

/* Branch info with upstream status */
typedef struct {
    char *branch;      /* Branch name (caller must free), NULL if not on branch */
    char commit[16];   /* Short hash of the branch tip, empty if unknown */
    int has_upstream;  /* 1 if origin/<branch> exists */
    int out_of_sync;   /* 1 if local and remote hashes differ */
    int ahead;         /* Commits ahead of upstream (local only) */
    int behind;        /* Commits behind upstream (local only) */
} GitBranchInfo;

/* Get branch info including upstream sync status.
 * Returns 1 if on a branch, 0 otherwise. Caller must free info->branch. */
int git_get_branch_info(const char *repo_path, GitBranchInfo *info);

/* Get the latest tag reachable from HEAD.
 * Returns allocated string or NULL. Caller must free. */
char *git_get_latest_tag(const char *repo_path);

/* Get origin remote URL by parsing .git/config directly.
 * Returns allocated string or NULL. Caller must free. */
char *git_get_remote_url(const char *repo_path);

/* Convert a git remote URL to a browser-clickable HTTPS URL.
 * Handles git@, ssh://, https://, http:// formats. Strips trailing .git.
 * Returns allocated string or NULL. Caller must free. */
char *git_remote_to_web_url(const char *remote);

/* ============================================================================
 * Shell Escape
 * ============================================================================ */

/* Escape a string for safe use inside shell single quotes: ' -> '\''
 * Required for every path or ref that reaches popen/system.
 * Returns: Newly allocated string (caller must free), or NULL on overflow. */
char *shell_escape(const char *path);

#endif /* L_GIT_H */
