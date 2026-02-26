/*
 * select.c - Interactive file selection mode
 */

#include "select.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>

/* ============================================================================
 * Terminal Handling
 * ============================================================================ */

static struct termios orig_termios;
static int raw_mode_enabled = 0;

static void term_disable_raw(void) {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033[?7h");   /* Re-enable line wrap */
    printf("\033[?25h");  /* Show cursor */
        fflush(stdout);
        raw_mode_enabled = 0;
    }
}

static void sigint_handler(int sig) {
    ssize_t r;
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        r = write(STDOUT_FILENO, "\033[?7h\033[?25h\n", 13);
    } else {
        r = write(STDOUT_FILENO, "\n", 1);
    }
    (void)r;
    _exit(128 + sig);
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
    raw.c_lflag &= ~(ECHO | ICANON);
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
    KEY_FILTER_FILES
} KeyPress;

static KeyPress term_read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == 'q' || c == 'Q') return KEY_QUIT;
    if (c == 'k' || c == 'K') return KEY_UP;
    if (c == 'j' || c == 'J') return KEY_DOWN;
    if (c == 'h' || c == 'H') return KEY_LEFT;
    if (c == 'l' || c == 'L') return KEY_RIGHT;
    if (c == 'o' || c == 'O') return KEY_OPEN;
    if (c == 'y' || c == 'Y') return KEY_YANK;
    if (c == 'f' || c == 'F') return KEY_FILTER_FILES;

    if (c == '\033') {
        /* Check if more data available (arrow key sequence) with timeout */
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
                }
            }
        }
        /* ESC alone or unrecognized sequence */
        return KEY_QUIT;
    }
    return KEY_NONE;
}

/* ============================================================================
 * Collapsed Directory Tracking
 * ============================================================================ */

#define MAX_COLLAPSED 256

typedef struct {
    char *paths[MAX_COLLAPSED];
    int count;
} CollapsedSet;

static void collapsed_init(CollapsedSet *set) {
    set->count = 0;
}

static void collapsed_free(CollapsedSet *set) {
    for (int i = 0; i < set->count; i++) {
        free(set->paths[i]);
    }
    set->count = 0;
}

static int collapsed_contains(CollapsedSet *set, const char *path) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void collapsed_toggle(CollapsedSet *set, const char *path) {
    /* If already collapsed, remove it (expand) */
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->paths[i], path) == 0) {
            free(set->paths[i]);
            /* Move last element to fill gap (unless removing last) */
            if (i < set->count - 1) {
                set->paths[i] = set->paths[set->count - 1];
            }
            set->count--;
            return;
        }
    }
    /* Not found, add it (collapse) */
    if (set->count < MAX_COLLAPSED) {
        char *dup = strdup(path);
        if (dup) set->paths[set->count++] = dup;
    }
}

/* ============================================================================
 * Selection State
 * ============================================================================ */

/* Info stored for each flattened node */
typedef struct {
    TreeNode *node;
    int depth;
    int has_visible_children;
    int continuation[L_MAX_DEPTH];  /* Copy of continuation state at this node */
} FlatNode;

typedef struct {
    FlatNode *items;
    int count;
    int capacity;
    int cursor;            /* Current cursor position */
    int scroll_offset;     /* First visible line */
    int term_rows;         /* Terminal height */
    int visible_lines;     /* Lines currently displayed */
    int first_render;      /* Is this the first render? */
} SelectState;

static void state_init(SelectState *state) {
    state->items = NULL;
    state->count = 0;
    state->capacity = 0;
    state->cursor = 0;
    state->scroll_offset = 0;
    state->term_rows = 24;
    state->visible_lines = 0;
    state->first_render = 1;
}

static void state_clear(SelectState *state) {
    state->count = 0;
    /* Keep capacity and allocated memory for reuse */
}

static void state_add(SelectState *state, TreeNode *node, int depth,
                      int has_visible_children, int *continuation) {
    if (state->count >= state->capacity) {
        int new_cap = state->capacity ? state->capacity * 2 : 256;
        FlatNode *new_items = realloc(state->items, new_cap * sizeof(FlatNode));
        if (!new_items) return;  /* Out of memory - skip this node */
        state->items = new_items;
        state->capacity = new_cap;
    }
    FlatNode *item = &state->items[state->count];
    item->node = node;
    item->depth = depth;
    item->has_visible_children = has_visible_children;
    memcpy(item->continuation, continuation, L_MAX_DEPTH * sizeof(int));
    state->count++;
}

static void state_free(SelectState *state) {
    free(state->items);
    state->items = NULL;
    state->count = 0;
    state->capacity = 0;
}

/* ============================================================================
 * Tree Flattening
 * ============================================================================ */

/* Count visible children for a node */
static int count_visible_children(const TreeNode *node, const Config *cfg,
                                   CollapsedSet *collapsed) {
    /* If collapsed, report no visible children */
    if (collapsed && collapsed_contains(collapsed, node->entry.path)) {
        return 0;
    }
    int filtering = is_filtering_active(cfg);
    int count = 0;
    for (size_t i = 0; i < node->child_count; i++) {
        if (!filtering || node_is_visible(&node->children[i], cfg)) {
            count++;
        }
    }
    return count;
}

static void flatten_children(SelectState *state, const TreeNode *parent,
                             int depth, int *continuation, const Config *cfg,
                             CollapsedSet *collapsed);

static void flatten_node(SelectState *state, TreeNode *node, int depth,
                         int *continuation, const Config *cfg,
                         CollapsedSet *collapsed) {
    int has_visible_children = count_visible_children(node, cfg, collapsed) > 0;

    /* Add this node */
    state_add(state, node, depth, has_visible_children, continuation);

    /* Recurse into children (unless collapsed) */
    int is_collapsed = collapsed && collapsed_contains(collapsed, node->entry.path);
    if (node->child_count > 0 && !is_collapsed) {
        flatten_children(state, node, depth, continuation, cfg, collapsed);
    }
}

static void flatten_children(SelectState *state, const TreeNode *parent,
                             int depth, int *continuation, const Config *cfg,
                             CollapsedSet *collapsed) {
    if (parent->child_count == 0) return;

    int filtering = is_filtering_active(cfg);

    /* Build list of visible children indices */
    size_t *visible_indices = malloc(parent->child_count * sizeof(size_t));
    if (!visible_indices) return;  /* Out of memory */
    size_t visible_count = 0;

    for (size_t i = 0; i < parent->child_count; i++) {
        if (!filtering || node_is_visible(&parent->children[i], cfg)) {
            visible_indices[visible_count++] = i;
        }
    }

    /* Flatten each visible child */
    for (size_t vi = 0; vi < visible_count; vi++) {
        size_t i = visible_indices[vi];
        TreeNode *child = &parent->children[i];
        int is_last = (vi == visible_count - 1);

        /* Set continuation for this depth */
        continuation[depth] = !is_last;

        flatten_node(state, child, depth + 1, continuation, cfg, collapsed);
    }

    free(visible_indices);
}

static void flatten_all(SelectState *state, TreeNode **trees, int tree_count,
                        const Config *cfg, CollapsedSet *collapsed) {
    int continuation[L_MAX_DEPTH] = {0};
    state_clear(state);
    for (int i = 0; i < tree_count; i++) {
        memset(continuation, 0, sizeof(continuation));
        flatten_node(state, trees[i], 0, continuation, cfg, collapsed);
    }
}

/* Recalculate column widths based on visible (flattened) items only */
static void recalculate_columns(SelectState *state, Column *cols, const Icons *icons) {
    if (!cols) return;  /* No columns in short format */
    /* Reset to minimum widths */
    for (int i = 0; i < NUM_COLUMNS; i++) {
        cols[i].width = 1;
    }
    /* Update based on visible items */
    for (int i = 0; i < state->count; i++) {
        columns_update_widths(cols, &state->items[i].node->entry, icons);
    }
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
                        PrintContext *ctx, CollapsedSet *collapsed) {
    FlatNode *item = &state->items[index];

    /* Clear line */
    printf("\r\033[K");

    /* Set up the line prefix (cursor indicator) */
    static char prefix_buf[64];
    const char *cursor_icon = ctx->icons->cursor[0] ? ctx->icons->cursor : ">";
    if (is_selected) {
        snprintf(prefix_buf, sizeof(prefix_buf), "%s%s%s ", COLOR_CYAN, cursor_icon, COLOR_RESET);
    } else {
        /* Pad with spaces to match cursor icon width (assume single-width for now) */
        snprintf(prefix_buf, sizeof(prefix_buf), "  ");
    }
    const char *prefix = prefix_buf;

    /* Copy continuation state into context */
    memcpy(ctx->continuation, item->continuation, L_MAX_DEPTH * sizeof(int));

    /* Adjust has_visible_children and is_expanded based on collapsed state */
    int has_visible = item->has_visible_children;
    int is_expanded = item->node->was_expanded;
    if (node_is_directory(item->node) && collapsed_contains(collapsed, item->node->entry.path)) {
        /* Show as having children even when collapsed (so we can expand) */
        has_visible = (item->node->child_count > 0);
        is_expanded = 0;  /* Show closed icon when collapsed */
    }

    /* Create a modified context with the line prefix */
    PrintContext line_ctx = *ctx;
    line_ctx.line_prefix = prefix;
    line_ctx.continuation = ctx->continuation;
    line_ctx.selected = is_selected;

    /* Call the real print_entry */
    print_entry(&item->node->entry, item->depth, is_expanded, has_visible, &line_ctx);
}

static void render_view(SelectState *state, PrintContext *ctx, CollapsedSet *collapsed, int files_only) {
    int max_visible = state->term_rows - 2;  /* Leave room for status line */
    if (max_visible < 1) max_visible = 1;
    if (max_visible > state->count) max_visible = state->count;

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
    if (end > state->count) end = state->count;
    int new_visible = (end - state->scroll_offset) + 1;  /* +1 for status */

    for (int i = state->scroll_offset; i < end; i++) {
        render_line(state, i, i == state->cursor, ctx, collapsed);
    }

    /* Status line */
    if (files_only) {
        printf("\r\033[K%s[j/k] files  [f] all  [h/l] fold  [o] open  [y] yank  [Enter] select  [q] quit%s",
               COLOR_GREY, COLOR_RESET);
    } else {
        printf("\r\033[K%s[j/k] move  [f] files  [h/l] fold  [o] open  [y] yank  [Enter] select  [q] quit%s",
               COLOR_GREY, COLOR_RESET);
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
 * Navigation Helpers
 * ============================================================================ */

/* Find next file index (skipping directories), returns -1 if none found */
static int find_next_file(SelectState *state, int from, int direction) {
    int count = state->count;
    if (count == 0) return -1;

    for (int i = 1; i < count; i++) {
        int idx = (from + i * direction + count) % count;
        if (!node_is_directory(state->items[idx].node)) {
            return idx;
        }
    }
    return -1;  /* No files found */
}

/* ============================================================================
 * Clipboard
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

char *select_run(TreeNode **trees, int tree_count, PrintContext *ctx) {
    SelectState state;
    state_init(&state);

    CollapsedSet collapsed;
    collapsed_init(&collapsed);

    /* Filter mode: 0 = all, 1 = files only */
    int files_only = 0;

    /* We need a continuation array for rendering */
    int continuation[L_MAX_DEPTH] = {0};

    /* Flatten all trees into visible node list */
    flatten_all(&state, trees, tree_count, ctx->cfg, &collapsed);

    if (state.count == 0) {
        state_free(&state);
        collapsed_free(&collapsed);
        return NULL;
    }

    /* Get terminal size */
    get_terminal_size(&state.term_rows);

    /* Set up context with our continuation array */
    PrintContext render_ctx = *ctx;
    render_ctx.continuation = continuation;
    render_ctx.line_prefix = NULL;
    render_ctx.term_width = get_terminal_width();

    /* Enter raw mode and render */
    term_enable_raw();
    render_view(&state, &render_ctx, &collapsed, files_only);

    char *result = NULL;

    while (1) {
        /* Safety check - exit if tree becomes empty */
        if (state.count == 0) break;

        KeyPress key = term_read_key();
        FlatNode *current = &state.items[state.cursor];

        switch (key) {
            case KEY_UP:
                if (files_only) {
                    int next = find_next_file(&state, state.cursor, -1);
                    if (next >= 0) state.cursor = next;
                } else {
                    state.cursor = (state.cursor - 1 + state.count) % state.count;
                }
                render_view(&state, &render_ctx, &collapsed, files_only);
                break;

            case KEY_DOWN:
                if (files_only) {
                    int next = find_next_file(&state, state.cursor, 1);
                    if (next >= 0) state.cursor = next;
                } else {
                    state.cursor = (state.cursor + 1) % state.count;
                }
                render_view(&state, &render_ctx, &collapsed, files_only);
                break;

            case KEY_LEFT:
                /* Collapse current directory (or parent if file/already collapsed) */
                if (node_is_directory(current->node) &&
                    !collapsed_contains(&collapsed, current->node->entry.path) &&
                    (current->node->child_count > 0 || current->node->was_expanded)) {
                    /* Collapse this directory (including empty expanded dirs) */
                    collapsed_toggle(&collapsed, current->node->entry.path);
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &collapsed);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    /* Find the cursor position again */
                    state.cursor = 0;  /* Default to first item */
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &collapsed, files_only);
                }
                break;

            case KEY_RIGHT:
                /* Expand current directory if collapsed or not loaded */
                if (node_is_directory(current->node)) {
                    if (collapsed_contains(&collapsed, current->node->entry.path)) {
                        /* Was manually collapsed - uncollapse */
                        collapsed_toggle(&collapsed, current->node->entry.path);
                    } else if (current->node->child_count == 0 && !current->node->was_expanded) {
                        /* Not loaded yet - dynamically expand */
                        tree_expand_node_from_config(current->node, ctx->columns, ctx->git,
                                         ctx->cfg, ctx->icons);
                    } else {
                        /* Already expanded (including empty dirs), nothing to do */
                        break;
                    }
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &collapsed);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    /* Find the cursor position again */
                    state.cursor = 0;  /* Default to first item */
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &collapsed, files_only);
                }
                break;

            case KEY_FILTER_FILES:
                if (!files_only) {
                    /* Check if there are any files before enabling */
                    int has_files = find_next_file(&state, state.cursor, 1) >= 0 ||
                                    !node_is_directory(current->node);
                    if (!has_files) {
                        /* No files - ignore */
                        break;
                    }
                    files_only = 1;
                    /* Move to next file if on a directory */
                    if (node_is_directory(current->node)) {
                        int next = find_next_file(&state, state.cursor, 1);
                        if (next >= 0) state.cursor = next;
                    }
                } else {
                    files_only = 0;
                }
                render_view(&state, &render_ctx, &collapsed, files_only);
                break;

            case KEY_OPEN:
                if (node_is_directory(current->node)) {
                    /* Toggle expand/collapse for directories */
                    if (current->node->child_count > 0 || current->node->was_expanded) {
                        /* Has children or was already expanded (empty dir) - toggle */
                        collapsed_toggle(&collapsed, current->node->entry.path);
                    } else {
                        /* Not yet expanded - dynamically expand */
                        tree_expand_node_from_config(current->node, ctx->columns, ctx->git,
                                         ctx->cfg, ctx->icons);
                    }
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &collapsed);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    /* Find the cursor position again */
                    state.cursor = 0;  /* Default to first item */
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &collapsed, files_only);
                } else {
                    /* Open file: use system handler for binary, EDITOR for text */
                    char cmd[PATH_MAX + 64];

                    printf("\r\033[K\n");
                    term_disable_raw();

                    if (should_open_externally(current->node->entry.path)) {
#ifdef PLATFORM_MACOS
                        snprintf(cmd, sizeof(cmd), "open \"%s\"",
                                 current->node->entry.path);
#else
                        snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" 2>/dev/null",
                                 current->node->entry.path);
#endif
                    } else {
                        const char *editor = getenv("EDITOR");
                        if (!editor) editor = "vim";
                        snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor,
                                 current->node->entry.path);
                    }
                    if (system(cmd)) { /* ignore */ }

                    state_free(&state);
                    collapsed_free(&collapsed);
                    return NULL;
                }
                break;

            case KEY_ENTER:
                result = strdup(current->node->entry.path);
                goto cleanup;

            case KEY_YANK: {
                copy_to_clipboard(current->node->entry.path);
                printf("\r\033[K%sYanked: %s%s\n", COLOR_GREEN,
                       current->node->entry.path, COLOR_RESET);
                fflush(stdout);
                goto cleanup;
            }

            case KEY_QUIT:
                goto cleanup;

            default:
                break;
        }
    }

cleanup:
    printf("\r\033[K\n");
    term_disable_raw();
    state_free(&state);
    collapsed_free(&collapsed);
    return result;
}
