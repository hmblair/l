/*
 * tree.h - Tree data structures and building
 */

#ifndef L_TREE_H
#define L_TREE_H

#include "common.h"
#include "icons.h"
#include "fileinfo.h"
#include "git.h"

/* ============================================================================
 * Compute Options - Fine-grained control over what metadata to collect
 * ============================================================================ */

typedef struct {
    unsigned int sizes : 1;         /* Compute file/directory sizes */
    unsigned int file_counts : 1;   /* Compute file counts for directories */
    unsigned int line_counts : 1;   /* Compute line counts for text files */
    unsigned int media_info : 1;    /* Parse images/audio/PDF for metadata */
    unsigned int git_status : 1;    /* Populate git status */
    unsigned int git_diff : 1;      /* Compute git diff stats (lines added/removed) */
    unsigned int type_stats : 1;    /* Compute line breakdown by file type (dirs) */
    unsigned int git_repo_info : 1; /* Compute git repo info (branch, tag, remote) */
} ComputeOpts;

/* Preset configurations */
#define COMPUTE_NONE     ((ComputeOpts){0, 0, 0, 0, 0, 0, 0, 0})
#define COMPUTE_BASIC    ((ComputeOpts){0, 0, 0, 0, 1, 0, 0, 0})  /* git status only */
#define COMPUTE_LONG     ((ComputeOpts){1, 1, 1, 1, 1, 1, 0, 0})  /* + lines, media, diff */
#define COMPUTE_SUMMARY  ((ComputeOpts){1, 1, 1, 1, 1, 0, 1, 1})  /* + type_stats, repo_info */

/* ============================================================================
 * File Entry - Represents a single filesystem entry
 * ============================================================================ */

typedef struct FileEntry {
    /* --- Identity --- */
    char *path;                  /* Display path (may be relative) */
    char *name;                  /* Filename component */
    char *symlink_target;        /* Target if symlink, NULL otherwise */
    FileType type;               /* Detected file type (C, Python, etc.) */

    /* --- Basic metadata --- */
    mode_t mode;
    dev_t dev;                   /* Device ID (for mount boundary detection) */
    off_t size;                  /* File size or directory total */
    time_t mtime;                /* Last modification time */
    long file_count;             /* Number of files (directories only) */
    int is_mount_point;          /* 1 if on different filesystem than parent */

    /* --- Content analysis --- */
    ContentType content_type;    /* text/binary/image/etc. */
    int line_count;              /* -1 if not computed */
    int word_count;              /* -1 if not computed */

    /* --- Type statistics (directories only, requires type_stats) --- */
    TypeStats type_stats;        /* Line breakdown by file type */
    int has_type_stats;          /* 1 if type_stats is valid */

    /* --- Git file status --- */
    int is_ignored;              /* In .gitignore */
    int is_git_root;             /* Is a git repository root */
    char git_status[3];          /* Two-char status (e.g., "M ", " M") */
    int diff_added;              /* Lines added */
    int diff_removed;            /* Lines removed */

    /* --- Git directory status (aggregated from children) --- */
    GitSummary git_dir_status;   /* Aggregated status for directory contents */
    int has_git_dir_status;      /* 1 if git_dir_status is valid */

    /* --- Git repository info (git roots only, requires git_repo_info) --- */
    char *branch;                /* Current branch name */
    char *tag;                   /* Latest tag, if any */
    int tag_distance;            /* Commits since tag (0 if at tag) */
    char *remote;                /* Remote URL */
    char short_hash[8];          /* Abbreviated commit hash */
    char commit_count[32];       /* Number of commits */
    int has_upstream;            /* 1 if origin/<branch> exists */
    int out_of_sync;             /* 1 if local and remote differ */
    int ahead;                   /* Commits ahead of upstream */
    int behind;                  /* Commits behind upstream */
    GitSummary repo_status;      /* Repo-wide dirty status */
    int has_git_repo_info;       /* 1 if repo info fields are valid */
} FileEntry;

/* ============================================================================
 * File List - Dynamic array of file entries
 * ============================================================================ */

typedef struct {
    FileEntry *entries;
    size_t count;
    size_t capacity;
} FileList;

void file_list_init(FileList *list);
void file_list_add(FileList *list, FileEntry *entry);
void file_entry_free(FileEntry *entry);
void file_list_free(FileList *list);

/* ============================================================================
 * Tree Node - Recursive tree structure
 * ============================================================================ */

typedef struct TreeNode {
    FileEntry entry;
    struct TreeNode *children;
    size_t child_count;
    int has_git_status;
    int matches_grep;
    int was_expanded;
} TreeNode;

void tree_node_free(TreeNode *node);

/* ============================================================================
 * Tree Build Options
 * ============================================================================ */

typedef enum {
    SORT_NONE,
    SORT_SIZE,
    SORT_TIME,
    SORT_NAME
} SortMode;

/* Callback to skip recursion into a directory during tree building.
 * Return non-zero to skip the directory's children. */
typedef int (*tree_skip_fn)(const FileEntry *entry, void *ctx);

typedef struct {
    int max_depth;
    int show_hidden;
    int skip_gitignored;
    SortMode sort_by;
    int sort_reverse;
    const char *cwd;           /* Current working directory (for relative paths) */
    ComputeOpts compute;       /* What metadata to compute */
    tree_skip_fn skip_fn;      /* Optional: skip recursion predicate */
    void *skip_ctx;            /* Context for skip_fn */
} TreeBuildOpts;

/* ============================================================================
 * Tree Building Functions
 * ============================================================================ */

/* Read directory entries into a list */
int read_directory(const char *dir_path, FileList *list,
                   const TreeBuildOpts *opts);

/* Build a complete tree from a path */
TreeNode *build_tree(const char *path, const TreeBuildOpts *opts,
                     GitCache *git, const Icons *icons);

/* Build ancestry tree (path with parent directories) */
TreeNode *build_ancestry_tree(const char *path, const TreeBuildOpts *opts,
                              GitCache *git, const Icons *icons);

/* Expand a single node's children (lazy loading) */
void tree_expand_node(TreeNode *node, const TreeBuildOpts *opts,
                      GitCache *git, const Icons *icons);

/* ============================================================================
 * Tree Traversal Helpers
 * ============================================================================ */

/* Compute git status flags recursively (returns 1 if any node has status) */
int compute_git_status_flags(TreeNode *node, GitCache *git, int show_hidden);

/* Compute grep match flags recursively (returns 1 if any node matches) */
int compute_grep_flags(TreeNode *node, const char *pattern);


/* Check if node is a directory type */
int node_is_directory(const TreeNode *node);

#endif /* L_TREE_H */
