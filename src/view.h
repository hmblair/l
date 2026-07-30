/*
 * view.h - The view layer: which rows a listing shows, in what order, with
 * what widths and git roll-ups.
 *
 * view_build() is the single place visibility is evaluated for static output:
 * it flattens the forest into the exact row list the renderer will draw,
 * attributes hidden git changes bottom-up to their nearest visible ancestor,
 * and measures every column width from those same rows. The renderer
 * (render.c) just iterates the rows; nothing downstream re-derives
 * visibility. The interactive picker builds its own row list (select.c) but
 * shares the primitives here (node_is_shown, width measurement,
 * git_attribute_to_view).
 */

#ifndef L_VIEW_H
#define L_VIEW_H

#include "common.h"
#include "config.h"
#include "tree.h"
#include "git.h"
#include "icons.h"

/* ============================================================================
 * Column Definitions
 * ============================================================================ */

typedef void (*ColumnFormatter)(const FileEntry *fe, const Icons *icons,
                                char *buf, size_t len);

typedef struct {
    const char *name;
    int width;
    ColumnFormatter format;
} Column;

#define NUM_COLUMNS 3
#define COL_SIZE  0
#define COL_LINES 1
#define COL_TIME  2

void columns_init(Column *cols);
void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons);
void diff_widths_update(int *add_width, int *del_width, const FileEntry *fe,
                        GitCache *git);

/* Resolve the diff line counts a row draws: a file's own stats, or a
 * directory's rolled-up view summary. Shared by the renderer and width pass. */
void entry_diff_stats(const FileEntry *fe, GitCache *git,
                      int *added, int *removed);

/* ============================================================================
 * Visibility Policy
 * ============================================================================ */

int is_filtering_active(const Config *cfg);
int node_is_visible(const TreeNode *node, const Config *cfg);
int node_is_hidden(const TreeNode *node, const Config *cfg);
int node_is_shown(const TreeNode *node, const Config *cfg,
                  int apply_content_filters, int live_filter_active);

/* ============================================================================
 * View - the flattened set of rows a listing draws
 * ============================================================================ */

typedef struct {
    TreeNode *node;
    int depth;
    int expanded;          /* drives the open/closed folder icon (static:
                            * was_expanded; interactive: ui_expanded) */
    uint64_t cont_mask;    /* bit d set: the ancestor at depth d has later
                            * siblings (drives the │ / └ tree glyphs) */
} ViewRow;

typedef struct {
    ViewRow *rows;
    size_t count;
    size_t capacity;

    int tree_count;
    size_t *tree_row_start;   /* [tree_count + 1]: tree t owns rows
                               * [start[t], start[t+1]) */
    int *tree_no_matches;     /* [tree_count]: filtering left nothing to show */

    Column cols[NUM_COLUMNS];
    int diff_add_width;
    int diff_del_width;
} View;

/* Mode parameters for view building. Zero-initialized (with filter_depth
 * INT_MAX) means the static listing; the interactive picker sets all three. */
typedef struct {
    int interactive;    /* gate recursion on node->ui_expanded; no "No
                         * matches." rows, no list-mode root suppression */
    int live_filter;    /* a '/' query is active: matches_grep filters at
                         * every depth, including tree roots */
    int filter_depth;   /* content filters (-f/-m/--min-size/...) apply only
                         * to children of nodes at depth < filter_depth;
                         * INT_MAX for static mode, the initial build depth
                         * for the picker (interactively expanded levels are
                         * always shown) */
} ViewOptions;

/* One traversal + one attribution pass + one width pass, all over the same
 * row set. Requires the git/grep visibility flags to be computed first. */
View *view_build(TreeNode **trees, int tree_count, const Config *cfg,
                 GitCache *git, const Icons *icons);
View *view_build_opts(TreeNode **trees, int tree_count, const Config *cfg,
                      GitCache *git, const Icons *icons, const ViewOptions *vo);
void view_free(View *view);

/* Bottom-up git attribution: zero every visible directory's view summary,
 * then add each cached change to its nearest visible ancestor within its
 * repo scope (entries whose own path is visible are represented by their own
 * row; entries whose walk leaves every repo root are dropped). Shared by
 * view_build and the interactive picker's re-summary pass. */
void git_attribute_to_view(GitCache *git, TreeNode *const *visible, size_t count);

/* ============================================================================
 * Tree Building Wrappers (convenience functions using Config)
 * ============================================================================ */

TreeBuildOpts config_to_build_opts(const Config *cfg);
TreeNode *build_tree_from_config(const char *path, GitCache *git,
                                  const Config *cfg, const Icons *icons);
TreeNode *build_ancestry_tree_from_config(const char *path, GitCache *git,
                                           const Config *cfg, const Icons *icons);
void tree_expand_node_from_config(TreeNode *node, GitCache *git,
                                   const Config *cfg, const Icons *icons);

/* Build all argument trees into the shared git cache and run the git-only /
 * grep visibility-flag passes — everything main() does between parsing and
 * view_build. The picker's reload key uses the same call, so reloading is
 * identical to re-running l. */
TreeNode **forest_build(char *const *dirs, int dir_count, const Config *cfg,
                        GitCache *git, const Icons *icons);
void forest_free(TreeNode **trees, int dir_count);

#endif /* L_VIEW_H */
