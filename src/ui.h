/*
 * ui.h - Display types and functions: columns, tree, colors
 */

#ifndef L_UI_H
#define L_UI_H

#include "common.h"
#include "icons.h"
#include "fileinfo.h"
#include "git.h"

/* ============================================================================
 * Types
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
    int long_format;
    int long_format_explicit;
    int expand_all;
    int list_mode;
    int summary_mode;
    int no_icons;
    int sort_reverse;
    int git_only;
    int show_ancestry;
    int color_all;
    int interactive;
    int is_tty;
    SortMode sort_by;
    char cwd[PATH_MAX];
    char home[PATH_MAX];
    char script_dir[PATH_MAX];
    const char *grep_pattern;
} Config;

/* File entry representing a filesystem entry */
typedef struct FileEntry {
    char *path;
    char *name;
    char *symlink_target;
    mode_t mode;
    off_t size;
    long file_count;
    time_t mtime;
    int line_count;      /* For images: megapixels * 10; for audio: duration in seconds; for PDF: page count */
    int is_image;        /* 1 if line_count holds megapixels */
    int is_audio;        /* 1 if line_count holds duration in seconds */
    int is_pdf;          /* 1 if line_count holds page count */
    int is_ignored;
    int is_git_root;     /* 1 if this directory is a git repo root */
    char git_status[3];
    int diff_added;
    int diff_removed;
    FileType type;
} FileEntry;

/* File list for directory entries */
typedef struct {
    FileEntry *entries;
    size_t count;
    size_t capacity;
} FileList;

/* Tree node for directory tree */
typedef struct TreeNode {
    FileEntry entry;
    struct TreeNode *children;
    size_t child_count;
    int has_git_status;
    int matches_grep;
    int was_expanded;  /* 1 if we attempted to show children (even if empty) */
} TreeNode;

/* Column formatter function type */
typedef void (*ColumnFormatter)(const FileEntry *fe, const Icons *icons, char *buf, size_t len);

/* Column definition */
typedef struct {
    const char *name;
    int width;
    ColumnFormatter format;
} Column;

/* Column indices */
#define NUM_COLUMNS 3
#define COL_SIZE  0
#define COL_LINES 1
#define COL_TIME  2

/* Print context passed to tree printing functions */
typedef struct {
    GitCache *git;
    const Icons *icons;
    const FileTypes *filetypes;
    const Config *cfg;
    Column *columns;
    int *continuation;
    int diff_add_width;   /* Width of insertions column (0 = hidden) */
    int diff_del_width;   /* Width of deletions column (0 = hidden) */
    const char *line_prefix;  /* Optional prefix printed at start of each line */
    int selected;             /* Is this line selected (for interactive mode) */
} PrintContext;

/* Helper macros */
#define CLR(cfg, c) ((cfg)->is_tty ? (c) : "")
#define RST(cfg)    ((cfg)->is_tty ? COLOR_RESET : "")

/* ============================================================================
 * Column Functions
 * ============================================================================ */

void columns_init(Column *cols);
void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons);
void format_size(off_t bytes, char *buf, size_t len);
void format_relative_time(time_t mtime, char *buf, size_t len);
const char *get_count_icon(const FileEntry *fe, const Icons *icons);

/* ============================================================================
 * File List Management
 * ============================================================================ */

void file_list_init(FileList *list);
void file_list_add(FileList *list, FileEntry *entry);
void file_entry_free(FileEntry *entry);
void file_list_free(FileList *list);
int read_directory(const char *dir_path, FileList *list, const Config *cfg);

/* ============================================================================
 * Tree Visibility Helpers
 * ============================================================================ */

int is_filtering_active(const Config *cfg);
int node_is_visible(const TreeNode *node, const Config *cfg);
int node_is_directory(const TreeNode *node);

/* ============================================================================
 * Tree Building and Printing
 * ============================================================================ */

void tree_node_free(TreeNode *node);
TreeNode *build_tree(const char *path, Column *cols, GitCache *git,
                     const Config *cfg, const Icons *icons);
TreeNode *build_ancestry_tree(const char *path, Column *cols, GitCache *git,
                              const Config *cfg, const Icons *icons);
int compute_git_status_flags(TreeNode *node, GitCache *git, int show_hidden);
int compute_grep_flags(TreeNode *node, const char *pattern);
void columns_recalculate_visible(Column *cols, TreeNode **trees, int tree_count,
                                 const Icons *icons, const Config *cfg);
void compute_diff_widths(TreeNode **trees, int tree_count, GitCache *gits,
                         int *add_width, int *del_width, const Config *cfg);
void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx);
void print_entry(const FileEntry *fe, int depth, int was_expanded, int has_visible_children, const PrintContext *ctx);
void print_summary(const TreeNode *node, PrintContext *ctx);

/* Dynamically expand a directory node that wasn't fully loaded */
void tree_expand_node(TreeNode *node, Column *cols, GitCache *git,
                      const Config *cfg, const Icons *icons);

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

const char *get_git_indicator(GitCache *cache, const char *path,
                              const Icons *icons, const Config *cfg);

/* ============================================================================
 * Path Resolution
 * ============================================================================ */

void resolve_source_dir(const char *argv0, char *src_dir, size_t len);

/* Wrapper functions for Config-based path operations */
void get_realpath(const char *path, char *resolved, const Config *cfg);
void get_abspath(const char *path, char *resolved, const Config *cfg);
void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg);

#endif /* L_UI_H */
