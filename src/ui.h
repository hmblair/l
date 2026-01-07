/*
 * ui.h - Display types and functions: icons, columns, tree, colors
 */

#ifndef L_UI_H
#define L_UI_H

#include "common.h"
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

typedef enum {
    FTYPE_UNKNOWN,
    FTYPE_DIR,
    FTYPE_FILE,
    FTYPE_EXEC,
    FTYPE_DEVICE,
    FTYPE_SOCKET,
    FTYPE_FIFO,
    FTYPE_SYMLINK,
    FTYPE_SYMLINK_DIR,
    FTYPE_SYMLINK_EXEC,
    FTYPE_SYMLINK_DEVICE,
    FTYPE_SYMLINK_SOCKET,
    FTYPE_SYMLINK_FIFO,
    FTYPE_SYMLINK_BROKEN
} FileType;

typedef struct {
    int max_depth;
    int show_hidden;
    int long_format;
    int long_format_explicit;
    int expand_all;
    int list_mode;
    int no_icons;
    int sort_reverse;
    int git_only;
    int show_ancestry;
    int color_all;
    int is_tty;
    SortMode sort_by;
    char cwd[PATH_MAX];
    char home[PATH_MAX];
    char script_dir[PATH_MAX];
    const char *grep_pattern;
} Config;

/* Extension icon mapping */
typedef struct {
    char ext[L_MAX_EXT_LEN];
    char icon[L_MAX_ICON_LEN];
} ExtIcon;

/* Icons configuration */
typedef struct Icons {
    char default_icon[L_MAX_ICON_LEN];
    char symlink[L_MAX_ICON_LEN];
    char symlink_dir[L_MAX_ICON_LEN];
    char symlink_exec[L_MAX_ICON_LEN];
    char symlink_file[L_MAX_ICON_LEN];
    char symlink_broken[L_MAX_ICON_LEN];
    char directory[L_MAX_ICON_LEN];
    char current_dir[L_MAX_ICON_LEN];
    char locked_dir[L_MAX_ICON_LEN];
    char executable[L_MAX_ICON_LEN];
    char device[L_MAX_ICON_LEN];
    char socket[L_MAX_ICON_LEN];
    char fifo[L_MAX_ICON_LEN];
    char file[L_MAX_ICON_LEN];
    char binary[L_MAX_ICON_LEN];
    char git_modified[L_MAX_ICON_LEN];
    char git_untracked[L_MAX_ICON_LEN];
    char git_staged[L_MAX_ICON_LEN];
    char git_deleted[L_MAX_ICON_LEN];
    char git_upstream[L_MAX_ICON_LEN];
    char readonly[L_MAX_ICON_LEN];
    char count_files[L_MAX_ICON_LEN];
    char count_lines[L_MAX_ICON_LEN];
    char count_pixels[L_MAX_ICON_LEN];
    ExtIcon ext_icons[L_MAX_EXT_ICONS];
    int ext_count;
} Icons;

/* File entry representing a filesystem entry */
typedef struct FileEntry {
    char *path;
    char *name;
    char *symlink_target;
    mode_t mode;
    off_t size;
    long file_count;
    time_t mtime;
    int line_count;      /* For images: megapixels * 10 (e.g., 125 = 12.5MP) */
    int is_image;        /* 1 if line_count holds megapixels */
    int is_ignored;
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
    const Config *cfg;
    Column *columns;
    int *continuation;
    int diff_add_width;   /* Width of insertions column (0 = hidden) */
    int diff_del_width;   /* Width of deletions column (0 = hidden) */
} PrintContext;

/* Helper macros */
#define CLR(cfg, c) ((cfg)->is_tty ? (c) : "")
#define RST(cfg)    ((cfg)->is_tty ? COLOR_RESET : "")
#define ICON_OR_FALLBACK(preferred, fallback) \
    ((preferred)[0] ? (preferred) : (fallback))

/* ============================================================================
 * Icons Functions
 * ============================================================================ */

void icons_init_defaults(Icons *icons);
void icons_load(Icons *icons, const char *script_dir);
const char *get_icon(const Icons *icons, FileType type, int is_cwd,
                     int is_locked, int is_binary, const char *name);
const char *get_ext_icon(const Icons *icons, const char *name);
const char *get_count_icon(const FileEntry *fe, const Icons *icons);

/* ============================================================================
 * Column Functions
 * ============================================================================ */

void columns_init(Column *cols);
void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons);
void format_size(off_t bytes, char *buf, size_t len);
void format_relative_time(time_t mtime, char *buf, size_t len);

/* ============================================================================
 * File Type Detection
 * ============================================================================ */

FileType detect_file_type(const char *path, struct stat *st, char **symlink_target);
const char *get_file_color(FileType type, int is_cwd, int is_ignored, const Config *cfg);

/* ============================================================================
 * Line Counting
 * ============================================================================ */

int count_file_lines(const char *path);

/* ============================================================================
 * File List Management
 * ============================================================================ */

void file_list_init(FileList *list);
void file_list_add(FileList *list, FileEntry *entry);
void file_entry_free(FileEntry *entry);
void file_list_free(FileList *list);
int read_directory(const char *dir_path, FileList *list, const Config *cfg);

/* ============================================================================
 * Tree Building and Printing
 * ============================================================================ */

void tree_node_free(TreeNode *node);
TreeNode *build_tree(const char *path, Column *cols, GitCache *git,
                     const Config *cfg, const Icons *icons);
TreeNode *build_ancestry_tree(const char *path, Column *cols, GitCache *git,
                              const Config *cfg, const Icons *icons);
int compute_git_status_flags(TreeNode *node);
int compute_grep_flags(TreeNode *node, const char *pattern);
void columns_recalculate_visible(Column *cols, TreeNode **trees, int tree_count,
                                 const Icons *icons, const Config *cfg);
void compute_diff_widths(TreeNode **trees, int tree_count, int *add_width, int *del_width,
                         const Config *cfg);
void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx);

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
