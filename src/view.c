/*
 * view.c - View layer implementation: visibility, flattening, git
 * attribution, and width measurement.
 */

#include "view.h"
#include "format.h"

/* ============================================================================
 * Column Formatters
 * ============================================================================ */

static void col_format_size(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;
    if (fe->size < 0) {
        snprintf(buf, len, "-");
    } else {
        format_size(fe->size, buf, len);
    }
}

static void col_format_lines(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;
    format_content_quantity(fe, buf, len);
}

static void col_format_time(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;
    format_relative_time(fe->mtime, buf, len);
}

void columns_init(Column *cols) {
    cols[COL_SIZE].name = "size";
    cols[COL_SIZE].width = 1;
    cols[COL_SIZE].format = col_format_size;

    cols[COL_LINES].name = "lines";
    cols[COL_LINES].width = 1;
    cols[COL_LINES].format = col_format_lines;

    cols[COL_TIME].name = "time";
    cols[COL_TIME].width = 1;
    cols[COL_TIME].format = col_format_time;
}

void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons) {
    char buf[32];
    for (int i = 0; i < NUM_COLUMNS; i++) {
        cols[i].format(fe, icons, buf, sizeof(buf));
        int len = (int)strlen(buf);
        if (len > cols[i].width) cols[i].width = len;
    }
}

static int count_digits(int n) {
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) { count = 1; n = -n; }  /* for minus sign */
    while (n > 0) { count++; n /= 10; }
    return count;
}

/* Resolve the diff line counts a row draws. A file reports its own stats; a
 * directory reports its view summary — the lines of descendants not shown on
 * their own row, attributed to this (the nearest visible) ancestor. Falls
 * back to the full recursive summary if no view was prepared. */
void entry_diff_stats(const FileEntry *fe, GitCache *git,
                      int *added, int *removed) {
    if (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) {
        const char *abs = fe->abs_path ? fe->abs_path : fe->path;
        GitSummary gs = fe->has_view_git_summary ? fe->view_git_summary
                                                 : git_get_dir_summary(git, abs);
        *added = gs.diff_added;
        *removed = gs.diff_removed;
    } else {
        *added = fe->diff_added;
        *removed = fe->diff_removed;
    }
}

/* Grow the diff-column widths to fit one entry. Directories read the same
 * rolled-up view summary the renderer draws (see entry_diff_stats), so the
 * measured width matches the printed value exactly. A resulting width of 0
 * means no entry has changes and the column is omitted (see print_entry). */
void diff_widths_update(int *add_width, int *del_width, const FileEntry *fe,
                        GitCache *git) {
    int added, removed;
    entry_diff_stats(fe, git, &added, &removed);
    if (added > 0) {
        int w = count_digits(added);
        if (w > *add_width) *add_width = w;
    }
    if (removed > 0) {
        int w = count_digits(removed);
        if (w > *del_width) *del_width = w;
    }
}

/* ============================================================================
 * Visibility Policy
 * ============================================================================ */

int is_filtering_active(const Config *cfg) {
    return cfg->req.git_only || cfg->req.hide_gitignored || cfg->req.grep_pattern ||
           cfg->req.min_size > 0 || cfg->req.dir_only;
}

int node_is_visible(const TreeNode *node, const Config *cfg) {
    if (cfg->req.git_only && !node->has_git_status) return 0;
    if (cfg->req.hide_gitignored && node->entry.is_ignored) return 0;
    if (cfg->req.grep_pattern && !node->matches_grep) return 0;
    if (cfg->req.min_size > 0 && (node->entry.size < 0 || node->entry.size < cfg->req.min_size)) return 0;
    if (cfg->req.dir_only && !node_is_directory(node)) return 0;
    return 1;
}

/* A hidden entry (dotfile) is not displayed unless -a is given. This is kept
 * separate from node_is_visible because, in the interactive picker, the content
 * filters above only apply within the initial scan depth, whereas hidden entries
 * stay hidden at every depth. */
int node_is_hidden(const TreeNode *node, const Config *cfg) {
    if (node->is_ancestor) return 0;
    return !cfg->req.show_hidden && node->entry.name[0] == '.';
}

/* The single visibility decision shared by both the static view and the
 * interactive picker: an entry is shown iff it isn't hidden (without -a), passes
 * the active content filters (-f/--filter, git-only, ...) when those apply, and
 * matches the interactive '/' query when one is active. The two flags capture
 * the only policy difference between the modes — content filters are depth-gated
 * in the picker, and the live query exists only there — so the actual rule lives
 * in exactly one place. */
int node_is_shown(const TreeNode *node, const Config *cfg,
                  int apply_content_filters, int live_filter_active) {
    /* Ancestry-spine nodes (-p) are the path to the target and always render,
     * bypassing both the hidden and content (git-only, grep, ...) filters. */
    if (node->is_ancestor) return 1;
    if (node_is_hidden(node, cfg)) return 0;
    if (apply_content_filters && !node_is_visible(node, cfg)) return 0;
    if (live_filter_active && !node->matches_grep) return 0;
    return 1;
}

/* ============================================================================
 * Git Attribution - fill view summaries bottom-up from the changes
 * ============================================================================ */

/* Small open-addressing map from canonical path to visible node, sized for
 * one attribution pass. */
typedef struct {
    const char **keys;
    TreeNode **nodes;
    size_t cap;            /* power of two */
} VisibleMap;

static const char *visible_key(const TreeNode *node) {
    return node->entry.abs_path ? node->entry.abs_path : node->entry.path;
}

static void visible_map_insert(VisibleMap *map, TreeNode *node) {
    const char *key = visible_key(node);
    size_t i = hash_string(key) & (map->cap - 1);
    while (map->keys[i]) {
        if (strcmp(map->keys[i], key) == 0) return;  /* first row wins */
        i = (i + 1) & (map->cap - 1);
    }
    map->keys[i] = key;
    map->nodes[i] = node;
}

static TreeNode *visible_map_get(const VisibleMap *map, const char *key) {
    size_t i = hash_string(key) & (map->cap - 1);
    while (map->keys[i]) {
        if (strcmp(map->keys[i], key) == 0) return map->nodes[i];
        i = (i + 1) & (map->cap - 1);
    }
    return NULL;
}

typedef struct {
    GitCache *git;
    const VisibleMap *map;
} AttributeCtx;

/* Walk one change up from its parent directory toward (and including) the
 * outermost enclosing repo root; the first visible directory absorbs it.
 * Entries shown on their own row are dropped (the row itself represents
 * them), as are entries whose walk exits every repo root — the explicit form
 * of the old in-repo query gate. */
static void attribute_change(const char *path, unsigned flags,
                             int lines_added, int lines_removed, void *ud) {
    AttributeCtx *ctx = ud;
    int has_counts = GITF_IS_CHANGE(flags);
    int has_lines = lines_added || lines_removed;
    if (!has_counts && !has_lines) return;

    if (visible_map_get(ctx->map, path)) return;  /* shown on its own row */

    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    for (;;) {
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) return;  /* reached filesystem root */
        *slash = '\0';
        if (!git_cache_path_in_any_repo(ctx->git, dir)) return;

        TreeNode *vn = visible_map_get(ctx->map, dir);
        if (vn && node_is_directory(vn)) {
            GitSummary *s = &vn->entry.view_git_summary;
            git_summary_apply_flags(s, flags, +1);
            s->diff_added += lines_added;
            s->diff_removed += lines_removed;
            return;
        }
    }
}

void git_attribute_to_view(GitCache *git, TreeNode *const *visible, size_t count) {
    /* Reset summaries and index the visible rows by canonical path. */
    size_t cap = 16;
    while (cap < count * 2) cap <<= 1;
    VisibleMap map = {
        .keys = xcalloc(cap, sizeof(char *)),
        .nodes = xcalloc(cap, sizeof(TreeNode *)),
        .cap = cap,
    };
    for (size_t i = 0; i < count; i++) {
        TreeNode *node = visible[i];
        if (node_is_directory(node)) {
            memset(&node->entry.view_git_summary, 0,
                   sizeof(node->entry.view_git_summary));
            node->entry.has_view_git_summary = 1;
        }
        visible_map_insert(&map, node);
    }

    AttributeCtx ctx = { .git = git, .map = &map };
    git_cache_foreach_change(git, attribute_change, &ctx);

    free(map.keys);
    free(map.nodes);
}

/* ============================================================================
 * View Building
 * ============================================================================ */

static void view_push(View *v, TreeNode *node, int depth, int expanded,
                      uint64_t cont_mask) {
    if (v->count >= v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 256;
        v->rows = xrealloc(v->rows, v->capacity * sizeof(ViewRow));
    }
    v->rows[v->count++] = (ViewRow){ node, depth, expanded, cont_mask };
}

/* Emit a shown node and every shown descendant, depth-first — the exact set
 * and order the renderer draws. */
static void view_emit_subtree(View *v, TreeNode *node, int depth,
                              uint64_t cont_mask, const Config *cfg,
                              const ViewOptions *vo) {
    int expanded = vo->interactive ? node->ui_expanded : node->was_expanded;
    view_push(v, node, depth, expanded, cont_mask);

    if (node->child_count == 0) return;
    if (vo->interactive && !node->ui_expanded) return;  /* collapsed */

    /* Content filters apply only within the initial depth in the picker
     * (levels the user expanded past the scan depth are always shown);
     * static mode filters at every depth (filter_depth = INT_MAX). */
    int filtering = (depth < vo->filter_depth) && is_filtering_active(cfg);

    size_t *visible_indices = xmalloc(node->child_count * sizeof(size_t));
    size_t visible_count = 0;
    for (size_t i = 0; i < node->child_count; i++) {
        if (node_is_shown(&node->children[i], cfg, filtering, vo->live_filter)) {
            visible_indices[visible_count++] = i;
        }
    }

    for (size_t vi = 0; vi < visible_count; vi++) {
        TreeNode *child = &node->children[visible_indices[vi]];
        int is_last = (vi == visible_count - 1);
        uint64_t child_mask = cont_mask;
        if (!is_last) child_mask |= 1ull << depth;
        view_emit_subtree(v, child, depth + 1, child_mask, cfg, vo);
    }

    free(visible_indices);
}

View *view_build(TreeNode **trees, int tree_count, const Config *cfg,
                 GitCache *git, const Icons *icons) {
    ViewOptions vo = { .interactive = 0, .live_filter = 0, .filter_depth = INT_MAX };
    return view_build_opts(trees, tree_count, cfg, git, icons, &vo);
}

View *view_build_opts(TreeNode **trees, int tree_count, const Config *cfg,
                      GitCache *git, const Icons *icons, const ViewOptions *vo) {
    View *v = xcalloc(1, sizeof(View));
    columns_init(v->cols);
    v->tree_count = tree_count;
    v->tree_row_start = xmalloc((tree_count + 1) * sizeof(size_t));
    v->tree_no_matches = xcalloc(tree_count, sizeof(int));

    int filtering = (0 < vo->filter_depth) && is_filtering_active(cfg);

    for (int t = 0; t < tree_count; t++) {
        v->tree_row_start[t] = v->count;
        TreeNode *root = trees[t];

        if (vo->interactive) {
            /* The picker draws every root (no "No matches." rows, no list
             * mode), except roots hidden by an active '/' query. */
            if (vo->live_filter && !root->matches_grep) continue;
            view_emit_subtree(v, root, 0, 0, cfg, vo);
            continue;
        }

        /* Filtering that leaves no visible children shows "No matches."
         * instead of a lone root row. -g/-m still shows the repo root even
         * when clean, so git-only skips this. */
        if (filtering && !cfg->req.git_only) {
            int has_visible = 0;
            for (size_t j = 0; j < root->child_count; j++) {
                const TreeNode *child = &root->children[j];
                if (node_is_hidden(child, cfg)) continue;
                if (node_is_visible(child, cfg)) {
                    has_visible = 1;
                    break;
                }
            }
            if (!has_visible) {
                v->tree_no_matches[t] = 1;
                continue;
            }
        }

        if (cfg->req.list_mode && root->entry.type == FTYPE_DIR) {
            /* List mode does not draw a directory root's own row, only its
             * shown children (each with its subtree). */
            for (size_t j = 0; j < root->child_count; j++) {
                TreeNode *child = &root->children[j];
                if (!node_is_shown(child, cfg, filtering, 0)) continue;
                view_emit_subtree(v, child, 0, 0, cfg, vo);
            }
        } else {
            view_emit_subtree(v, root, 0, 0, cfg, vo);
        }
    }
    v->tree_row_start[tree_count] = v->count;

    /* Data pass: attribute hidden git changes to their nearest visible
     * ancestor. Per tree, not globally: each argument's rows form their own
     * view, so a path listed under two arguments keeps independent
     * summaries (a row shown in one tree must not absorb changes out of
     * another tree's roll-up). */
    if (cfg->compute.git_status && v->count > 0) {
        TreeNode **visible = xmalloc(v->count * sizeof(TreeNode *));
        for (int t = 0; t < tree_count; t++) {
            size_t start = v->tree_row_start[t];
            size_t n = v->tree_row_start[t + 1] - start;
            if (n == 0) continue;
            for (size_t i = 0; i < n; i++) visible[i] = v->rows[start + i].node;
            git_attribute_to_view(git, visible, n);
        }
        free(visible);
    }

    /* Width pass over the same rows, so alignment can never drift from what
     * is printed. Reads the view summaries set above. */
    if (cfg->disp.long_format) {
        for (size_t i = 0; i < v->count; i++) {
            columns_update_widths(v->cols, &v->rows[i].node->entry, icons);
            diff_widths_update(&v->diff_add_width, &v->diff_del_width,
                               &v->rows[i].node->entry, git);
        }
    }

    return v;
}

void view_free(View *view) {
    if (!view) return;
    free(view->rows);
    free(view->tree_row_start);
    free(view->tree_no_matches);
    free(view);
}

/* ============================================================================
 * Config to TreeBuildOpts Conversion
 * ============================================================================ */

static int skip_below_min_size(const FileEntry *entry, void *ctx) {
    off_t min_size = *(off_t *)ctx;
    return entry->size < 0 || entry->size < min_size;
}

TreeBuildOpts config_to_build_opts(const Config *cfg) {
    TreeBuildOpts opts = {
        .max_depth = cfg->req.max_depth,
        .show_hidden = cfg->req.show_hidden,
        .skip_gitignored = !cfg->req.expand_all,
        .sort_by = cfg->req.sort_by,
        .sort_reverse = cfg->req.sort_reverse,
        .cwd = cfg->env.cwd,
        .compute = cfg->compute,
        .skip_fn = cfg->req.min_size > 0 ? skip_below_min_size : NULL,
        .skip_ctx = (void *)&cfg->req.min_size,
        .ancestry_to_repo = cfg->req.git_only && !cfg->req.ancestry_explicit
    };
    return opts;
}

TreeNode *build_tree_from_config(const char *path, GitCache *git,
                                  const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    return build_tree(path, &opts, git, icons);
}

TreeNode *build_ancestry_tree_from_config(const char *path, GitCache *git,
                                           const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    return build_ancestry_tree(path, &opts, git, icons);
}

void tree_expand_node_from_config(TreeNode *node, GitCache *git,
                                   const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    tree_expand_node(node, &opts, git, icons);
}

/* ============================================================================
 * Forest Building - the full data pass for a set of arguments
 * ============================================================================ */

TreeNode **forest_build(char *const *dirs, int dir_count, const Config *cfg,
                        GitCache *git, const Icons *icons) {
    TreeNode **trees = xmalloc(dir_count * sizeof(TreeNode *));

    for (int i = 0; i < dir_count; i++) {
        if (cfg->req.show_ancestry) {
            trees[i] = build_ancestry_tree_from_config(dirs[i], git, cfg, icons);
        } else {
            trees[i] = build_tree_from_config(dirs[i], git, cfg, icons);
        }
    }

    /* Pre-compute visibility flags for filtering */
    if (cfg->req.git_only) {
        for (int i = 0; i < dir_count; i++) {
            compute_git_status_flags(trees[i], git);
        }
    }
    if (cfg->req.grep_pattern) {
        for (int i = 0; i < dir_count; i++) {
            compute_grep_flags(trees[i], cfg->req.grep_pattern);
        }
    }

    return trees;
}

void forest_free(TreeNode **trees, int dir_count) {
    if (!trees) return;
    for (int i = 0; i < dir_count; i++) {
        tree_node_free(trees[i]);
        free(trees[i]);
    }
    free(trees);
}
