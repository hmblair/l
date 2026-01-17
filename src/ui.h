/*
 * ui.h - Display types and functions: columns, printing, colors
 */

#ifndef L_UI_H
#define L_UI_H

#include "common.h"
#include "icons.h"
#include "tree.h"

/* ============================================================================
 * Display Configuration
 * ============================================================================ */

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
    ComputeOpts compute;        /* What metadata to compute */
} Config;

/* ============================================================================
 * Column Definitions
 * ============================================================================ */

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

/* Column functions */
void columns_init(Column *cols);
void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons);
void columns_recalculate_visible(Column *cols, TreeNode **trees, int tree_count,
                                 const Icons *icons, const Config *cfg);
void compute_diff_widths(TreeNode **trees, int tree_count, GitCache *gits,
                         int *add_width, int *del_width, const Config *cfg);

/* Formatting helpers */
void format_size(off_t bytes, char *buf, size_t len);
void format_relative_time(time_t mtime, char *buf, size_t len);
void format_count(long count, char *buf, size_t len);
const char *get_count_icon(const FileEntry *fe, const Icons *icons);

/* ============================================================================
 * Print Context
 * ============================================================================ */

typedef struct {
    GitCache *git;
    const Icons *icons;
    const FileTypes *filetypes;
    const Shebangs *shebangs;
    const Config *cfg;
    Column *columns;
    int *continuation;
    int diff_add_width;
    int diff_del_width;
    const char *line_prefix;
    int selected;
} PrintContext;

/* ============================================================================
 * Tree Visibility and Filtering
 * ============================================================================ */

int is_filtering_active(const Config *cfg);
int node_is_visible(const TreeNode *node, const Config *cfg);

/* ============================================================================
 * Printing Functions
 * ============================================================================ */

void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx);
void print_entry(const FileEntry *fe, int depth, int was_expanded,
                 int has_visible_children, const PrintContext *ctx);
void print_summary(TreeNode *node, PrintContext *ctx);

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

const char *get_git_indicator(GitCache *cache, const char *path,
                              const Icons *icons, const Config *cfg);

/* ============================================================================
 * Path Resolution Helpers
 * ============================================================================ */

void resolve_source_dir(const char *argv0, char *src_dir, size_t len);
void get_realpath(const char *path, char *resolved, const Config *cfg);
void get_abspath(const char *path, char *resolved, const Config *cfg);
void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg);

/* ============================================================================
 * Tree Building Wrappers (convenience functions using Config)
 * ============================================================================ */

/* Convert Config to TreeBuildOpts */
TreeBuildOpts config_to_build_opts(const Config *cfg);

/* Build tree using Config (wraps build_tree with TreeBuildOpts) */
TreeNode *build_tree_from_config(const char *path, Column *cols, GitCache *git,
                                  const Config *cfg, const Icons *icons);

/* Build ancestry tree using Config */
TreeNode *build_ancestry_tree_from_config(const char *path, Column *cols, GitCache *git,
                                           const Config *cfg, const Icons *icons);

/* Expand node using Config */
void tree_expand_node_from_config(TreeNode *node, Column *cols, GitCache *git,
                                   const Config *cfg, const Icons *icons);

/* Helper macros */
#define CLR(cfg, c) ((cfg)->is_tty ? (c) : "")
#define RST(cfg)    ((cfg)->is_tty ? COLOR_RESET : "")

#endif /* L_UI_H */
