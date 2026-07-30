/*
 * ui.h - Display types and functions: columns, printing, colors
 */

#ifndef L_UI_H
#define L_UI_H

#include "common.h"
#include "icons.h"
#include "tree.h"
#include "format.h"

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
    int git_only;            /* -m: show only git-changed entries, rooted at repo */
    int hide_gitignored;     /* -g: hide entries that are gitignored */
    int show_ancestry;
    int ancestry_explicit;   /* -p was given explicitly (vs. implied by -m),
                              * so anchor the ancestry at ~ (or /), not the
                              * enclosing repo root. */
    int color_all;
    int interactive;
    int dir_only;
    int is_tty;
    SortMode sort_by;
    char cwd[PATH_MAX];
    char home[PATH_MAX];
    char script_dir[PATH_MAX];
    char column_separator[L_MAX_SEPARATOR_LEN];  /* glyph drawn between long-mode columns; blank = plain spaces */
    const char *grep_pattern;
    off_t min_size;              /* Minimum size filter (0 = disabled) */
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
/* Single width-measuring pass: sizes the info and diff columns from exactly the
 * rows the renderer draws. Run after git/grep visibility flags are set. */
void measure_columns(TreeNode **trees, int tree_count, GitCache *git,
                     const Icons *icons, const Config *cfg,
                     Column *cols, int *diff_add_width, int *diff_del_width);
void diff_widths_update(int *add_width, int *del_width, const FileEntry *fe,
                        GitCache *git);

/* Resolve the diff line counts a row draws: a file's own stats, or a
 * directory's rolled-up view summary. Shared by the renderer and width pass. */
void entry_diff_stats(const FileEntry *fe, GitCache *git,
                      int *added, int *removed);

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
    int term_width;
} PrintContext;

/* ============================================================================
 * Tree Visibility and Filtering
 * ============================================================================ */

int is_filtering_active(const Config *cfg);
int node_is_visible(const TreeNode *node, const Config *cfg);
int node_is_hidden(const TreeNode *node, const Config *cfg);
int node_is_shown(const TreeNode *node, const Config *cfg,
                  int apply_content_filters, int live_filter_active);
void view_summary_remove_shown_child(GitSummary *s, const TreeNode *child, GitCache *git);
void git_summary_clamp(GitSummary *s);
void compute_view_summaries(TreeNode *node, const Config *cfg, GitCache *git);

/* ============================================================================
 * Printing Functions
 * ============================================================================ */

void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx);
void print_entry(const FileEntry *fe, int depth, int was_expanded,
                 const PrintContext *ctx);
void print_summary(TreeNode *node, PrintContext *ctx);

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

const char *get_git_indicator(GitCache *cache, const char *path,
                              const Icons *icons, const Config *cfg);

/* ============================================================================
 * Path Resolution Helpers
 * ============================================================================ */

int get_terminal_width(void);
void get_realpath(const char *path, char *resolved, const Config *cfg);
void get_abspath(const char *path, char *resolved, const Config *cfg);
void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg);

/* ============================================================================
 * Tree Building Wrappers (convenience functions using Config)
 * ============================================================================ */

/* Convert Config to TreeBuildOpts */
TreeBuildOpts config_to_build_opts(const Config *cfg);

/* Build tree using Config (wraps build_tree with TreeBuildOpts) */
TreeNode *build_tree_from_config(const char *path, GitCache *git,
                                  const Config *cfg, const Icons *icons);

/* Build ancestry tree using Config */
TreeNode *build_ancestry_tree_from_config(const char *path, GitCache *git,
                                           const Config *cfg, const Icons *icons);

/* Expand node using Config */
void tree_expand_node_from_config(TreeNode *node, GitCache *git,
                                   const Config *cfg, const Icons *icons);

/* Helper macros */
#define CLR(cfg, c) ((cfg)->is_tty ? (c) : "")
#define RST(cfg)    ((cfg)->is_tty ? COLOR_RESET : "")

#endif /* L_UI_H */
