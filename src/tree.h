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
    unsigned int sizes : 1;        /* Compute file/directory sizes */
    unsigned int file_counts : 1;  /* Compute file counts for directories */
    unsigned int line_counts : 1;  /* Compute line counts for text files */
    unsigned int media_info : 1;   /* Parse images/audio/PDF for metadata */
    unsigned int git_status : 1;   /* Populate git status */
    unsigned int git_diff : 1;     /* Compute git diff stats (lines added/removed) */
} ComputeOpts;

/* Preset configurations */
#define COMPUTE_NONE     ((ComputeOpts){0, 0, 0, 0, 0, 0})
#define COMPUTE_BASIC    ((ComputeOpts){1, 1, 0, 0, 1, 0})  /* sizes, counts, git status */
#define COMPUTE_LONG     ((ComputeOpts){1, 1, 1, 1, 1, 1})  /* everything */
#define COMPUTE_SUMMARY  ((ComputeOpts){1, 1, 1, 0, 1, 0})  /* sizes, counts, lines, git status */

/* ============================================================================
 * File Entry - Represents a single filesystem entry
 * ============================================================================ */

typedef struct FileEntry {
    char *path;
    char *name;
    char *symlink_target;
    mode_t mode;
    off_t size;
    long file_count;
    time_t mtime;
    ContentType content_type;
    int line_count;
    int is_ignored;
    int is_git_root;
    char git_status[3];
    int diff_added;
    int diff_removed;
    FileType type;
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

typedef struct {
    int max_depth;
    int show_hidden;
    int skip_gitignored;
    SortMode sort_by;
    int sort_reverse;
    const char *cwd;           /* Current working directory (for relative paths) */
    ComputeOpts compute;       /* What metadata to compute */
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
