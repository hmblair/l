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
    struct GitPopulateRecord *populated;  /* memo of completed populates */
    struct GitRootRecord *root_lookups;   /* memo of git_find_root results */
    int aggregates_stale;                 /* a populate has run since the last rebuild */
#ifdef _OPENMP
    omp_lock_t lock;
#endif
} GitCache;

/* One completed git_populate_repo call, recording the repo and the terms it
 * ran under. A call matching a record is skipped: it would rescan the whole
 * repo to write back the statuses already in the cache. This is what keeps a
 * listing of many arguments cheap — git_populate_repo runs once per argument,
 * and the arguments usually share a single repo. */
struct GitPopulateRecord {
    char *repo_path;
    char *base;                /* NULL when reporting against HEAD */
    int include_diff_stats;
    int nested;
    struct GitPopulateRecord *next;
};

/* One resolved repository lookup, keyed by the directory the search starts
 * from. Discovering a repo opens it and parses its config from disk, which
 * costs far more than the answer is worth repeating: every listing argument
 * triggers a lookup, and arguments in one directory all resolve to the same
 * repo. root is NULL when the directory lies outside any repo. */
struct GitRootRecord {
    char *dir;
    char *root;
    struct GitRootRecord *next;
};

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

/* Per-directory roll-up of every change strictly beneath it, rebuilt by
 * git_cache_sync_aggregates once a build pass has finished populating (git
 * aggregates walk each changed entry's ancestors up to the outermost enclosing
 * repo root). Makes git_get_dir_summary an O(1) lookup instead of a full-cache
 * prefix scan. */
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
 * Returns 1 if found, 0 otherwise. Results are memoized in cache, which may be
 * NULL to look up without one. */
int git_find_root(GitCache *cache, const char *path, char *root, size_t root_len);

/* Populate cache with all file statuses from a repository.
 * If include_diff_stats is true, also populate lines added/removed. */
/* nested: the repo was discovered inside another repo's listing — its
 * populate reports on the superproject's behalf and honors the declared
 * submodule.<name>.ignore policy (untracked skipped, dirty/all not
 * populated at all). Pass 0 when the repo is the listing's own root. */
/* base: ref to report changes and diff stats against instead of HEAD (NULL
 * for normal HEAD-relative reporting). Ignored for nested repos and for
 * repos where the ref does not resolve (a forest can span repos that do not
 * share the ref; those keep HEAD-relative reporting). */
void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats, int nested, const char *base);

/* True if base names a commit in repo_path (branch, tag, or hash). */
int git_base_resolves(const char *repo_path, const char *base);

/* Rebuild the per-directory roll-ups if a populate has invalidated them. A
 * rebuild costs a walk of the whole cache, so it runs once per build pass
 * rather than once per repo; call it after building and before anything reads
 * git_get_dir_summary. */
void git_cache_sync_aggregates(GitCache *cache);

/* Get aggregated git status for all files under a directory (O(1) lookup of
 * the aggregate built by git_cache_sync_aggregates; zero summary if none) */
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
