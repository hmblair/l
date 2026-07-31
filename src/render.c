/*
 * render.c - Rendering: row printing, the view renderer, and the summary
 * card. Everything here is presentation: all facts are read from FileEntry
 * and the View; no filesystem or git queries happen at draw time.
 */

#include "render.h"
#include "cache.h"
#include <dirent.h>
#include <ctype.h>
#include <fnmatch.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/ioctl.h>

/* Path convenience wrappers over the Config environment */
static void get_realpath(const char *path, char *resolved, const Config *cfg) {
    path_get_realpath(path, resolved, cfg->env.cwd);
}

static void get_abspath(const char *path, char *resolved, const Config *cfg) {
    path_get_abspath(path, resolved, cfg->env.cwd);
}

static void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg) {
    path_abbreviate_home(path, buf, len, cfg->env.home);
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

const char *git_indicator_from_flags(unsigned flags, const Icons *icons,
                                     const Config *cfg) {
    static __thread char indicator[L_GIT_INDICATOR_SIZE];
    indicator[0] = '\0';

    if (cfg->disp.no_icons) return indicator;
    if (!flags || (flags & GITF_IGNORED)) return indicator;

    char *p = indicator;
    size_t remaining = sizeof(indicator);

    if (flags & GITF_UNTRACKED) {
        append_git_icon(p, &remaining, icons->git_untracked, CLR(cfg, COLOR_RED), cfg);
    } else {
        if (flags & GITF_WT_MODIFIED) {
            p = append_git_icon(p, &remaining, icons->git_modified, CLR(cfg, COLOR_RED), cfg);
        } else if (flags & GITF_WT_DELETED) {
            p = append_git_icon(p, &remaining, icons->git_deleted, CLR(cfg, COLOR_RED), cfg);
        }
        if (flags & (GITF_STAGED | GITF_STAGED_DELETED)) {
            const char *staged_icon = (flags & GITF_STAGED_DELETED)
                ? icons->git_deleted : icons->git_staged;
            append_git_icon(p, &remaining, staged_icon, CLR(cfg, COLOR_YELLOW), cfg);
        }
    }

    return indicator;
}


/* ============================================================================
 * Tree Printing
 * ============================================================================ */

/* Declared in render.h; defined later in this file */

/* Buffer size for print_entry line assembly */
#define ENTRY_BUF_SIZE 8192

/* Append formatted text to a buffer, advancing pos. Silently stops if full. */
#define EMIT(buf, pos, size, ...) do { \
    int _n = snprintf((buf) + (pos), (size) - (pos), __VA_ARGS__); \
    if (_n > 0 && (pos) + _n < (int)(size)) (pos) += _n; \
    else if (_n > 0) (pos) = (int)(size) - 1; \
} while (0)

static void emit_prefix(char *buf, int *pos, int size, int depth, int *continuation, const Config *cfg) {
    if (cfg->req.list_mode) return;

    if (!cfg->disp.is_tty) {
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

void print_entry(const FileEntry *fe, int depth, int was_expanded, const PrintContext *ctx) {
    /* Use the canonical path precomputed at build time; fall back to realpath()
     * only when it wasn't precomputed (e.g. ancestry mode). */
    char abs_path_buf[PATH_MAX];
    const char *abs_path = fe->abs_path;
    if (!abs_path) {
        get_realpath(fe->path, abs_path_buf, ctx->cfg);
        abs_path = abs_path_buf;
    }

    /* Match on the logical path (symlinks preserved), not abs_path (realpath-
     * resolved), so the marker follows the folder the user actually navigated
     * through and lands on exactly one row even when a symlink and its target
     * both appear in the tree. */
    int is_cwd = (strcmp(fe->path, ctx->cfg->env.cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    char line[ENTRY_BUF_SIZE];
    int pos = 0;

    /* Print optional line prefix (used for interactive selection cursor) */
    if (ctx->line_prefix) {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s", ctx->line_prefix);
    }

    if (ctx->cfg->disp.long_format && ctx->columns) {
        char col_buf[32];
        for (int i = 0; i < NUM_COLUMNS; i++) {
            ctx->columns[i].format(fe, ctx->icons, col_buf, sizeof(col_buf));
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*s%s", CLR(ctx->cfg, COLOR_GREY), ctx->columns[i].width, col_buf, RST(ctx->cfg));
            if (i == COL_LINES) {
                const char *count_icon = content_quantity_icon(fe, ctx->icons);
                if (count_icon[0]) {
                    EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s", CLR(ctx->cfg, COLOR_GREY), count_icon, RST(ctx->cfg));
                } else {
                    EMIT(line, pos, ENTRY_BUF_SIZE, "  ");
                }
            }
            if (i == NUM_COLUMNS - 1) {
                /* Trailing gap before the diff/tree columns. When diff columns
                 * are present, mirror the inter-column separator so the dot
                 * carries through; otherwise use the plain two-space gap. */
                const char *sep = ctx->cfg->disp.column_separator;
                int has_diff = (ctx->diff_add_width > 0 || ctx->diff_del_width > 0);
                if (has_diff && sep[0]) {
                    EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s ", CLR(ctx->cfg, COLOR_GREY), sep, RST(ctx->cfg));
                } else {
                    EMIT(line, pos, ENTRY_BUF_SIZE, "  ");
                }
            } else {
                const char *sep = ctx->cfg->disp.column_separator;
                if (sep[0]) {
                    EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s ", CLR(ctx->cfg, COLOR_GREY), sep, RST(ctx->cfg));
                } else {
                    /* Blank separator: fall back to the original two-space gap. */
                    EMIT(line, pos, ENTRY_BUF_SIZE, "  ");
                }
            }
        }
        /* Diff columns (only shown when there are diffs). A directory's counts
         * are its rolled-up view summary — the lines hidden inside descendants
         * not shown on their own row. */
        int diff_added, diff_removed;
        entry_diff_stats(fe, ctx->git, &diff_added, &diff_removed);
        if (ctx->diff_add_width > 0) {
            if (diff_added > 0) {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*d%s ", CLR(ctx->cfg, COLOR_GREEN),
                       ctx->diff_add_width, diff_added, RST(ctx->cfg));
            } else {
                EMIT(line, pos, ENTRY_BUF_SIZE, "%s%*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_add_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_del_width > 0) {
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

    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    /* Grey lock before any entry you can't write to (read-only or no access).
     * A writable-but-unreadable dir (drop-box) stays unlocked but renders red. */
    if (!ctx->cfg->disp.no_icons && fe->is_readonly) {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s ", CLR(ctx->cfg, COLOR_GREY), ctx->icons->readonly, RST(ctx->cfg));
    }

    if (is_dir && !ctx->cfg->disp.no_icons) {
        /* Directory: show the git status of descendants that aren't shown on
         * their own row. This is precomputed per view in view_git_summary (see
         * compute_view_summaries / the picker's view prep); a collapsed dir
         * carries its full recursive summary, an expanded one only the hidden
         * and filtered-out children plus deleted files. Fall back to the full
         * recursive summary if no view was prepared. */
        GitSummary gs = fe->has_view_git_summary
            ? fe->view_git_summary
            : git_get_dir_summary(ctx->git, abs_path);
        if (gs.modified) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
        }
        if (gs.untracked) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
        }
        if (gs.staged) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
        }
        if (gs.staged_deleted) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged_deleted, ctx->icons->git_deleted, RST(ctx->cfg));
        }
        if (gs.deleted) {
            EMIT(line, pos, ENTRY_BUF_SIZE, "%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
        }
    } else {
        const char *git_ind = git_indicator_from_flags(fe->git_flags, ctx->icons, ctx->cfg);
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s", git_ind);
    }

    const char *color = (fe->type == FTYPE_DIR && fe->size < 0) ? CLR(ctx->cfg, COLOR_RED) :
                        get_file_color(fe->type, fe->is_ignored, ctx->cfg->disp.is_tty, ctx->cfg->disp.color_all);
    const char *style = is_hidden ? CLR(ctx->cfg, STYLE_ITALIC) : "";

    if (!ctx->cfg->disp.no_icons) {
        int is_binary = (fe->file_count < 0 && fe->line_count == -1);
        int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
        int is_expanded = is_dir ? was_expanded : 0;
        int is_root = (fe->path[0] == '/' && fe->path[1] == '\0');
        const char *icon;
        if ((fe->is_mount_point || is_root) && fe->type == FTYPE_DIR) {
            icon = ctx->icons->mount_point;
        } else {
            icon = get_icon(ctx->icons, fe->type, is_expanded, is_binary, fe->name);
        }
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s ", color, icon, RST(ctx->cfg));
    }

    const char *bold = ctx->selected ? CLR(ctx->cfg, STYLE_BOLD) : "";
    if (ctx->cfg->req.list_mode) {
        char abbrev[PATH_MAX];
        abbreviate_home(abs_path, abbrev, sizeof(abbrev), ctx->cfg);
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s%s%s", color, bold, style, abbrev, RST(ctx->cfg));
    } else {
        EMIT(line, pos, ENTRY_BUF_SIZE, "%s%s%s%s%s", color, bold, style, fe->name, RST(ctx->cfg));
    }

    if (is_cwd) {
        EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s", CLR(ctx->cfg, COLOR_YELLOW), ctx->icons->cwd_marker, RST(ctx->cfg));
    }

    /* Repo root decorations: all fields were computed at build time
     * (annotate_git_root), so rendering never touches git. */
    if (is_dir && fe->is_git_root && fe->branch) {
        EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s %s%s%s", CLR(ctx->cfg, COLOR_GREY), ctx->icons->git_branch, CLR(ctx->cfg, STYLE_ITALIC), fe->branch, RST(ctx->cfg));
        if (fe->short_hash[0]) {
            EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s %s%s%s", CLR(ctx->cfg, COLOR_GREY), ctx->icons->git_commit, CLR(ctx->cfg, STYLE_ITALIC), fe->short_hash, RST(ctx->cfg));
        }
        if (fe->tag) {
            EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s %s%s", CLR(ctx->cfg, COLOR_GREY), ctx->icons->git_tag, fe->tag, RST(ctx->cfg));
        }
        if (fe->has_upstream) {
            const char *cloud_color = fe->out_of_sync ? COLOR_RED : COLOR_GREY;
            char *web_url = git_remote_to_web_url(fe->remote);
            if (web_url && ctx->cfg->disp.is_tty) {
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s\033]8;;%s\033\\%s\033]8;;\033\\%s", CLR(ctx->cfg, cloud_color), web_url, ctx->icons->git_upstream, RST(ctx->cfg));
            } else {
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s%s%s", CLR(ctx->cfg, cloud_color), ctx->icons->git_upstream, RST(ctx->cfg));
            }
            free(web_url);
            if (fe->ahead > 0)
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s+%d%s", CLR(ctx->cfg, COLOR_RED), fe->ahead, RST(ctx->cfg));
            if (fe->behind > 0)
                EMIT(line, pos, ENTRY_BUF_SIZE, " %s-%d%s", CLR(ctx->cfg, COLOR_RED), fe->behind, RST(ctx->cfg));
        }
    }

    if (fe->symlink_target) {
        char abbrev[PATH_MAX];
        /* Show relative path if target is under the same directory as the link */
        const char *link_slash = strrchr(fe->path, '/');
        size_t dir_len = link_slash ? (size_t)(link_slash - fe->path) : 0;
        if (dir_len > 0 && strncmp(fe->symlink_target, fe->path, dir_len) == 0 &&
            fe->symlink_target[dir_len] == '/') {
            snprintf(abbrev, sizeof(abbrev), "%s", fe->symlink_target + dir_len + 1);
        } else {
            abbreviate_home(fe->symlink_target, abbrev, sizeof(abbrev), ctx->cfg);
        }
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

/* Draw every row of a built View: decode each row's continuation mask into
 * the context's array and print. Trees that filtering emptied print the
 * "No matches." notice instead. */
void render_view(const View *view, PrintContext *ctx) {
    for (int t = 0; t < view->tree_count; t++) {
        if (view->tree_no_matches[t]) {
            printf("%sNo matches.%s\n",
                   CLR(ctx->cfg, COLOR_RED), RST(ctx->cfg));
            continue;
        }
        for (size_t i = view->tree_row_start[t]; i < view->tree_row_start[t + 1]; i++) {
            const ViewRow *row = &view->rows[i];
            for (int d = 0; d < row->depth && d < L_MAX_DEPTH; d++) {
                ctx->continuation[d] = (row->cont_mask >> d) & 1;
            }
            print_entry(&row->node->entry, row->depth, row->expanded, ctx);
        }
    }
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
int visible_strlen(const char *s) {
    int len = 0;
    for (; *s; s++) {
        if (s[0] == '\033' && s[1] == ']') {
            /* OSC sequence: skip until ST (\033\\) */
            s += 2;
            while (*s && !(s[0] == '\033' && s[1] == '\\')) s++;
            if (*s) s++; /* skip past \\ */
        } else if (*s == '\033') {
            /* CSI sequence: skip until 'm' */
            s++;
            while (*s && *s != 'm') s++;
        } else {
            if ((*s & 0xC0) != 0x80) len++;
        }
    }
    return len;
}

/* Truncate string to max_visible_len visible characters, adding "..." if truncated.
 * Preserves ANSI escape sequences. Returns new string that must be freed. */
char *truncate_visible(const char *s, int max_visible_len) {
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
        if (src[0] == '\033' && src[1] == ']') {
            /* Copy OSC sequence verbatim */
            *dst++ = *src++; *dst++ = *src++;
            while (*src && !(src[0] == '\033' && src[1] == '\\')) *dst++ = *src++;
            if (*src) { *dst++ = *src++; *dst++ = *src++; }
        } else if (*src == '\033') {
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

    /* Copy remaining escape sequences to ensure colors/links reset */
    while (*src) {
        if (src[0] == '\033' && src[1] == ']') {
            *dst++ = *src++; *dst++ = *src++;
            while (*src && !(src[0] == '\033' && src[1] == '\\')) *dst++ = *src++;
            if (*src) { *dst++ = *src++; *dst++ = *src++; }
        } else if (*src == '\033') {
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
    int term_width = cfg->disp.is_tty ? get_terminal_width() : 0;
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

/* Data step for summary mode, run before print_summary: compute the type
 * statistics and git info the card displays, so printing itself is pure.
 * Summary mode always counts all files (including hidden) to match
 * file_count. */
void summary_prepare(TreeNode *node, PrintContext *ctx) {
    FileEntry *fe = &node->entry;
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);

    if (is_dir && !fe->has_type_stats) {
        fileinfo_compute_type_stats(fe, node, ctx->filetypes, ctx->shebangs, 1);
    }

    char abs_path[PATH_MAX];
    get_realpath(fe->path, abs_path, ctx->cfg);
    char git_root[PATH_MAX];
    int in_git_repo = git_find_root(abs_path, git_root, sizeof(git_root));

    if (is_dir && in_git_repo && !fe->has_git_dir_status) {
        fileinfo_compute_git_dir_status(fe, ctx->git);
    }

    if (is_dir && fe->is_git_root && !fe->has_git_repo_info) {
        fileinfo_compute_git_repo_info(fe, ctx->git);
    }
}

/* Print summary for a single file or directory (summary_prepare must have
 * run on the node) */
void print_summary(TreeNode *node, PrintContext *ctx) {
    FileEntry *fe = &node->entry;
    const Config *cfg = ctx->cfg;
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    int is_cwd = (strcmp(fe->path, cfg->env.cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    Card card;
    card_init(&card);

    /* Header line: icon + name */
    const char *color = get_file_color(fe->type, fe->is_ignored, cfg->disp.is_tty, cfg->disp.color_all);
    const char *style = is_hidden ? CLR(cfg, STYLE_ITALIC) : "";
    char cwd_marker[64] = "";
    if (is_cwd)
        snprintf(cwd_marker, sizeof(cwd_marker), " %s%s%s", CLR(cfg, COLOR_YELLOW), ctx->icons->cwd_marker, RST(cfg));
    int is_binary = (fe->file_count < 0 && fe->line_count == -1);
    const char *icon = cfg->disp.no_icons ? "" : get_icon(ctx->icons, fe->type, node->was_expanded, is_binary, fe->name);
    const char *icon_space = cfg->disp.no_icons ? "" : " ";

    if (fe->has_git_repo_info && fe->branch) {
        if (fe->has_upstream) {
            const char *cloud_color = fe->out_of_sync ? COLOR_RED : COLOR_GREY;
            char ahead_behind[64] = "";
            int ab_pos = 0;
            if (fe->ahead > 0)
                ab_pos += snprintf(ahead_behind + ab_pos, sizeof(ahead_behind) - ab_pos, " %s+%d%s", CLR(cfg, COLOR_RED), fe->ahead, RST(cfg));
            if (fe->behind > 0)
                snprintf(ahead_behind + ab_pos, sizeof(ahead_behind) - ab_pos, " %s-%d%s", CLR(cfg, COLOR_RED), fe->behind, RST(cfg));
            char *web_url = git_remote_to_web_url(fe->remote);
            if (web_url && cfg->disp.is_tty) {
                card_add(&card, "%s%s%s%s%s%s%s %s%s%s%s %s\033]8;;%s\033\\%s\033]8;;\033\\%s%s",
                         color, icon, icon_space, style, fe->name, RST(cfg), cwd_marker,
                         CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), fe->branch, RST(cfg),
                         CLR(cfg, cloud_color), web_url, ctx->icons->git_upstream, RST(cfg), ahead_behind);
            } else {
                card_add(&card, "%s%s%s%s%s%s%s %s%s%s%s %s%s%s%s",
                         color, icon, icon_space, style, fe->name, RST(cfg), cwd_marker,
                         CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), fe->branch, RST(cfg),
                         CLR(cfg, cloud_color), ctx->icons->git_upstream, RST(cfg), ahead_behind);
            }
            free(web_url);
        } else {
            card_add(&card, "%s%s%s%s%s%s%s %s%s%s%s",
                     color, icon, icon_space, style, fe->name, RST(cfg), cwd_marker,
                     CLR(cfg, COLOR_GREY), CLR(cfg, STYLE_ITALIC), fe->branch, RST(cfg));
        }
    } else {
        card_add(&card, "%s%s%s%s%s%s%s", color, icon, icon_space, style, fe->name, RST(cfg), cwd_marker);
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
    } else if (fe->content_type == CONTENT_AUDIO && fe->line_count >= 0) {
        char dur_buf[32];
        format_duration(fe->line_count, dur_buf, sizeof(dur_buf));
        card_add(&card, "%sDuration:%s %s", CLR(cfg, COLOR_GREY), RST(cfg), dur_buf);
    } else if (fe->content_type == CONTENT_IMAGE && fe->line_count >= 0) {
        double mp = fe->line_count / 10.0;
        char mp_buf[32];
        if (mp >= 10.0) {
            snprintf(mp_buf, sizeof(mp_buf), "%.0f MP", mp);
        } else {
            snprintf(mp_buf, sizeof(mp_buf), "%.1f MP", mp);
        }
        card_add(&card, "%sPixels:%s   %s", CLR(cfg, COLOR_GREY), RST(cfg), mp_buf);
    } else if (fe->content_type == CONTENT_PDF && fe->line_count >= 0) {
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
        if (summary->modified || summary->untracked || summary->staged || summary->deleted || summary->staged_deleted) {
            char status_buf[128] = "";
            int pos = 0;
            if (summary->staged) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%d staged%s", CLR(cfg, COLOR_GREEN), summary->staged, RST(cfg));
            }
            if (summary->staged_deleted) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d deleted (staged)%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_GREEN), summary->staged_deleted, RST(cfg));
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
        if (gs->modified || gs->untracked || gs->staged || gs->deleted || gs->staged_deleted) {
            char status_buf[128] = "";
            int pos = 0;
            if (gs->staged) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%d staged%s", CLR(cfg, COLOR_GREEN), gs->staged, RST(cfg));
            }
            if (gs->staged_deleted) {
                pos += snprintf(status_buf + pos, sizeof(status_buf) - pos,
                                "%s%s%d deleted (staged)%s", pos > 0 ? ", " : "",
                                CLR(cfg, COLOR_GREEN), gs->staged_deleted, RST(cfg));
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
