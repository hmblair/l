/*
 * ui.c - Display functions: icons, columns, tree, colors
 */

#include "ui.h"
#include "cache.h"
#include <dirent.h>
#include <ctype.h>
#include <fnmatch.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>

/* ============================================================================
 * Path Wrappers (using Config)
 * ============================================================================ */

void get_realpath(const char *path, char *resolved, const Config *cfg) {
    path_get_realpath(path, resolved, cfg->cwd);
}

void get_abspath(const char *path, char *resolved, const Config *cfg) {
    path_get_abspath(path, resolved, cfg->cwd);
}

void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg) {
    path_abbreviate_home(path, buf, len, cfg->home);
}

void resolve_source_dir(const char *argv0, char *src_dir, size_t len) {
    char exe_abs[PATH_MAX];
    char try_path[PATH_MAX];
    int found = 0;

    if (strchr(argv0, '/')) {
        if (realpath(argv0, exe_abs) != NULL) found = 1;
    } else {
        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = xstrdup(path_env);
            char *saveptr;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir) {
                snprintf(try_path, sizeof(try_path), "%s/%s", dir, argv0);
                if (access(try_path, X_OK) == 0 && realpath(try_path, exe_abs) != NULL) {
                    found = 1;
                    break;
                }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
    }

    /* 1. Check same directory as binary (development) */
    if (found) {
        char *slash = strrchr(exe_abs, '/');
        if (slash) {
            *slash = '\0';
            if (strlen(exe_abs) + sizeof("/" L_CONFIG_FILE) <= sizeof(try_path)) {
                snprintf(try_path, sizeof(try_path), "%s/%s", exe_abs, L_CONFIG_FILE);
                if (access(try_path, R_OK) == 0) {
                    strncpy(src_dir, exe_abs, len - 1);
                    src_dir[len - 1] = '\0';
                    return;
                }
            }
        }
    }

    /* 2. Check ~/.config/l/ (installed) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(try_path, sizeof(try_path), "%s/.config/l/%s", home, L_CONFIG_FILE);
        if (access(try_path, R_OK) == 0) {
            snprintf(src_dir, len, "%s/.config/l", home);
            return;
        }
    }

    /* 3. Check /usr/local/share/l/ (system-wide) */
    snprintf(try_path, sizeof(try_path), "/usr/local/share/l/%s", L_CONFIG_FILE);
    if (access(try_path, R_OK) == 0) {
        strncpy(src_dir, "/usr/local/share/l", len - 1);
        src_dir[len - 1] = '\0';
        return;
    }

    /* Fallback to current directory */
    strncpy(src_dir, ".", len - 1);
    src_dir[len - 1] = '\0';
}

/* ============================================================================
 * Size and Time Formatting
 * ============================================================================ */

void format_size(off_t bytes, char *buf, size_t len) {
    if (bytes < 0) {
        snprintf(buf, len, "-");
        return;
    }
    const char *units[] = {"B", "K", "M", "G", "T", "P"};
    int unit_idx = 0;
    double size = (double)bytes;

    while (size >= 1024 && unit_idx < 5) {
        size /= 1024;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, len, "%lld%s", (long long)bytes, units[0]);
    } else if (size < 10) {
        snprintf(buf, len, "%.1f%s", size, units[unit_idx]);
    } else {
        snprintf(buf, len, "%.0f%s", size, units[unit_idx]);
    }
}

void format_count(long count, char *buf, size_t len) {
    if (count >= 1000000) {
        double m = count / 1000000.0;
        snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
    } else if (count >= 1000) {
        double k = count / 1000.0;
        snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
    } else {
        snprintf(buf, len, "%ld", count);
    }
}

void format_relative_time(time_t mtime, char *buf, size_t len) {
    time_t now = time(NULL);
    long diff = (long)(now - mtime);

    if (diff < L_SECONDS_PER_MINUTE) {
        snprintf(buf, len, "now");
    } else if (diff < L_SECONDS_PER_HOUR) {
        snprintf(buf, len, "%ldm ago", diff / L_SECONDS_PER_MINUTE);
    } else if (diff < L_SECONDS_PER_DAY) {
        snprintf(buf, len, "%ldh ago", diff / L_SECONDS_PER_HOUR);
    } else if (diff < L_SECONDS_PER_WEEK) {
        snprintf(buf, len, "%ldd ago", diff / L_SECONDS_PER_DAY);
    } else {
        struct tm *tm = localtime(&mtime);
        strftime(buf, len, "%b %d", tm);
    }
}

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

    if (fe->file_count >= 0) {
        if (fe->file_count >= 1000000) {
            double m = fe->file_count / 1000000.0;
            snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
        } else if (fe->file_count >= 1000) {
            double k = fe->file_count / 1000.0;
            snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
        } else {
            snprintf(buf, len, "%ld", fe->file_count);
        }
    } else if (fe->content_type == CONTENT_IMAGE && fe->line_count >= 0) {
        /* line_count holds megapixels * 10 */
        double mp = fe->line_count / 10.0;
        if (mp >= 10.0) {
            snprintf(buf, len, "%.0fM", mp);
        } else {
            snprintf(buf, len, "%.1fM", mp);
        }
    } else if (fe->content_type == CONTENT_AUDIO && fe->line_count >= 0) {
        /* line_count holds duration in seconds */
        int secs = fe->line_count;
        int hours = secs / 3600;
        int mins = (secs % 3600) / 60;
        int s = secs % 60;
        if (hours > 0) {
            snprintf(buf, len, "%d:%02d:%02d", hours, mins, s);
        } else {
            snprintf(buf, len, "%d:%02d", mins, s);
        }
    } else if (fe->content_type == CONTENT_PDF && fe->line_count >= 0) {
        /* line_count holds page count */
        snprintf(buf, len, "%d", fe->line_count);
    } else if (fe->line_count >= 1000000) {
        double m = fe->line_count / 1000000.0;
        snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
    } else if (fe->line_count >= 1000) {
        double k = fe->line_count / 1000.0;
        snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
    } else if (fe->line_count >= 0) {
        snprintf(buf, len, "%d", fe->line_count);
    } else {
        snprintf(buf, len, "-");
    }
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

static void columns_reset_widths(Column *cols) {
    for (int i = 0; i < NUM_COLUMNS; i++) {
        cols[i].width = 1;
    }
}

int is_filtering_active(const Config *cfg) {
    return cfg->git_only || cfg->grep_pattern;
}

int node_is_visible(const TreeNode *node, const Config *cfg) {
    if (cfg->git_only && !node->has_git_status) return 0;
    if (cfg->grep_pattern && !node->matches_grep) return 0;
    return 1;
}

static void columns_update_visible_recursive(Column *cols, const TreeNode *node,
                                             const Icons *icons, const Config *cfg) {
    if (node_is_visible(node, cfg)) {
        columns_update_widths(cols, &node->entry, icons);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        columns_update_visible_recursive(cols, &node->children[i], icons, cfg);
    }
}

void columns_recalculate_visible(Column *cols, TreeNode **trees, int tree_count,
                                 const Icons *icons, const Config *cfg) {
    columns_reset_widths(cols);
    for (int i = 0; i < tree_count; i++) {
        columns_update_visible_recursive(cols, trees[i], icons, cfg);
    }
}

static int count_digits(int n) {
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) { count = 1; n = -n; }  /* for minus sign */
    while (n > 0) { count++; n /= 10; }
    return count;
}

static void compute_diff_widths_recursive(const TreeNode *node, GitCache *git,
                                          int *add_width, int *del_width,
                                          const Config *cfg) {
    if (node_is_visible(node, cfg)) {
        if (node->entry.diff_added > 0) {
            int w = count_digits(node->entry.diff_added);
            if (w > *add_width) *add_width = w;
        }
        /* For directories, use recursive count (maximum possible value) */
        int diff_removed = node->entry.diff_removed;
        if (node->entry.type == FTYPE_DIR || node->entry.type == FTYPE_SYMLINK_DIR) {
            int recursive = git_deleted_lines_recursive(git, node->entry.path);
            if (recursive > diff_removed) diff_removed = recursive;
        }
        if (diff_removed > 0) {
            int w = count_digits(diff_removed);
            if (w > *del_width) *del_width = w;
        }
    }
    for (size_t i = 0; i < node->child_count; i++) {
        compute_diff_widths_recursive(&node->children[i], git, add_width, del_width, cfg);
    }
}

void compute_diff_widths(TreeNode **trees, int tree_count, GitCache *gits,
                         int *add_width, int *del_width, const Config *cfg) {
    *add_width = 0;
    *del_width = 0;
    for (int i = 0; i < tree_count; i++) {
        compute_diff_widths_recursive(trees[i], &gits[i], add_width, del_width, cfg);
    }
}

const char *get_count_icon(const FileEntry *fe, const Icons *icons) {
    if (fe->file_count >= 0) {
        return icons->count_files;
    } else if (fe->content_type == CONTENT_IMAGE && fe->line_count >= 0) {
        return icons->count_pixels;
    } else if (fe->content_type == CONTENT_AUDIO && fe->line_count >= 0) {
        return icons->count_duration;
    } else if (fe->content_type == CONTENT_PDF && fe->line_count >= 0) {
        return icons->count_pages;
    } else if (fe->line_count >= 0) {
        return icons->count_lines;
    }
    return "";
}



/* ============================================================================
 * Config to TreeBuildOpts Conversion
 * ============================================================================ */

static void columns_update_widths_recursive(Column *cols, const TreeNode *node,
                                            const Icons *icons) {
    columns_update_widths(cols, &node->entry, icons);
    for (size_t i = 0; i < node->child_count; i++) {
        columns_update_widths_recursive(cols, &node->children[i], icons);
    }
}

TreeBuildOpts config_to_build_opts(const Config *cfg) {
    TreeBuildOpts opts = {
        .max_depth = cfg->max_depth,
        .show_hidden = cfg->show_hidden,
        .skip_gitignored = 0,
        .sort_by = cfg->sort_by,
        .sort_reverse = cfg->sort_reverse,
        .cwd = cfg->cwd,
        .compute = cfg->compute
    };
    return opts;
}

TreeNode *build_tree_from_config(const char *path, Column *cols, GitCache *git,
                                  const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    TreeNode *tree = build_tree(path, &opts, git, icons);

    /* Update column widths if in long format */
    if (cfg->long_format && cols && tree) {
        columns_update_widths_recursive(cols, tree, icons);
    }

    return tree;
}

TreeNode *build_ancestry_tree_from_config(const char *path, Column *cols, GitCache *git,
                                           const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    TreeNode *tree = build_ancestry_tree(path, &opts, git, icons);

    /* Update column widths if in long format */
    if (cfg->long_format && cols && tree) {
        columns_update_widths_recursive(cols, tree, icons);
    }

    return tree;
}

void tree_expand_node_from_config(TreeNode *node, Column *cols, GitCache *git,
                                   const Config *cfg, const Icons *icons) {
    TreeBuildOpts opts = config_to_build_opts(cfg);
    tree_expand_node(node, &opts, git, icons);

    /* Update column widths for newly expanded children */
    if (cfg->long_format && cols) {
        for (size_t i = 0; i < node->child_count; i++) {
            columns_update_widths(cols, &node->children[i].entry, icons);
        }
    }
}

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

static char *append_git_icon(char *buf, size_t *remaining,
                             const char *icon, const char *color, const Config *cfg) {
    int written = snprintf(buf, *remaining, "%s%s%s ", color, icon, RST(cfg));
    *remaining -= written;
    return buf + written;
}

const char *get_git_indicator(GitCache *cache, const char *path,
                              const Icons *icons, const Config *cfg) {
    static __thread char indicator[L_GIT_INDICATOR_SIZE];
    indicator[0] = '\0';

    if (cfg->no_icons) return indicator;

    const char *status = git_cache_get(cache, path);
    if (!status || strcmp(status, "!!") == 0) return indicator;

    char *p = indicator;
    size_t remaining = sizeof(indicator);

    if (strcmp(status, "??") == 0) {
        append_git_icon(p, &remaining, icons->git_untracked, CLR(cfg, COLOR_RED), cfg);
    } else {
        if (status[1] == 'M') {
            p = append_git_icon(p, &remaining, icons->git_modified, CLR(cfg, COLOR_RED), cfg);
        } else if (status[1] == 'D') {
            p = append_git_icon(p, &remaining, icons->git_deleted, CLR(cfg, COLOR_RED), cfg);
        }
        if (status[0] != ' ' && status[0] != '?' && status[0] != '!') {
            const char *staged_icon = (status[0] == 'D') ? icons->git_deleted : icons->git_staged;
            append_git_icon(p, &remaining, staged_icon, CLR(cfg, COLOR_YELLOW), cfg);
        }
    }

    return indicator;
}


/* ============================================================================
 * Tree Printing
 * ============================================================================ */

static void print_prefix(int depth, int *continuation, const Config *cfg) {
    if (cfg->list_mode) return;

    if (!cfg->is_tty) {
        for (int i = 0; i < depth; i++) {
            printf("  ");
        }
        return;
    }

    printf("%s", COLOR_GREY);
    for (int i = 0; i < depth - 1; i++) {
        printf("%s", continuation[i] ? TREE_VERT : TREE_SPACE);
    }
    if (depth > 0) {
        printf("%s", continuation[depth - 1] ? TREE_BRANCH : TREE_LAST);
    }
    printf("%s", COLOR_RESET);
}

void print_entry(const FileEntry *fe, int depth, int was_expanded, int has_visible_children, const PrintContext *ctx) {
    char abs_path[PATH_MAX];
    get_realpath(fe->path, abs_path, ctx->cfg);

    int is_cwd = (strcmp(abs_path, ctx->cfg->cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    /* Print optional line prefix (used for interactive selection cursor) */
    if (ctx->line_prefix) {
        printf("%s", ctx->line_prefix);
    }

    if (ctx->cfg->long_format && ctx->columns) {
        char buf[32];
        for (int i = 0; i < NUM_COLUMNS; i++) {
            ctx->columns[i].format(fe, ctx->icons, buf, sizeof(buf));
            printf("%s%*s%s", CLR(ctx->cfg, COLOR_GREY), ctx->columns[i].width, buf, RST(ctx->cfg));
            if (i == COL_LINES) {
                const char *count_icon = get_count_icon(fe, ctx->icons);
                if (count_icon[0]) {
                    printf(" %s%s%s", CLR(ctx->cfg, COLOR_GREY), count_icon, RST(ctx->cfg));
                } else {
                    printf("  ");
                }
            }
            printf("  ");
        }
        /* Diff columns (only shown when there are diffs) */
        if (ctx->diff_add_width > 0) {
            if (fe->diff_added > 0) {
                printf("%s%*d%s ", CLR(ctx->cfg, COLOR_GREEN),
                       ctx->diff_add_width, fe->diff_added, RST(ctx->cfg));
            } else {
                printf("%s%*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_add_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_del_width > 0) {
            /* For directories, compute deleted lines based on collapsed/expanded state */
            int diff_removed = fe->diff_removed;
            if (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) {
                if (has_visible_children) {
                    diff_removed = git_deleted_lines_direct(ctx->git, abs_path);
                } else {
                    diff_removed = git_deleted_lines_recursive(ctx->git, abs_path);
                }
            }

            if (diff_removed > 0) {
                printf("%s%-*d%s ", CLR(ctx->cfg, COLOR_RED),
                       ctx->diff_del_width, diff_removed, RST(ctx->cfg));
            } else {
                printf("%s%-*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_del_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_add_width > 0 || ctx->diff_del_width > 0) {
            printf(" ");
        }
    }

    print_prefix(depth, ctx->continuation, ctx->cfg);

    int is_readonly = (access(fe->path, W_OK) != 0 && access(fe->path, R_OK) == 0);
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    if (!ctx->cfg->no_icons && is_readonly && !is_dir) {
        printf("%s%s%s ", CLR(ctx->cfg, COLOR_YELLOW), ctx->icons->readonly, RST(ctx->cfg));
    }

    if (is_dir && !has_visible_children && !ctx->cfg->no_icons) {
        /* Collapsed directory: show full recursive summary */
        GitSummary gs = git_get_dir_summary(ctx->git, abs_path);
        if (gs.modified) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
        }
        if (gs.untracked) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
        }
        if (gs.staged) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
        }
        if (gs.deleted) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
        }
    } else if (is_dir && has_visible_children && !ctx->cfg->no_icons) {
        /* Expanded directory: show directly deleted files count */
        int deleted_direct = git_count_deleted_direct(ctx->git, abs_path);
        if (deleted_direct) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), deleted_direct, ctx->icons->git_deleted, RST(ctx->cfg));
        }
        /* Also show hidden file status if hidden files aren't shown */
        if (!ctx->cfg->show_hidden) {
            GitSummary gs = git_get_hidden_dir_summary(ctx->git, abs_path);
            if (gs.modified) {
                printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
            }
            if (gs.untracked) {
                printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
            }
            if (gs.staged) {
                printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
            }
            /* Note: deleted already counted above in deleted_direct if visible,
             * but hidden deleted files need separate handling */
            if (gs.deleted && !deleted_direct) {
                printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
            }
        }
    } else {
        const char *git_ind = get_git_indicator(ctx->git, abs_path, ctx->icons, ctx->cfg);
        printf("%s", git_ind);
    }

    int is_locked = (fe->type == FTYPE_DIR && (fe->size < 0 || is_readonly));
    const char *color = (fe->type == FTYPE_DIR && fe->size < 0) ? CLR(ctx->cfg, COLOR_RED) :
                        get_file_color(fe->type, is_cwd, fe->is_ignored, ctx->cfg->is_tty, ctx->cfg->color_all);
    const char *style = is_hidden ? CLR(ctx->cfg, STYLE_ITALIC) : "";

    if (!ctx->cfg->no_icons) {
        int is_binary = (fe->file_count < 0 && fe->line_count == -1);
        int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
        int is_expanded = is_dir ? was_expanded : 0;
        printf("%s%s%s ", color, get_icon(ctx->icons, fe->type, is_expanded, is_locked, is_binary, fe->name), RST(ctx->cfg));
    }

    const char *bold = ctx->selected ? CLR(ctx->cfg, STYLE_BOLD) : "";
    if (ctx->cfg->list_mode) {
        char abbrev[PATH_MAX];
        abbreviate_home(abs_path, abbrev, sizeof(abbrev), ctx->cfg);
        printf("%s%s%s%s%s", color, bold, style, abbrev, RST(ctx->cfg));
    } else {
        printf("%s%s%s%s%s", color, bold, style, fe->name, RST(ctx->cfg));
    }

    if (is_dir && fe->is_git_root) {
        char *branch = git_get_branch(fe->path);
        if (branch) {
            char local_hash[64], remote_hash[64];
            char local_ref[128], remote_ref[128];
            snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
            snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);
            git_read_ref(fe->path, local_ref, local_hash, sizeof(local_hash));
            int has_upstream = git_read_ref(fe->path, remote_ref, remote_hash, sizeof(remote_hash));
            if (has_upstream) {
                int out_of_sync = strcmp(local_hash, remote_hash) != 0;
                const char *cloud_color = out_of_sync ? COLOR_RED : COLOR_GREY;
                printf(" %s%s%s%s %s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), branch, RST(ctx->cfg), CLR(ctx->cfg, cloud_color), ctx->icons->git_upstream, RST(ctx->cfg));
            } else {
                printf(" %s%s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), branch, RST(ctx->cfg));
            }
            free(branch);
        }
    }

    if (fe->symlink_target) {
        char abbrev[PATH_MAX];
        abbreviate_home(fe->symlink_target, abbrev, sizeof(abbrev), ctx->cfg);
        const char *target_base = strrchr(fe->symlink_target, '/');
        target_base = target_base ? target_base + 1 : fe->symlink_target;
        const char *target_style = (target_base[0] == '.') ? CLR(ctx->cfg, STYLE_ITALIC) : "";
        printf(" %s %s%s%s%s", ctx->icons->symlink, color, target_style, abbrev, RST(ctx->cfg));
    }

    printf("\n");
}

static void print_tree_children(const TreeNode *parent, int depth, PrintContext *ctx);

void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx) {
    int filtering = is_filtering_active(ctx->cfg);

    if (ctx->cfg->list_mode && node->entry.type == FTYPE_DIR) {
        size_t visible_count = 0;
        size_t *visible_indices = NULL;

        if (filtering) {
            visible_indices = xmalloc(node->child_count * sizeof(size_t));
            for (size_t i = 0; i < node->child_count; i++) {
                if (node_is_visible(&node->children[i], ctx->cfg)) {
                    visible_indices[visible_count++] = i;
                }
            }
        } else {
            visible_count = node->child_count;
        }

        for (size_t vi = 0; vi < visible_count; vi++) {
            size_t i = filtering ? visible_indices[vi] : vi;
            const TreeNode *child = &node->children[i];
            int is_last = (vi == visible_count - 1);
            if (depth > 0) ctx->continuation[depth - 1] = !is_last;

            int has_visible_children = 0;
            if (child->child_count > 0) {
                if (filtering) {
                    for (size_t j = 0; j < child->child_count; j++) {
                        if (node_is_visible(&child->children[j], ctx->cfg)) {
                            has_visible_children = 1;
                            break;
                        }
                    }
                } else {
                    has_visible_children = 1;
                }
            }

            print_entry(&child->entry, depth, child->was_expanded, has_visible_children, ctx);

            if (child->child_count > 0) {
                print_tree_children(child, depth, ctx);
            }
        }
        free(visible_indices);
        return;
    }

    int has_visible_children = 0;
    if (node->child_count > 0) {
        if (filtering) {
            for (size_t i = 0; i < node->child_count; i++) {
                if (node_is_visible(&node->children[i], ctx->cfg)) {
                    has_visible_children = 1;
                    break;
                }
            }
        } else {
            has_visible_children = 1;
        }
    }

    print_entry(&node->entry, depth, node->was_expanded, has_visible_children, ctx);

    if (node->child_count > 0) {
        print_tree_children(node, depth, ctx);
    }
}

static void print_tree_children(const TreeNode *parent, int depth, PrintContext *ctx) {
    int filtering = is_filtering_active(ctx->cfg);
    size_t visible_count = 0;
    size_t *visible_indices = NULL;

    if (filtering) {
        visible_indices = xmalloc(parent->child_count * sizeof(size_t));
        for (size_t i = 0; i < parent->child_count; i++) {
            if (node_is_visible(&parent->children[i], ctx->cfg)) {
                visible_indices[visible_count++] = i;
            }
        }
    } else {
        visible_count = parent->child_count;
    }

    for (size_t vi = 0; vi < visible_count; vi++) {
        size_t i = filtering ? visible_indices[vi] : vi;
        const TreeNode *child = &parent->children[i];
        int is_last = (vi == visible_count - 1);

        ctx->continuation[depth] = !is_last;

        int has_visible_children = 0;
        if (child->child_count > 0) {
            if (filtering) {
                for (size_t j = 0; j < child->child_count; j++) {
                    if (node_is_visible(&child->children[j], ctx->cfg)) {
                        has_visible_children = 1;
                        break;
                    }
                }
            } else {
                has_visible_children = 1;
            }
        }

        print_entry(&child->entry, depth + 1, child->was_expanded, has_visible_children, ctx);

        if (child->child_count > 0) {
            print_tree_children(child, depth + 1, ctx);
        }
    }

    free(visible_indices);
}

/* ============================================================================
 * Summary Mode
 * ============================================================================ */

/* Print summary for a single file or directory */
void print_summary(const TreeNode *node, PrintContext *ctx) {
    const FileEntry *fe = &node->entry;
    const Config *cfg = ctx->cfg;
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    int is_cwd = (strcmp(fe->path, cfg->cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    /* First line: match short mode output (icon + name + git branch for repos) */
    int is_locked = (fe->type == FTYPE_DIR && access(fe->path, R_OK) != 0);
    const char *color = get_file_color(fe->type, is_cwd, fe->is_ignored, cfg->is_tty, cfg->color_all);
    const char *style = is_hidden ? CLR(cfg, STYLE_ITALIC) : "";

    /* Git status summary for directories (only if inside a git repo) */
    if (is_dir && !cfg->no_icons) {
        char abs_path[PATH_MAX];
        get_realpath(fe->path, abs_path, cfg);
        char git_root[PATH_MAX];
        /* Only show git status if this directory is inside a git repo */
        if (git_find_root(abs_path, git_root, sizeof(git_root))) {
            GitSummary gs = git_get_dir_summary(ctx->git, abs_path);
            if (gs.modified) {
                printf("%s%d %s%s ", CLR(cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(cfg));
            }
            if (gs.untracked) {
                printf("%s%d %s%s ", CLR(cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(cfg));
            }
            if (gs.staged) {
                printf("%s%d %s%s ", CLR(cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(cfg));
            }
            if (gs.deleted) {
                printf("%s%d %s%s ", CLR(cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(cfg));
            }
        }
    }

    if (!cfg->no_icons) {
        int is_binary = (fe->file_count < 0 && fe->line_count == -1);
        printf("%s%s%s ", color, get_icon(ctx->icons, fe->type, node->was_expanded, is_locked, is_binary, fe->name), RST(cfg));
    }
    printf("%s%s%s%s", color, style, fe->name, RST(cfg));

    /* Git branch and upstream for git repo roots */
    if (is_dir && fe->is_git_root) {
        char *branch = git_get_branch(fe->path);
        if (branch) {
            char local_hash[64], remote_hash[64];
            char local_ref[128], remote_ref[128];
            snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
            snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);
            git_read_ref(fe->path, local_ref, local_hash, sizeof(local_hash));
            int has_upstream = git_read_ref(fe->path, remote_ref, remote_hash, sizeof(remote_hash));
            if (has_upstream) {
                int out_of_sync = strcmp(local_hash, remote_hash) != 0;
                const char *cloud_color = out_of_sync ? COLOR_RED : COLOR_GREY;
                printf(" %s%s%s%s %s%s%s", CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), branch, RST(cfg),
                       CLR(cfg, cloud_color), ctx->icons->git_upstream, RST(cfg));
            } else {
                printf(" %s%s%s%s", CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), branch, RST(cfg));
            }
            free(branch);
        }
    }
    printf("\n");

    /* Full path (show both symlink and target for symlinks) */
    if (fe->symlink_target) {
        char link_path[PATH_MAX];
        get_abspath(fe->path, link_path, cfg);
        printf("   %sPath:%s     %s\n", CLR(cfg, COLOR_GREY), RST(cfg), link_path);
        printf("   %sTarget:%s   %s\n", CLR(cfg, COLOR_GREY), RST(cfg), fe->symlink_target);
    } else {
        char full_path[PATH_MAX];
        get_realpath(fe->path, full_path, cfg);
        printf("   %sPath:%s     %s\n", CLR(cfg, COLOR_GREY), RST(cfg), full_path);
    }

    /* Type (files only) */
    if (!is_dir) {
        const char *type_name = get_file_type_name(fe->path, ctx->filetypes, ctx->shebangs);
        if (type_name) {
            printf("   %sType:%s     %s\n", CLR(cfg, COLOR_GREY), RST(cfg), type_name);
        }
    }

    /* Size */
    char size_buf[32];
    format_size(fe->size, size_buf, sizeof(size_buf));
    printf("   %sSize:%s     %s\n", CLR(cfg, COLOR_GREY), RST(cfg), size_buf);

    /* File count (directories only) */
    if (is_dir && fe->file_count >= 0) {
        char count_buf[32];
        format_count(fe->file_count, count_buf, sizeof(count_buf));
        printf("   %sFiles:%s    %s\n", CLR(cfg, COLOR_GREY), RST(cfg), count_buf);
    }

    /* Line count */
    if (is_dir) {
        /* Compute type stats from the already-built tree */
        TypeStats stats;
        type_stats_from_tree(&stats, node, ctx->filetypes, ctx->shebangs, cfg->show_hidden);
        if (stats.total_lines > 0) {
            char total_buf[32];
            format_count(stats.total_lines, total_buf, sizeof(total_buf));
            printf("   %sLines:%s    %s\n", CLR(cfg, COLOR_GREY), RST(cfg), total_buf);
            /* Sort by line count descending */
            type_stats_sort(&stats);
            /* Calculate alignment widths */
            int max_name_len = 0;
            int max_count_len = 0;
            for (int i = 0; i < stats.count; i++) {
                if (!stats.entries[i].has_lines) continue;
                int len = (int)strlen(stats.entries[i].name);
                if (len > max_name_len) max_name_len = len;
                char tmp[32];
                format_count(stats.entries[i].line_count, tmp, sizeof(tmp));
                int clen = (int)strlen(tmp);
                if (clen > max_count_len) max_count_len = clen;
            }
            /* Show breakdown (text files with line counts only) */
            for (int i = 0; i < stats.count; i++) {
                if (!stats.entries[i].has_lines) continue;
                char line_buf[32];
                format_count(stats.entries[i].line_count, line_buf, sizeof(line_buf));
                printf("     %s%s:%s %*s\n", CLR(cfg, COLOR_GREY),
                       stats.entries[i].name, RST(cfg),
                       (int)(max_name_len - strlen(stats.entries[i].name) + max_count_len + 1),
                       line_buf);
            }
        }
    } else if (fe->line_count > 0 && fe->content_type == CONTENT_TEXT) {
        char line_buf[32];
        format_count(fe->line_count, line_buf, sizeof(line_buf));
        printf("   %sLines:%s    %s\n", CLR(cfg, COLOR_GREY), RST(cfg), line_buf);
    }

    /* Modified time */
    char time_buf[64];
    struct tm *tm = localtime(&fe->mtime);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm);
    printf("   %sModified:%s %s\n", CLR(cfg, COLOR_GREY), RST(cfg), time_buf);

    /* Git info (for git repositories) */
    char git_root[PATH_MAX];
    if (is_dir && git_find_root(fe->path, git_root, sizeof(git_root)) &&
        strcmp(fe->path, git_root) == 0) {

        printf("\n");

        /* Branch */
        char *branch = git_get_branch(git_root);
        if (branch) {
            /* Get commit hash */
            char hash[64] = "";
            char ref[128];
            snprintf(ref, sizeof(ref), "refs/heads/%s", branch);
            git_read_ref(git_root, ref, hash, sizeof(hash));
            hash[7] = '\0';  /* Short hash */

            printf("   %sBranch:%s   %s %s(%s)%s\n", CLR(cfg, COLOR_GREY), RST(cfg),
                   branch, CLR(cfg, COLOR_GREY), hash, RST(cfg));
            free(branch);
        }

        /* Commit count */
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd), "git -C '%s' rev-list --count HEAD 2>/dev/null", git_root);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\n")] = '\0';
                long count = atol(buf);
                if (count > 0) {
                    char count_buf[32];
                    format_count(count, count_buf, sizeof(count_buf));
                    printf("   %sCommits:%s  %s\n", CLR(cfg, COLOR_GREY), RST(cfg), count_buf);
                }
            }
            pclose(fp);
        }

        /* Latest tag */
        snprintf(cmd, sizeof(cmd), "git -C '%s' describe --tags --abbrev=0 2>/dev/null", git_root);
        fp = popen(cmd, "r");
        if (fp) {
            char buf[128];
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\n")] = '\0';
                if (buf[0]) {
                    printf("   %sTag:%s      %s\n", CLR(cfg, COLOR_GREY), RST(cfg), buf);
                }
            }
            pclose(fp);
        }

        /* Remote URL */
        snprintf(cmd, sizeof(cmd), "git -C '%s' remote get-url origin 2>/dev/null", git_root);
        fp = popen(cmd, "r");
        if (fp) {
            char buf[512];
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\n")] = '\0';
                if (buf[0]) {
                    printf("   %sRemote:%s   %s\n", CLR(cfg, COLOR_GREY), RST(cfg), buf);
                }
            }
            pclose(fp);
        }

        /* Dirty status */
        GitSummary summary = git_get_dir_summary(ctx->git, git_root);
        if (summary.modified || summary.untracked || summary.staged || summary.deleted) {
            printf("   %sStatus:%s   ", CLR(cfg, COLOR_GREY), RST(cfg));
            int first = 1;
            if (summary.staged) {
                printf("%s%d staged%s", CLR(cfg, COLOR_GREEN), summary.staged, RST(cfg));
                first = 0;
            }
            if (summary.modified) {
                printf("%s%d modified%s", first ? "" : ", ", summary.modified, RST(cfg));
                first = 0;
            }
            if (summary.deleted) {
                printf("%s%d deleted%s", first ? "" : ", ", summary.deleted, RST(cfg));
                first = 0;
            }
            if (summary.untracked) {
                printf("%s%d untracked%s", first ? "" : ", ", summary.untracked, RST(cfg));
            }
            printf("\n");
        }
    }

    printf("\n");
}
