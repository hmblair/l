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
#include <stdarg.h>
#include <sys/ioctl.h>

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
    return cfg->git_only || cfg->grep_pattern || cfg->min_size > 0;
}

int node_is_visible(const TreeNode *node, const Config *cfg) {
    if (cfg->git_only && !node->has_git_status) return 0;
    if (cfg->grep_pattern && !node->matches_grep) return 0;
    if (cfg->min_size > 0 && (node->entry.size < 0 || node->entry.size < cfg->min_size)) return 0;
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

static int skip_below_min_size(const FileEntry *entry, void *ctx) {
    off_t min_size = *(off_t *)ctx;
    return entry->size < 0 || entry->size < min_size;
}

TreeBuildOpts config_to_build_opts(const Config *cfg) {
    TreeBuildOpts opts = {
        .max_depth = cfg->max_depth,
        .show_hidden = cfg->show_hidden,
        .skip_gitignored = !cfg->expand_all,
        .sort_by = cfg->sort_by,
        .sort_reverse = cfg->sort_reverse,
        .cwd = cfg->cwd,
        .compute = cfg->compute,
        .skip_fn = cfg->min_size > 0 ? skip_below_min_size : NULL,
        .skip_ctx = (void *)&cfg->min_size
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

/* Forward declarations for string utilities defined later */
static int visible_strlen(const char *s);
static char *truncate_visible(const char *s, int max_visible_len);

/* Buffer size for print_entry line assembly */
#define ENTRY_BUF_SIZE 8192

/* Append formatted text to a buffer, advancing pos. Silently stops if full. */
#define EMIT(buf, pos, size, ...) do { \
    int _n = snprintf((buf) + (pos), (size) - (pos), __VA_ARGS__); \
    if (_n > 0 && (pos) + _n < (int)(size)) (pos) += _n; \
    else if (_n > 0) (pos) = (int)(size) - 1; \
} while (0)

static void emit_prefix(char *buf, int *pos, int size, int depth, int *continuation, const Config *cfg) {
    if (cfg->list_mode) return;

    if (!cfg->is_tty) {
        for (int i = 0; i < depth; i++) {
            EMIT(buf, *pos, size, "  ");
        }
        return;
    }

    EMIT(buf, *pos, size, "%s", COLOR_GREY);
    for (int i = 0; i < depth - 1; i++) {
        EMIT(buf, *pos, size, "%s", continuation[i] ? TREE_VERT : TREE_SPACE);
    }
    if (depth > 0) {
        EMIT(buf, *pos, size, "%s", continuation[depth - 1] ? TREE_BRANCH : TREE_LAST);
    }
    EMIT(buf, *pos, size, "%s", COLOR_RESET);
}

void print_entry(const FileEntry *fe, int depth, int was_expanded, int has_visible_children, const PrintContext *ctx) {
    char abs_path[PATH_MAX];
    get_realpath(fe->path, abs_path, ctx->cfg);

    int is_cwd = (strcmp(abs_path, ctx->cfg->cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    char line[ENTRY_BUF_SIZE];
    int pos = 0;

    /* Print optional line prefix (used for interactive selection cursor) */
    if (ctx->line_prefix) {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s", ctx->line_prefix);
    }

    if (ctx->cfg->long_format && ctx->columns) {
        char col_buf[32];
        for (int i = 0; i < NUM_COLUMNS; i++) {
            ctx->columns[i].format(fe, ctx->icons, col_buf, sizeof(col_buf));
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*s%s", CLR(ctx->cfg, COLOR_GREY), ctx->columns[i].width, col_buf, RST(ctx->cfg));
            if (i == COL_LINES) {
                const char *count_icon = get_count_icon(fe, ctx->icons);
                if (count_icon[0]) {
                    EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s", CLR(ctx->cfg, COLOR_GREY), count_icon, RST(ctx->cfg));
                } else {
                    EMIT(line, pos, ENTRY_BUF_SIZE, "  ");
                }
            }
            EMIT(line, pos, ENTRY_BUF_SIZE, "  ");
        }
        /* Diff columns (only shown when there are diffs) */
        if (ctx->diff_add_width > 0) {
            if (fe->diff_added > 0) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*d%s ", CLR(ctx->cfg, COLOR_GREEN),
                       ctx->diff_add_width, fe->diff_added, RST(ctx->cfg));
            } else {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*s%s ", CLR(ctx->cfg, COLOR_GREY),
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
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%-*d%s ", CLR(ctx->cfg, COLOR_RED),
                       ctx->diff_del_width, diff_removed, RST(ctx->cfg));
            } else {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%-*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_del_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_add_width > 0 || ctx->diff_del_width > 0) {
            EMIT(line, pos, ENTRY_BUF_SIZE, " ");
        }
    }

    emit_prefix(line, &pos, ENTRY_BUF_SIZE, depth, ctx->continuation, ctx->cfg);

    int is_readonly = (access(fe->path, W_OK) != 0 && access(fe->path, R_OK) == 0);
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    if (!ctx->cfg->no_icons && is_readonly && !is_dir) {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s ", CLR(ctx->cfg, COLOR_YELLOW), ctx->icons->readonly, RST(ctx->cfg));
    }

    if (is_dir && !has_visible_children && !ctx->cfg->no_icons) {
        /* Collapsed directory: show full recursive summary */
        GitSummary gs = git_get_dir_summary(ctx->git, abs_path);
        if (gs.modified) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
        }
        if (gs.untracked) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
        }
        if (gs.staged) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
        }
        if (gs.deleted) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
        }
    } else if (is_dir && has_visible_children && !ctx->cfg->no_icons) {
        /* Expanded directory: show directly deleted files count */
        int deleted_direct = git_count_deleted_direct(ctx->git, abs_path);
        if (deleted_direct) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), deleted_direct, ctx->icons->git_deleted, RST(ctx->cfg));
        }
        /* Also show hidden file status if hidden files aren't shown */
        if (!ctx->cfg->show_hidden) {
            GitSummary gs = git_get_hidden_dir_summary(ctx->git, abs_path);
            if (gs.modified) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
            }
            if (gs.untracked) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
            }
            if (gs.staged) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
            }
            /* Note: deleted already counted above in deleted_direct if visible,
             * but hidden deleted files need separate handling */
            if (gs.deleted && !deleted_direct) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
            }
        }
    } else {
        const char *git_ind = get_git_indicator(ctx->git, abs_path, ctx->icons, ctx->cfg);
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s", git_ind);
    }

    int is_locked = (fe->type == FTYPE_DIR && (fe->size < 0 || is_readonly));
    const char *color = (fe->type == FTYPE_DIR && fe->size < 0) ? CLR(ctx->cfg, COLOR_RED) :
                        get_file_color(fe->type, is_cwd, fe->is_ignored, ctx->cfg->is_tty, ctx->cfg->color_all);
    const char *style = is_hidden ? CLR(ctx->cfg, STYLE_ITALIC) : "";

    if (!ctx->cfg->no_icons) {
        int is_binary = (fe->file_count < 0 && fe->line_count == -1);
        int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
        int is_expanded = is_dir ? was_expanded : 0;
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s ", color, get_icon(ctx->icons, fe->type, is_expanded, is_locked, is_binary, fe->name), RST(ctx->cfg));
    }

    const char *bold = ctx->selected ? CLR(ctx->cfg, STYLE_BOLD) : "";
    if (ctx->cfg->list_mode) {
        char abbrev[PATH_MAX];
        abbreviate_home(abs_path, abbrev, sizeof(abbrev), ctx->cfg);
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s%s%s", color, bold, style, abbrev, RST(ctx->cfg));
    } else {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s%s%s", color, bold, style, fe->name, RST(ctx->cfg));
    }

    if (is_dir && fe->is_git_root) {
        GitBranchInfo gi;
        if (git_get_branch_info(fe->path, &gi)) {
            if (gi.has_upstream) {
                const char *cloud_color = gi.out_of_sync ? COLOR_RED : COLOR_GREY;
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s%s %s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), gi.branch, RST(ctx->cfg), CLR(ctx->cfg, cloud_color), ctx->icons->git_upstream, RST(ctx->cfg));
            } else {
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), gi.branch, RST(ctx->cfg));
            }
            free(gi.branch);
        }
    }

    if (fe->symlink_target) {
        char abbrev[PATH_MAX];
        abbreviate_home(fe->symlink_target, abbrev, sizeof(abbrev), ctx->cfg);
        const char *target_base = strrchr(fe->symlink_target, '/');
        target_base = target_base ? target_base + 1 : fe->symlink_target;
        const char *target_style = (target_base[0] == '.') ? CLR(ctx->cfg, STYLE_ITALIC) : "";
        const char *target_color = (fe->type == FTYPE_SYMLINK_BROKEN) ?
            CLR(ctx->cfg, COLOR_RED) : CLR(ctx->cfg, COLOR_GREY);
        EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s %s%s%s%s", CLR(ctx->cfg, COLOR_GREY), ctx->icons->symlink, RST(ctx->cfg), target_color, target_style, abbrev, RST(ctx->cfg));
    }

    line[pos] = '\0';

    /* Truncate to terminal width if set */
    if (ctx->term_width > 0 && visible_strlen(line) > ctx->term_width) {
        char *truncated = truncate_visible(line, ctx->term_width);
        printf("%s\n", truncated);
        free(truncated);
    } else {
        printf("%s\n", line);
    }
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
 * Summary Mode - Card Layout
 * ============================================================================ */

#define MAX_CARD_LINES 64
#define MAX_CARD_LINE_LEN 512

typedef struct {
    char lines[MAX_CARD_LINES][MAX_CARD_LINE_LEN];
    int visible_lens[MAX_CARD_LINES];  /* Length without ANSI codes */
    int count;
    int max_width;
} Card;

static void card_init(Card *card) {
    card->count = 0;
    card->max_width = 0;
}

/* Get terminal width (columns) */
int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;  /* Default fallback */
}

/* Calculate visible length (excluding ANSI escape sequences) */
static int visible_strlen(const char *s) {
    int len = 0;
    int in_escape = 0;
    for (; *s; s++) {
        if (*s == '\033') {
            in_escape = 1;
        } else if (in_escape) {
            if (*s == 'm') in_escape = 0;
        } else {
            /* Handle UTF-8: count codepoints, not bytes */
            if ((*s & 0xC0) != 0x80) len++;
        }
    }
    return len;
}

/* Truncate string to max_visible_len visible characters, adding "..." if truncated.
 * Preserves ANSI escape sequences. Returns new string that must be freed. */
static char *truncate_visible(const char *s, int max_visible_len) {
    if (max_visible_len < 4) max_visible_len = 4;

    int visible_len = visible_strlen(s);
    if (visible_len <= max_visible_len) {
        return xstrdup(s);
    }

    int target_visible = max_visible_len - 3;

    size_t alloc_size = strlen(s) + 4;
    char *result = xmalloc(alloc_size);

    const char *src = s;
    char *dst = result;
    int visible_count = 0;
    int in_escape = 0;

    while (*src && visible_count < target_visible) {
        if (*src == '\033') {
            in_escape = 1;
            *dst++ = *src++;
        } else if (in_escape) {
            *dst++ = *src++;
            if (*(src - 1) == 'm') in_escape = 0;
        } else {
            if ((*src & 0xC0) != 0x80) {
                visible_count++;
                if (visible_count > target_visible) break;
            }
            *dst++ = *src++;
            while (*src && (*src & 0xC0) == 0x80) {
                *dst++ = *src++;
            }
        }
    }

    *dst++ = '.';
    *dst++ = '.';
    *dst++ = '.';

    /* Copy remaining escape sequences to ensure colors reset */
    while (*src) {
        if (*src == '\033') {
            in_escape = 1;
            *dst++ = *src++;
        } else if (in_escape) {
            *dst++ = *src++;
            if (*(src - 1) == 'm') in_escape = 0;
        } else {
            src++;
        }
    }

    *dst = '\0';
    return result;
}

static void card_add(Card *card, const char *fmt, ...) {
    if (card->count >= MAX_CARD_LINES) return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(card->lines[card->count], MAX_CARD_LINE_LEN, fmt, args);
    va_end(args);

    /* Remove trailing newline if present */
    int len = strlen(card->lines[card->count]);
    if (len > 0 && card->lines[card->count][len - 1] == '\n') {
        card->lines[card->count][len - 1] = '\0';
    }

    card->visible_lens[card->count] = visible_strlen(card->lines[card->count]);
    if (card->visible_lens[card->count] > card->max_width) {
        card->max_width = card->visible_lens[card->count];
    }
    card->count++;
}

static void card_add_empty(Card *card) {
    if (card->count >= MAX_CARD_LINES) return;
    card->lines[card->count][0] = '\0';
    card->visible_lens[card->count] = 0;
    card->count++;
}

static void card_print(const Card *card, const Config *cfg) {
    int term_width = cfg->is_tty ? get_terminal_width() : 0;
    int max_content_width = term_width > 0 ? term_width - 4 : 0;  /* 2 border + 2 padding */

    /* Limit card width to terminal if applicable */
    int content_width = card->max_width;
    if (max_content_width > 0 && content_width > max_content_width) {
        content_width = max_content_width;
    }

    int width = content_width + 4;
    if (width < 20) width = 20;

    /* Top border */
    printf("%s┌", CLR(cfg, COLOR_GREY));
    for (int i = 0; i < width - 2; i++) printf("─");
    printf("┐%s\n", RST(cfg));

    /* Content lines */
    for (int i = 0; i < card->count; i++) {
        char *display_line;
        int display_len;

        if (max_content_width > 0 && card->visible_lens[i] > max_content_width) {
            display_line = truncate_visible(card->lines[i], max_content_width);
            display_len = visible_strlen(display_line);
        } else {
            display_line = (char *)card->lines[i];
            display_len = card->visible_lens[i];
        }

        int padding = width - 4 - display_len;
        if (padding < 0) padding = 0;

        printf("%s│%s %s", CLR(cfg, COLOR_GREY), RST(cfg), display_line);
        for (int j = 0; j < padding; j++) printf(" ");
        printf(" %s│%s\n", CLR(cfg, COLOR_GREY), RST(cfg));

        if (display_line != card->lines[i]) {
            free(display_line);
        }
    }

    /* Bottom border */
    printf("%s└", CLR(cfg, COLOR_GREY));
    for (int i = 0; i < width - 2; i++) printf("─");
    printf("┘%s\n", RST(cfg));
}

/* Print summary for a single file or directory */
void print_summary(TreeNode *node, PrintContext *ctx) {
    FileEntry *fe = &node->entry;
    const Config *cfg = ctx->cfg;
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    int is_cwd = (strcmp(fe->path, cfg->cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    /* Compute extended data if not already done.
     * Summary mode always counts all files (including hidden) to match file_count. */
    if (is_dir && !fe->has_type_stats) {
        fileinfo_compute_type_stats(fe, node, ctx->filetypes, ctx->shebangs, 1);
    }

    char abs_path[PATH_MAX];
    get_realpath(fe->path, abs_path, cfg);
    char git_root[PATH_MAX];
    int in_git_repo = git_find_root(abs_path, git_root, sizeof(git_root));

    if (is_dir && in_git_repo && !fe->has_git_dir_status) {
        fileinfo_compute_git_dir_status(fe, ctx->git);
    }

    if (is_dir && fe->is_git_root && !fe->has_git_repo_info) {
        fileinfo_compute_git_repo_info(fe, ctx->git);
    }

    Card card;
    card_init(&card);

    /* Header line: icon + name */
    int is_locked = (fe->type == FTYPE_DIR && access(fe->path, R_OK) != 0);
    const char *color = get_file_color(fe->type, is_cwd, fe->is_ignored, cfg->is_tty, cfg->color_all);
    const char *style = is_hidden ? CLR(cfg, STYLE_ITALIC) : "";
    int is_binary = (fe->file_count < 0 && fe->line_count == -1);
    const char *icon = cfg->no_icons ? "" : get_icon(ctx->icons, fe->type, node->was_expanded, is_locked, is_binary, fe->name);
    const char *icon_space = cfg->no_icons ? "" : " ";

    if (fe->has_git_repo_info && fe->branch) {
        if (fe->has_upstream) {
            const char *cloud_color = fe->out_of_sync ? COLOR_RED : COLOR_GREY;
            card_add(&card, "%s%s%s%s%s%s%s %s%s%s%s %s%s%s",
                     color, icon, icon_space, style, fe->name, RST(cfg), "",
                     CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), fe->branch, RST(cfg),
                     CLR(cfg, cloud_color), ctx->icons->git_upstream, RST(cfg));
        } else {
            card_add(&card, "%s%s%s%s%s%s%s %s%s%s%s",
                     color, icon, icon_space, style, fe->name, RST(cfg), "",
                     CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), fe->branch, RST(cfg));
        }
    } else {
        card_add(&card, "%s%s%s%s%s%s", color, icon, icon_space, style, fe->name, RST(cfg));
    }

    card_add_empty(&card);

    /* Path */
    if (fe->symlink_target) {
        char link_path[PATH_MAX];
        get_abspath(fe->path, link_path, cfg);
        card_add(&card, "%sPath:%s     %s", CLR(cfg, COLOR_GREY), RST(cfg), link_path);
        card_add(&card, "%sTarget:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), fe->symlink_target);
    } else {
        char abs_path[PATH_MAX];
        char real_path[PATH_MAX];
        get_abspath(fe->path, abs_path, cfg);
        get_realpath(fe->path, real_path, cfg);
        card_add(&card, "%sPath:%s     %s", CLR(cfg, COLOR_GREY), RST(cfg), abs_path);
        if (strcmp(abs_path, real_path) != 0) {
            card_add(&card, "%sTarget:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), real_path);
        }
    }

    /* Type (files only) */
    if (!is_dir) {
        const char *type_name = get_file_type_name(fe->path, ctx->filetypes, ctx->shebangs);
        if (type_name) {
            card_add(&card, "%sType:%s     %s", CLR(cfg, COLOR_GREY), RST(cfg), type_name);
        }
    }

    /* Size */
    char size_buf[32];
    format_size(fe->size, size_buf, sizeof(size_buf));
    card_add(&card, "%sSize:%s     %s", CLR(cfg, COLOR_GREY), RST(cfg), size_buf);

    /* File type breakdown table (directories only) */
    if (is_dir && fe->has_type_stats && fe->type_stats.count > 0) {
        type_stats_sort(&fe->type_stats);

        /* Calculate column widths */
        int max_name_len = 0, max_files_len = 0, max_lines_len = 0;
        for (int i = 0; i < fe->type_stats.count; i++) {
            TypeStat *ts = &fe->type_stats.entries[i];
            int nlen = (int)strlen(ts->name);
            if (nlen > max_name_len) max_name_len = nlen;
            char tmp[32];
            format_count(ts->file_count, tmp, sizeof(tmp));
            int flen = (int)strlen(tmp);
            if (flen > max_files_len) max_files_len = flen;
            if (ts->has_lines) {
                format_count(ts->line_count, tmp, sizeof(tmp));
                int llen = (int)strlen(tmp);
                if (llen > max_lines_len) max_lines_len = llen;
            }
        }
        /* Check totals width and ensure headers fit */
        char tmp[32];
        format_count(fe->type_stats.total_files, tmp, sizeof(tmp));
        if ((int)strlen(tmp) > max_files_len) max_files_len = (int)strlen(tmp);
        format_count(fe->type_stats.total_lines, tmp, sizeof(tmp));
        if ((int)strlen(tmp) > max_lines_len) max_lines_len = (int)strlen(tmp);
        if (max_name_len < 5) max_name_len = 5;   /* At least "Total" */
        if (max_files_len < 5) max_files_len = 5;  /* At least "Files" */
        if (max_lines_len < 5) max_lines_len = 5;  /* At least "Lines" */

        /* Header */
        card_add(&card, "%s%*s  %*s  %*s%s", CLR(cfg, COLOR_GREY),
                 max_name_len, "", max_files_len, "Files", max_lines_len, "Lines", RST(cfg));

        /* Per-type rows */
        for (int i = 0; i < fe->type_stats.count; i++) {
            TypeStat *ts = &fe->type_stats.entries[i];
            char files_buf[32], lines_buf[32];
            format_count(ts->file_count, files_buf, sizeof(files_buf));
            if (ts->has_lines) {
                format_count(ts->line_count, lines_buf, sizeof(lines_buf));
            } else {
                snprintf(lines_buf, sizeof(lines_buf), "-");
            }
            card_add(&card, "%s%-*s%s  %*s  %*s",
                     CLR(cfg, COLOR_GREY), max_name_len, ts->name, RST(cfg),
                     max_files_len, files_buf, max_lines_len, lines_buf);
        }

        /* Total row */
        char total_files[32], total_lines[32];
        format_count(fe->type_stats.total_files, total_files, sizeof(total_files));
        if (fe->type_stats.total_lines > 0) {
            format_count(fe->type_stats.total_lines, total_lines, sizeof(total_lines));
        } else {
            snprintf(total_lines, sizeof(total_lines), "-");
        }
        card_add(&card, "%s%-*s  %*s  %*s%s", CLR(cfg, COLOR_GREY),
                 max_name_len, "Total", max_files_len, total_files,
                 max_lines_len, total_lines, RST(cfg));
    } else if (is_dir && fe->file_count >= 0) {
        /* Fallback: just show file count if no type stats */
        char count_buf[32];
        format_count(fe->file_count, count_buf, sizeof(count_buf));
        card_add(&card, "%sFiles:%s    %s", CLR(cfg, COLOR_GREY), RST(cfg), count_buf);
    }

    /* Single file stats */
    if (!is_dir && fe->line_count >= 0 && fe->content_type == CONTENT_TEXT) {
        char line_buf[32];
        format_count(fe->line_count, line_buf, sizeof(line_buf));
        card_add(&card, "%sLines:%s    %s", CLR(cfg, COLOR_GREY), RST(cfg), line_buf);
        if (fe->word_count >= 0) {
            char word_buf[32];
            format_count(fe->word_count, word_buf, sizeof(word_buf));
            card_add(&card, "%sWords:%s    %s", CLR(cfg, COLOR_GREY), RST(cfg), word_buf);
        }
    } else if (fe->line_count >= 0 && fe->content_type == CONTENT_PDF) {
        card_add(&card, "%sPages:%s    %d", CLR(cfg, COLOR_GREY), RST(cfg), fe->line_count);
    }

    /* Modified time */
    char time_buf[64], rel_buf[32];
    struct tm *tm = localtime(&fe->mtime);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm);
    format_relative_time(fe->mtime, rel_buf, sizeof(rel_buf));
    card_add(&card, "%sModified:%s %s (%s)", CLR(cfg, COLOR_GREY), RST(cfg), time_buf, rel_buf);

    /* Git info (for git repositories) */
    if (fe->has_git_repo_info) {
        card_add_empty(&card);

        if (fe->branch) {
            if (fe->short_hash[0]) {
                card_add(&card, "%sBranch:%s   %s %s(%s)%s", CLR(cfg, COLOR_GREY), RST(cfg),
                         fe->branch, CLR(cfg, COLOR_GREY), fe->short_hash, RST(cfg));
            } else {
                card_add(&card, "%sBranch:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), fe->branch);
            }
        }
        if (fe->commit_count[0]) {
            card_add(&card, "%sCommits:%s  %s", CLR(cfg, COLOR_GREY), RST(cfg), fe->commit_count);
        }
        if (fe->tag) {
            if (fe->tag_distance > 0) {
                card_add(&card, "%sTag:%s      %s %s(+%d)%s", CLR(cfg, COLOR_GREY), RST(cfg),
                         fe->tag, CLR(cfg, COLOR_GREY), fe->tag_distance, RST(cfg));
            } else {
                card_add(&card, "%sTag:%s      %s", CLR(cfg, COLOR_GREY), RST(cfg), fe->tag);
            }
        }
        if (fe->remote) {
            card_add(&card, "%sRemote:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), fe->remote);
        }

        /* Dirty status */
        GitSummary *summary = &fe->repo_status;
        if (summary->modified || summary->untracked || summary->staged || summary->deleted) {
            char status_buf[128] = "";
            int pos = 0;
            if (summary->staged) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%d staged%s", CLR(cfg, COLOR_GREEN), summary->staged, RST(cfg));
            }
            if (summary->modified) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d modified%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_RED), summary->modified, RST(cfg));
            }
            if (summary->deleted) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d deleted%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_RED), summary->deleted, RST(cfg));
            }
            if (summary->untracked) {
                snprintf(status_buf + pos, sizeof(status_buf) - pos,
                         "%s%s%d untracked%s", pos > 0 ? ", " : "",
                         CLR(cfg, COLOR_GREY), summary->untracked, RST(cfg));
            }
            card_add(&card, "%sStatus:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), status_buf);
        }
    }

    /* Git status summary for directories inside a repo (but not repo root) */
    if (is_dir && fe->has_git_dir_status && !fe->has_git_repo_info) {
        GitSummary *gs = &fe->git_dir_status;
        if (gs->modified || gs->untracked || gs->staged || gs->deleted) {
            char status_buf[128] = "";
            int pos = 0;
            if (gs->staged) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%d staged%s", CLR(cfg, COLOR_GREEN), gs->staged, RST(cfg));
            }
            if (gs->modified) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d modified%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_RED), gs->modified, RST(cfg));
            }
            if (gs->deleted) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d deleted%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_RED), gs->deleted, RST(cfg));
            }
            if (gs->untracked) {
                snprintf(status_buf + pos, sizeof(status_buf) - pos,
                         "%s%s%d untracked%s", pos > 0 ? ", " : "",
                         CLR(cfg, COLOR_GREY), gs->untracked, RST(cfg));
            }
            card_add_empty(&card);
            card_add(&card, "%sStatus:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), status_buf);
        }
    }

    card_print(&card, cfg);
    printf("\n");
}
