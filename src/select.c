/*
 * select.c - Interactive file selection mode
 *
 * A cursor over the same View the static listing draws. The tree is a
 * snapshot taken when the picker opens: opening a directory extends the tree
 * from that point (lazy materialization), closing one just collapses the
 * view, and 'r' rebuilds everything from scratch — exactly as if the picker
 * had been closed and reopened, with the cursor put back by path. There is
 * no automatic refresh: the input poll blocks, so an idle picker does no
 * work at all.
 */

#include "select.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

/* ============================================================================
 * Terminal Handling
 * ============================================================================ */

static struct termios orig_termios;
static int raw_mode_enabled = 0;
static int saved_stdout_fd = -1;  /* Original stdout, saved before tty redirect */

static void term_disable_raw(void) {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033[?7h");   /* Re-enable line wrap */
        printf("\033[?25h");  /* Show cursor */
        fflush(stdout);
        raw_mode_enabled = 0;
    }
}

static volatile sig_atomic_t sigint_received = 0;

static void sigint_handler(int sig) {
    (void)sig;
    sigint_received = 1;
}

static void term_enable_raw(void) {
    if (raw_mode_enabled) return;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_disable_raw);

    /* Handle SIGINT to restore terminal */
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    printf("\033[?7l");   /* Disable line wrap */
    printf("\033[?25l");  /* Hide cursor */
    raw_mode_enabled = 1;
}

typedef enum {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_QUIT,
    KEY_OPEN,
    KEY_YANK,
    KEY_RELOAD,       /* 'r' — rebuild the whole tree from scratch */
    KEY_FILTER_FILES,
    KEY_FILTER,       /* '/' — enter interactive filter mode */
    KEY_CHAR,         /* a printable character typed in filter mode */
    KEY_BACKSPACE,    /* delete last filter character */
    KEY_ESC           /* leave filter mode / clear query */
} KeyPress;

/* Wait for stdin to become readable; blocks indefinitely (there is no
 * background work to wake up for). Returns 1 if readable, -1 on error. */
static int poll_stdin(void) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    return poll(&pfd, 1, -1);
}

/* Read one escape sequence after ESC. Returns the mapped arrow KeyPress, or
 * KEY_NONE if a sequence was consumed but unrecognized, or KEY_ESC if ESC was
 * pressed alone (no sequence followed within the timeout). */
static KeyPress read_escape_sequence(void) {
    fd_set fds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  /* 50ms */
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[1] == 'A') return KEY_UP;
                if (seq[1] == 'B') return KEY_DOWN;
                if (seq[1] == 'D') return KEY_LEFT;
                if (seq[1] == 'C') return KEY_RIGHT;
                if (seq[1] == 'Z') return KEY_UP;   /* Shift+Tab */
            }
        }
        return KEY_NONE;  /* consumed an unrecognized sequence */
    }
    return KEY_ESC;  /* ESC pressed alone */
}

/* Read a key. In filter mode, printable characters are returned as KEY_CHAR
 * (with the byte stored in *out_char) so the caller can build a query, and
 * bare ESC leaves filter mode; in normal mode the vim-style navigation keys
 * apply and ESC quits. EOF on stdin quits (nothing further can ever arrive). */
static KeyPress term_read_key(int filter_mode, char *out_char) {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 0) return KEY_QUIT;   /* EOF */
    if (n != 1) return KEY_NONE;

    if (c == 3) return KEY_QUIT;              /* Ctrl+C */
    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == '\t') return KEY_DOWN;           /* Tab cycles down */

    if (filter_mode) {
        if (c == 127 || c == 8) return KEY_BACKSPACE;
        if (c == '\033') {
            KeyPress k = read_escape_sequence();
            return k;  /* arrows navigate; bare ESC (KEY_ESC) leaves filter */
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
            if (out_char) *out_char = c;
            return KEY_CHAR;
        }
        return KEY_NONE;  /* ignore other control bytes while typing */
    }

    if (c == 'q' || c == 'Q') return KEY_QUIT;
    if (c == 'k' || c == 'K') return KEY_UP;
    if (c == 'j' || c == 'J') return KEY_DOWN;
    if (c == 'h' || c == 'H') return KEY_LEFT;
    if (c == 'l' || c == 'L') return KEY_RIGHT;
    if (c == 'o' || c == 'O') return KEY_OPEN;
    if (c == 'y' || c == 'Y') return KEY_YANK;
    if (c == 'r' || c == 'R') return KEY_RELOAD;
    if (c == 'f' || c == 'F') return KEY_FILTER_FILES;
    if (c == '/') return KEY_FILTER;

    if (c == '\033') {
        KeyPress k = read_escape_sequence();
        /* In normal mode, ESC alone or an unrecognized sequence quits. */
        return (k == KEY_ESC || k == KEY_NONE) ? KEY_QUIT : k;
    }
    return KEY_NONE;
}

/* ============================================================================
 * Selection State
 * ============================================================================ */

typedef struct {
    View *view;            /* The rows on display (view.c owns the policy) */
    int cursor;            /* Current cursor position (index into view->rows) */
    int scroll_offset;     /* First visible line */
    int term_rows;         /* Terminal height */
    int visible_lines;     /* Lines currently displayed */
    int first_render;      /* Is this the first render? */
    int filter_mode;       /* Currently typing an interactive filter */
    int filter_len;        /* Length of the filter query */
    char filter[256];      /* Interactive substring/glob filter query */
} SelectState;

static int row_count(const SelectState *state) {
    return (int)state->view->count;
}

static TreeNode *row_node(const SelectState *state, int index) {
    return state->view->rows[index].node;
}

/* Rebuild the View after anything changes what should be shown (expansion,
 * filter, reload): refresh match flags for the active query (or restore the
 * CLI -f pattern's so its flags aren't left holding our query's results),
 * build the rows through the shared engine, and repoint the render context
 * at the new columns and diff widths. */
static void picker_rebuild(SelectState *state, TreeNode **trees, int tree_count,
                           PrintContext *ctx) {
    int live_filter = state->filter_len > 0;
    const char *pattern = live_filter ? state->filter
                                      : ctx->cfg->req.grep_pattern;
    if (pattern) {
        for (int i = 0; i < tree_count; i++) {
            compute_grep_flags(trees[i], pattern);
        }
    }

    view_free(state->view);
    ViewOptions vo = {
        .interactive = 1,
        .live_filter = live_filter,
        .filter_depth = ctx->cfg->req.max_depth,
    };
    state->view = view_build_opts(trees, tree_count, ctx->cfg, ctx->git,
                                  ctx->icons, &vo);

    ctx->columns = ctx->cfg->disp.long_format ? state->view->cols : NULL;
    ctx->diff_add_width = state->view->diff_add_width;
    ctx->diff_del_width = state->view->diff_del_width;
}

/* ============================================================================
 * Cursor Placement
 * ============================================================================ */

/* Put the cursor on a specific node (pointer identity; nodes are stable
 * across everything except a reload). Keeps the old position if absent. */
static void cursor_to_node(SelectState *state, const TreeNode *node) {
    for (int i = 0; i < row_count(state); i++) {
        if (row_node(state, i) == node) {
            state->cursor = i;
            return;
        }
    }
}

/* Put the cursor on a path after a reload: exact match, else the deepest
 * visible ancestor, else clamp the current index. */
static void cursor_to_path(SelectState *state, const char *path) {
    if (!path) return;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < row_count(state); i++) {
        const char *row_path = row_node(state, i)->entry.path;
        size_t len = strlen(row_path);
        if (strcmp(row_path, path) == 0) {
            state->cursor = i;
            return;
        }
        if (len > best_len && strncmp(path, row_path, len) == 0 &&
            path[len] == '/') {
            best = i;
            best_len = len;
        }
    }
    if (best >= 0) state->cursor = best;
}

/* Find next file index (skipping directories), returns -1 if none found */
static int find_next_file(SelectState *state, int from, int direction) {
    int count = row_count(state);
    if (count == 0) return -1;

    for (int i = 1; i < count; i++) {
        int idx = (from + i * direction + count) % count;
        if (!node_is_directory(row_node(state, idx))) {
            return idx;
        }
    }
    return -1;  /* No files found */
}

/* Snap cursor to nearest valid position. In files_only mode, finds the
 * closest file entry; otherwise just clamps to bounds. */
static void snap_cursor(SelectState *state, int files_only) {
    int count = row_count(state);
    if (count == 0) { state->cursor = 0; return; }
    if (state->cursor >= count)
        state->cursor = count - 1;

    if (!files_only || !node_is_directory(row_node(state, state->cursor)))
        return;

    /* Search forward and backward for the nearest file */
    int fwd = -1, bwd = -1;
    for (int i = state->cursor + 1; i < count; i++) {
        if (!node_is_directory(row_node(state, i))) { fwd = i; break; }
    }
    for (int i = state->cursor - 1; i >= 0; i--) {
        if (!node_is_directory(row_node(state, i))) { bwd = i; break; }
    }
    if (fwd >= 0 && bwd >= 0)
        state->cursor = (fwd - state->cursor <= state->cursor - bwd) ? fwd : bwd;
    else if (fwd >= 0)
        state->cursor = fwd;
    else if (bwd >= 0)
        state->cursor = bwd;
    /* else: no files at all, stay put */
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

static void get_terminal_size(int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row;
    } else {
        *rows = 24;
    }
}

static void render_line(SelectState *state, int index, int is_selected,
                        PrintContext *ctx) {
    const ViewRow *row = &state->view->rows[index];

    /* Clear line */
    printf("\r\033[K");

    /* Set up the line prefix (cursor indicator) */
    static char prefix_buf[64];
    const char *cursor_icon = ctx->icons->cursor;
    if (is_selected) {
        snprintf(prefix_buf, sizeof(prefix_buf), "%s%s%s ", COLOR_CYAN, cursor_icon, COLOR_RESET);
    } else {
        /* Pad with spaces to match cursor icon width (assume single-width for now) */
        snprintf(prefix_buf, sizeof(prefix_buf), "  ");
    }

    /* Decode the row's continuation mask into the context's array */
    for (int d = 0; d < row->depth && d < L_MAX_DEPTH; d++) {
        ctx->continuation[d] = (row->cont_mask >> d) & 1;
    }

    PrintContext line_ctx = *ctx;
    line_ctx.line_prefix = prefix_buf;
    line_ctx.selected = is_selected;

    print_entry(&row->node->entry, row->depth, row->expanded, &line_ctx);
}

static void render_picker(SelectState *state, PrintContext *ctx, int files_only) {
    int count = row_count(state);
    int max_visible = state->term_rows - 2;  /* Leave room for status line */
    if (max_visible < 1) max_visible = 1;
    if (max_visible > count) max_visible = count;

    /* Adjust scroll to keep cursor visible */
    if (state->cursor < state->scroll_offset) {
        state->scroll_offset = state->cursor;
    } else if (state->cursor >= state->scroll_offset + max_visible) {
        state->scroll_offset = state->cursor - max_visible + 1;
    }

    /* Move cursor up to top of our display area (if not first render) */
    int old_visible = state->visible_lines;
    if (!state->first_render && old_visible > 1) {
        printf("\033[%dA", old_visible - 1);
    }
    state->first_render = 0;

    /* Render visible lines */
    int end = state->scroll_offset + max_visible;
    if (end > count) end = count;
    int new_visible = (end - state->scroll_offset) + 1;  /* +1 for status */

    for (int i = state->scroll_offset; i < end; i++) {
        render_line(state, i, i == state->cursor, ctx);
    }

    /* Status line, clamped to the terminal width: line wrap is disabled, so
     * overflow would repeatedly overwrite the last column instead of
     * truncating cleanly. */
    char status[512];
    if (state->filter_mode) {
        snprintf(status, sizeof(status),
                 "%s/%s%s   %s%d match%s   [Enter] select  [Esc] cancel%s",
                 COLOR_CYAN, COLOR_RESET, state->filter,
                 COLOR_GREY, count, count == 1 ? "" : "es",
                 COLOR_RESET);
    } else if (files_only) {
        snprintf(status, sizeof(status),
                 "%s[j/k] files  [f] all  [/] filter  [h/l] fold  [o] open  [y] yank  [r] reload  [Enter] select  [q] quit%s",
                 COLOR_GREY, COLOR_RESET);
    } else {
        snprintf(status, sizeof(status),
                 "%s[j/k] move  [f] files  [/] filter  [h/l] fold  [o] open  [y] yank  [r] reload  [Enter] select  [q] quit%s",
                 COLOR_GREY, COLOR_RESET);
    }
    printf("\r\033[K");
    if (ctx->term_width > 0 && visible_strlen(status) > ctx->term_width) {
        char *truncated = truncate_visible(status, ctx->term_width);
        printf("%s", truncated);
        free(truncated);
    } else {
        printf("%s", status);
    }

    /* Clear any extra lines from previous render (when tree shrinks) */
    if (old_visible > new_visible) {
        for (int i = 0; i < old_visible - new_visible; i++) {
            printf("\n\033[K");
        }
        /* Move back up to status line position */
        printf("\033[%dA", old_visible - new_visible);
    }

    fflush(stdout);

    /* Track how many lines we printed */
    state->visible_lines = new_visible;
}

/* ============================================================================
 * Opening Files
 * ============================================================================ */

/* Check if file should be opened with system handler vs EDITOR */
static int should_open_externally(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    ext++;  /* skip the dot */

    /* Images */
    if (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
        strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
        strcasecmp(ext, "bmp") == 0 || strcasecmp(ext, "tiff") == 0 ||
        strcasecmp(ext, "tif") == 0 || strcasecmp(ext, "webp") == 0 ||
        strcasecmp(ext, "svg") == 0 || strcasecmp(ext, "ico") == 0 ||
        strcasecmp(ext, "heic") == 0 || strcasecmp(ext, "heif") == 0 ||
        strcasecmp(ext, "raw") == 0 || strcasecmp(ext, "psd") == 0)
        return 1;

    /* Audio */
    if (strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "wav") == 0 ||
        strcasecmp(ext, "flac") == 0 || strcasecmp(ext, "aac") == 0 ||
        strcasecmp(ext, "ogg") == 0 || strcasecmp(ext, "m4a") == 0 ||
        strcasecmp(ext, "wma") == 0 || strcasecmp(ext, "aiff") == 0)
        return 1;

    /* Video */
    if (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
        strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
        strcasecmp(ext, "wmv") == 0 || strcasecmp(ext, "flv") == 0 ||
        strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "m4v") == 0)
        return 1;

    /* Documents */
    if (strcasecmp(ext, "pdf") == 0 || strcasecmp(ext, "doc") == 0 ||
        strcasecmp(ext, "docx") == 0 || strcasecmp(ext, "xls") == 0 ||
        strcasecmp(ext, "xlsx") == 0 || strcasecmp(ext, "ppt") == 0 ||
        strcasecmp(ext, "pptx") == 0 || strcasecmp(ext, "odt") == 0 ||
        strcasecmp(ext, "ods") == 0 || strcasecmp(ext, "odp") == 0 ||
        strcasecmp(ext, "pages") == 0 || strcasecmp(ext, "numbers") == 0 ||
        strcasecmp(ext, "key") == 0)
        return 1;

    /* Archives */
    if (strcasecmp(ext, "zip") == 0 || strcasecmp(ext, "tar") == 0 ||
        strcasecmp(ext, "gz") == 0 || strcasecmp(ext, "rar") == 0 ||
        strcasecmp(ext, "7z") == 0 || strcasecmp(ext, "dmg") == 0)
        return 1;

    /* Other binary */
    if (strcasecmp(ext, "exe") == 0 || strcasecmp(ext, "app") == 0 ||
        strcasecmp(ext, "dll") == 0 || strcasecmp(ext, "so") == 0 ||
        strcasecmp(ext, "dylib") == 0 || strcasecmp(ext, "o") == 0 ||
        strcasecmp(ext, "a") == 0)
        return 1;

    return 0;
}

static void copy_to_clipboard(const char *text) {
#ifdef __APPLE__
    FILE *pbcopy = popen("pbcopy", "w");
    if (pbcopy) {
        fputs(text, pbcopy);
        pclose(pbcopy);
    }
#else
    /* Linux: try xclip or xsel */
    FILE *clip = popen("xclip -selection clipboard 2>/dev/null || xsel --clipboard 2>/dev/null", "w");
    if (clip) {
        fputs(text, clip);
        pclose(clip);
    }
#endif
}

/* ============================================================================
 * Main Selection Loop
 * ============================================================================ */

char *select_run(TreeNode ***trees_ref, int tree_count, char *const *dirs,
                 PrintContext *ctx) {
    TreeNode **trees = *trees_ref;

    SelectState state;
    memset(&state, 0, sizeof(state));
    state.term_rows = 24;
    state.first_render = 1;

    /* Filter mode: 0 = all, 1 = files only */
    int files_only = 0;

    /* We need a continuation array for rendering */
    int continuation[L_MAX_DEPTH] = {0};

    /* Set up our own render context (the caller's stays untouched) */
    PrintContext render_ctx = *ctx;
    render_ctx.continuation = continuation;
    render_ctx.line_prefix = NULL;

    /* Build the initial view */
    picker_rebuild(&state, trees, tree_count, &render_ctx);

    if (row_count(&state) == 0) {
        view_free(state.view);
        return NULL;
    }

    /* Redirect stdout to /dev/tty so UI output doesn't mix with the
     * result path when stdout is captured (e.g. in a shell widget).
     * The original stdout fd is restored after cleanup so the caller
     * can still printf the selected path to the real stdout.
     *
     * This happens before the terminal-size queries below: they read
     * STDOUT_FILENO, so they must run after the redirect. Otherwise, when
     * stdout is a captured pipe, the ioctl fails and the picker falls back
     * to a bogus 24x80, desyncing the cursor math from the real terminal. */
    int tty_fd = open("/dev/tty", O_WRONLY);
    if (tty_fd >= 0) {
        saved_stdout_fd = dup(STDOUT_FILENO);
        dup2(tty_fd, STDOUT_FILENO);
        close(tty_fd);
    }

    get_terminal_size(&state.term_rows);
    render_ctx.term_width = get_terminal_width();

    /* Enter raw mode and render */
    term_enable_raw();
    render_picker(&state, &render_ctx, files_only);

    char *result = NULL;
    char *yanked = NULL;   /* confirmation printed after the display is erased */

    while (1) {
        /* Exit if the view is empty. Stay while filtering so a query that
         * currently matches nothing can still be edited/cleared. */
        if (row_count(&state) == 0 && !state.filter_mode) break;

        if (sigint_received) goto cleanup;
        int ready = poll_stdin();
        if (sigint_received) goto cleanup;
        if (ready <= 0) continue;  /* signal interrupted poll */

        char typed = 0;
        KeyPress key = term_read_key(state.filter_mode, &typed);
        TreeNode *current = row_count(&state) > 0
            ? row_node(&state, state.cursor) : NULL;

        switch (key) {
            case KEY_UP:
                if (row_count(&state) == 0) break;
                if (files_only) {
                    int next = find_next_file(&state, state.cursor, -1);
                    if (next >= 0) state.cursor = next;
                } else {
                    state.cursor = (state.cursor - 1 + row_count(&state)) % row_count(&state);
                }
                render_picker(&state, &render_ctx, files_only);
                break;

            case KEY_DOWN:
                if (row_count(&state) == 0) break;
                if (files_only) {
                    int next = find_next_file(&state, state.cursor, 1);
                    if (next >= 0) state.cursor = next;
                } else {
                    state.cursor = (state.cursor + 1) % row_count(&state);
                }
                render_picker(&state, &render_ctx, files_only);
                break;

            case KEY_LEFT:
                /* Collapse current directory */
                if (current && node_is_directory(current) && current->ui_expanded) {
                    current->ui_expanded = 0;
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    cursor_to_node(&state, current);
                    snap_cursor(&state, files_only);
                    render_picker(&state, &render_ctx, files_only);
                }
                break;

            case KEY_RIGHT:
                /* Expand current directory (materialize children on first open) */
                if (current && node_is_directory(current) && !current->ui_expanded) {
                    if (current->child_count == 0 && !current->was_expanded) {
                        tree_expand_node_from_config(current, render_ctx.git,
                                                     render_ctx.cfg, render_ctx.icons);
                    }
                    current->ui_expanded = 1;
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    cursor_to_node(&state, current);
                    snap_cursor(&state, files_only);
                    render_picker(&state, &render_ctx, files_only);
                }
                break;

            case KEY_RELOAD: {
                /* Rebuild everything from scratch — identical to quitting and
                 * re-running l -i, with the cursor put back by path. Clears
                 * any live query (a fresh run has none). */
                char *saved = current ? xstrdup(current->entry.path) : NULL;
                state.filter_mode = 0;
                state.filter_len = 0;
                state.filter[0] = '\0';

                view_free(state.view);
                state.view = NULL;
                forest_free(trees, tree_count);
                git_cache_free(render_ctx.git);
                git_cache_init(render_ctx.git);
                trees = forest_build(dirs, tree_count, render_ctx.cfg,
                                     render_ctx.git, render_ctx.icons);
                *trees_ref = trees;

                picker_rebuild(&state, trees, tree_count, &render_ctx);
                state.cursor = 0;
                state.scroll_offset = 0;
                cursor_to_path(&state, saved);
                free(saved);
                snap_cursor(&state, files_only);
                get_terminal_size(&state.term_rows);
                render_picker(&state, &render_ctx, files_only);
                break;
            }

            case KEY_FILTER_FILES:
                if (!current) break;
                if (!files_only) {
                    /* Check if there are any files before enabling */
                    int has_files = find_next_file(&state, state.cursor, 1) >= 0 ||
                                    !node_is_directory(current);
                    if (!has_files) {
                        /* No files - ignore */
                        break;
                    }
                    files_only = 1;
                    /* Move to next file if on a directory */
                    if (node_is_directory(current)) {
                        int next = find_next_file(&state, state.cursor, 1);
                        if (next >= 0) state.cursor = next;
                    }
                } else {
                    files_only = 0;
                }
                render_picker(&state, &render_ctx, files_only);
                break;

            case KEY_OPEN:
                if (!current) break;
                if (node_is_directory(current)) {
                    /* Toggle expand/collapse */
                    if (current->ui_expanded) {
                        current->ui_expanded = 0;
                    } else {
                        if (current->child_count == 0 && !current->was_expanded) {
                            tree_expand_node_from_config(current, render_ctx.git,
                                                         render_ctx.cfg, render_ctx.icons);
                        }
                        current->ui_expanded = 1;
                    }
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    cursor_to_node(&state, current);
                    snap_cursor(&state, files_only);
                    render_picker(&state, &render_ctx, files_only);
                } else {
                    /* Open file: use system handler for binary, EDITOR for
                     * text. The path is bound as $0 so its characters are
                     * data, never shell syntax (interpolating it into the
                     * command line was an injection vector: a quote in a
                     * filename escaped the quoting). */
                    char cmd[PATH_MAX + 64];

                    printf("\r\033[K\n");
                    term_disable_raw();

                    if (should_open_externally(current->entry.path)) {
#ifdef PLATFORM_MACOS
                        snprintf(cmd, sizeof(cmd), "open \"$0\"");
#else
                        snprintf(cmd, sizeof(cmd), "xdg-open \"$0\" 2>/dev/null");
#endif
                    } else {
                        const char *editor = getenv("EDITOR");
                        if (!editor) editor = "vim";
                        snprintf(cmd, sizeof(cmd), "%s \"$0\"", editor);
                    }
                    pid_t pid = fork();
                    if (pid == 0) {
                        execl("/bin/sh", "sh", "-c", cmd,
                              current->entry.path, (char *)NULL);
                        _exit(127);
                    } else if (pid > 0) {
                        int status;
                        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    }

                    /* Restore original stdout */
                    if (saved_stdout_fd >= 0) {
                        fflush(stdout);
                        dup2(saved_stdout_fd, STDOUT_FILENO);
                        close(saved_stdout_fd);
                        saved_stdout_fd = -1;
                    }

                    view_free(state.view);
                    return NULL;
                }
                break;

            case KEY_ENTER:
                if (!current) break;  /* filtering with no matches: do nothing */
                result = xstrdup(current->entry.path);
                goto cleanup;

            case KEY_YANK: {
                if (!current) break;
                copy_to_clipboard(current->entry.path);
                /* Don't print here: the erase logic below assumes the cursor
                 * still sits on the status line. The confirmation goes out
                 * after the display is cleared. An empty (non-NULL) result
                 * means "action completed, nothing to select" — the caller
                 * prints nothing and exits 0. */
                yanked = xstrdup(current->entry.path);
                result = xstrdup("");
                goto cleanup;
            }

            case KEY_FILTER:
                /* '/' opens the interactive filter; an existing query is kept
                 * so it can be edited rather than retyped. */
                state.filter_mode = 1;
                render_picker(&state, &render_ctx, files_only);
                break;

            case KEY_CHAR:
                if (state.filter_len < (int)sizeof(state.filter) - 1) {
                    state.filter[state.filter_len++] = typed;
                    state.filter[state.filter_len] = '\0';
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    state.cursor = 0;
                    state.scroll_offset = 0;
                    render_picker(&state, &render_ctx, files_only);
                }
                break;

            case KEY_BACKSPACE:
                if (state.filter_len > 0) {
                    state.filter[--state.filter_len] = '\0';
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    state.cursor = 0;
                    state.scroll_offset = 0;
                    render_picker(&state, &render_ctx, files_only);
                }
                break;

            case KEY_ESC:
                /* Leave filter mode and restore the unfiltered list. */
                state.filter_mode = 0;
                if (state.filter_len > 0) {
                    state.filter_len = 0;
                    state.filter[0] = '\0';
                    picker_rebuild(&state, trees, tree_count, &render_ctx);
                    state.cursor = 0;
                    state.scroll_offset = 0;
                }
                render_picker(&state, &render_ctx, files_only);
                break;

            case KEY_QUIT:
                goto cleanup;

            default:
                break;
        }
    }

cleanup:
    /* Erase the entire interactive display */
    if (state.visible_lines > 1) {
        printf("\033[%dA", state.visible_lines - 1);  /* Move to top */
    }
    for (int i = 0; i < state.visible_lines; i++) {
        printf("\r\033[K\n");  /* Clear each line */
    }
    if (state.visible_lines > 0) {
        printf("\033[%dA", state.visible_lines);  /* Move back up */
    }
    if (yanked) {
        /* Label colored, path plain — same style as the error messages */
        printf("%sYanked:%s %s\n", COLOR_GREEN, COLOR_RESET, yanked);
        free(yanked);
    }
    fflush(stdout);
    term_disable_raw();

    /* Restore original stdout so the caller can print the result path */
    if (saved_stdout_fd >= 0) {
        fflush(stdout);
        dup2(saved_stdout_fd, STDOUT_FILENO);
        close(saved_stdout_fd);
        saved_stdout_fd = -1;
    }

    view_free(state.view);
    return result;
}
