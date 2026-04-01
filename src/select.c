/*
 * select.c - Interactive file selection mode
 */

#include "select.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

/* ============================================================================
 * Terminal Handling
 * ============================================================================ */

static struct termios orig_termios;
static int raw_mode_enabled = 0;
static int saved_stdout_fd = -1;  /* Original stdout, saved before tty redirect */
static int sigint_visible_lines = 0;  /* Lines on screen, for cleanup in signal handler */

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
    KEY_FILTER_FILES
} KeyPress;

/* Wait for stdin to become readable within timeout_ms.
 * Returns 1 if readable, 0 on timeout, -1 on error. */
static int poll_stdin(int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    return poll(&pfd, 1, timeout_ms);
}

static KeyPress term_read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (c == '\t') return KEY_DOWN;          /* Tab cycles down */
    if (c == 3) return KEY_QUIT;              /* Ctrl+C */
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
                    if (seq[1] == 'Z') return KEY_UP;   /* Shift+Tab */
                }
            }
        }
        /* ESC alone or unrecognized sequence */
        return KEY_QUIT;
    }
    return KEY_NONE;
}

/* ============================================================================
 * Expanded Directory Set - external UI state tracking which dirs are open
 * ============================================================================ */

#define MAX_EXPANDED 1024

typedef struct {
    char *paths[MAX_EXPANDED];
    int count;
} ExpandedSet;

static void expanded_init(ExpandedSet *set) {
    set->count = 0;
}

static void expanded_free(ExpandedSet *set) {
    for (int i = 0; i < set->count; i++) {
        free(set->paths[i]);
    }
    set->count = 0;
}

static int expanded_contains(ExpandedSet *set, const char *path) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->paths[i], path) == 0) {
            return 1;
        }
    }
    return 0;
}

static void expanded_add(ExpandedSet *set, const char *path) {
    if (expanded_contains(set, path)) return;
    if (set->count < MAX_EXPANDED) {
        char *dup = strdup(path);
        if (dup) set->paths[set->count++] = dup;
    }
}

static void expanded_remove(ExpandedSet *set, const char *path) {
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->paths[i], path) == 0) {
            free(set->paths[i]);
            if (i < set->count - 1) {
                set->paths[i] = set->paths[set->count - 1];
            }
            set->count--;
            return;
        }
    }
}

/* Seed the expanded set from the initially-built tree (dirs that were expanded
 * by the initial depth-limited scan). */
static void expanded_seed_from_tree(ExpandedSet *set, TreeNode *node) {
    if (node_is_directory(node) && node->was_expanded) {
        expanded_add(set, node->entry.path);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        expanded_seed_from_tree(set, &node->children[i]);
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

/* Count visible children for a node (only if expanded).
 * Filtering only applies within the initial tree depth; nodes expanded
 * interactively beyond that depth are always visible. */
static int count_visible_children(const TreeNode *node, const Config *cfg,
                                   ExpandedSet *expanded, int depth,
                                   int filter_depth) {
    if (!node_is_directory(node)) return 0;
    if (!expanded_contains(expanded, node->entry.path)) return 0;
    int filtering = (depth < filter_depth) && is_filtering_active(cfg);
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
                             ExpandedSet *expanded, int filter_depth);

static void flatten_node(SelectState *state, TreeNode *node, int depth,
                         int *continuation, const Config *cfg,
                         ExpandedSet *expanded, int filter_depth) {
    int has_visible_children = count_visible_children(node, cfg, expanded,
                                                      depth, filter_depth) > 0;

    /* Add this node */
    state_add(state, node, depth, has_visible_children, continuation);

    /* Recurse into children only if this dir is in the expanded set */
    int is_expanded = expanded_contains(expanded, node->entry.path);
    if (node->child_count > 0 && is_expanded) {
        flatten_children(state, node, depth, continuation, cfg, expanded,
                         filter_depth);
    }
}

static void flatten_children(SelectState *state, const TreeNode *parent,
                             int depth, int *continuation, const Config *cfg,
                             ExpandedSet *expanded, int filter_depth) {
    if (parent->child_count == 0) return;

    /* Filter only within the original tree depth; interactively expanded
     * nodes beyond that are always visible. */
    int filtering = (depth < filter_depth) && is_filtering_active(cfg);

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

        flatten_node(state, child, depth + 1, continuation, cfg, expanded,
                     filter_depth);
    }

    free(visible_indices);
}

static void flatten_all(SelectState *state, TreeNode **trees, int tree_count,
                        const Config *cfg, ExpandedSet *expanded) {
    int continuation[L_MAX_DEPTH] = {0};
    state_clear(state);
    for (int i = 0; i < tree_count; i++) {
        memset(continuation, 0, sizeof(continuation));
        flatten_node(state, trees[i], 0, continuation, cfg, expanded,
                     cfg->max_depth);
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
 * Live Refresh
 * ============================================================================ */

#define REFRESH_INTERVAL_MS  100
#define GIT_REFRESH_INTERVAL_MS  5000

static struct timespec timespec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static long timespec_diff_ms(struct timespec a, struct timespec b) {
    return (a.tv_sec - b.tv_sec) * 1000 + (a.tv_nsec - b.tv_nsec) / 1000000;
}

/* Recursively expand all dirs in the expanded set under a given node */
static void expand_from_set(TreeNode *node, ExpandedSet *expanded,
                            const TreeBuildOpts *opts, GitCache *git,
                            const Icons *icons) {
    for (size_t i = 0; i < node->child_count; i++) {
        TreeNode *child = &node->children[i];
        if (node_is_directory(child) && expanded_contains(expanded, child->entry.path)) {
            tree_expand_node(child, opts, git, icons);
            expand_from_set(child, expanded, opts, git, icons);
        }
    }
}

/* Recursively check and rescan a node and its expanded children (top-down).
 * Walks the live tree, not the flattened state, so rescan is safe. */
static int rescan_recursive(TreeNode *node, ExpandedSet *expanded,
                            const TreeBuildOpts *opts, GitCache *git,
                            const Icons *icons) {
    if (!node_is_directory(node)) return 0;
    if (!expanded_contains(expanded, node->entry.path)) return 0;

    struct stat st;
    if (stat(node->entry.path, &st) != 0) return 0;

    time_t new_mtime = GET_MTIME(st);
    if (new_mtime != node->entry.mtime) {
        /* This node changed — rescan it, which rebuilds all children.
         * expand_from_set re-expands any children in the expanded set,
         * recursively, so we don't need to recurse further here. */
        node->entry.mtime = new_mtime;
        tree_rescan_node(node, opts, git, icons);
        expand_from_set(node, expanded, opts, git, icons);
        return 1;
    }

    /* No change at this level — check children */
    int changed = 0;
    for (size_t i = 0; i < node->child_count; i++) {
        changed |= rescan_recursive(&node->children[i], expanded, opts, git, icons);
    }
    return changed;
}

/* Check expanded directories for mtime changes. Rescan any that changed.
 * When refresh_git is set, force rescan of all expanded dirs with git status.
 * Returns 1 if anything was rescanned. */
static int check_and_rescan(SelectState *state, TreeNode **trees, int tree_count,
                            PrintContext *ctx, ExpandedSet *expanded,
                            int refresh_git) {
    TreeBuildOpts opts = config_to_build_opts(ctx->cfg);
    if (!refresh_git) {
        opts.compute.git_status = 0;
        opts.compute.git_diff = 0;
    }

    int changed = 0;
    for (int i = 0; i < tree_count; i++) {
        changed |= rescan_recursive(trees[i], expanded, &opts, ctx->git, ctx->icons);
    }

    if (changed) {
        flatten_all(state, trees, tree_count, ctx->cfg, expanded);
        recalculate_columns(state, ctx->columns, ctx->icons);

        /* Remove stale paths (deleted directories) from expanded set */
        for (int i = expanded->count - 1; i >= 0; i--) {
            struct stat st;
            if (stat(expanded->paths[i], &st) != 0) {
                free(expanded->paths[i]);
                if (i < expanded->count - 1)
                    expanded->paths[i] = expanded->paths[expanded->count - 1];
                expanded->count--;
            }
        }
    }

    return changed;
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
                        PrintContext *ctx, ExpandedSet *expanded) {
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

    /* Determine expansion state from the external set */
    int has_visible = item->has_visible_children;
    int is_expanded = node_is_directory(item->node) &&
                      expanded_contains(expanded, item->node->entry.path);

    /* Create a modified context with the line prefix */
    PrintContext line_ctx = *ctx;
    line_ctx.line_prefix = prefix;
    line_ctx.continuation = ctx->continuation;
    line_ctx.selected = is_selected;

    /* Call the real print_entry */
    print_entry(&item->node->entry, item->depth, is_expanded, has_visible, &line_ctx);
}

static void render_view(SelectState *state, PrintContext *ctx, ExpandedSet *expanded, int files_only) {
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
        render_line(state, i, i == state->cursor, ctx, expanded);
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
    sigint_visible_lines = new_visible;
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

/* Snap cursor to nearest valid position. In files_only mode, finds the
 * closest file entry; otherwise just clamps to bounds. */
static void snap_cursor(SelectState *state, int files_only) {
    if (state->count == 0) { state->cursor = 0; return; }
    if (state->cursor >= state->count)
        state->cursor = state->count - 1;

    if (!files_only || !node_is_directory(state->items[state->cursor].node))
        return;

    /* Search forward and backward for the nearest file */
    int fwd = -1, bwd = -1;
    for (int i = state->cursor + 1; i < state->count; i++) {
        if (!node_is_directory(state->items[i].node)) { fwd = i; break; }
    }
    for (int i = state->cursor - 1; i >= 0; i--) {
        if (!node_is_directory(state->items[i].node)) { bwd = i; break; }
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

    ExpandedSet expanded;
    expanded_init(&expanded);

    /* Seed expanded set from the initially-built tree */
    for (int i = 0; i < tree_count; i++) {
        expanded_seed_from_tree(&expanded, trees[i]);
    }

    /* Filter mode: 0 = all, 1 = files only */
    int files_only = 0;

    /* We need a continuation array for rendering */
    int continuation[L_MAX_DEPTH] = {0};

    /* Flatten all trees into visible node list (with filter applied) */
    flatten_all(&state, trees, tree_count, ctx->cfg, &expanded);

    if (state.count == 0) {
        state_free(&state);
        expanded_free(&expanded);
        return NULL;
    }

    /* Get terminal size */
    get_terminal_size(&state.term_rows);

    /* Set up context with our continuation array */
    PrintContext render_ctx = *ctx;
    render_ctx.continuation = continuation;
    render_ctx.line_prefix = NULL;
    render_ctx.term_width = get_terminal_width();

    /* Redirect stdout to /dev/tty so UI output doesn't mix with the
     * result path when stdout is captured (e.g. in a shell widget).
     * The original stdout fd is restored after cleanup so the caller
     * can still printf the selected path to the real stdout. */
    int tty_fd = open("/dev/tty", O_WRONLY);
    if (tty_fd >= 0) {
        saved_stdout_fd = dup(STDOUT_FILENO);
        dup2(tty_fd, STDOUT_FILENO);
        close(tty_fd);
    }

    /* Enter raw mode and render */
    term_enable_raw();
    render_view(&state, &render_ctx, &expanded, files_only);

    char *result = NULL;

    /* Live refresh timers */
    struct timespec last_git_refresh = timespec_now();

    while (1) {
        /* Safety check - exit if tree becomes empty */
        if (state.count == 0) break;

        /* Check for Ctrl+C */
        if (sigint_received) goto cleanup;

        /* Poll stdin with timeout for live refresh */
        int ready = poll_stdin(REFRESH_INTERVAL_MS);
        if (sigint_received) goto cleanup;
        if (ready == 0) {
            /* Check if root directories still exist */
            int any_alive = 0;
            for (int i = 0; i < tree_count; i++) {
                struct stat st;
                if (stat(trees[i]->entry.path, &st) == 0) {
                    any_alive = 1;
                    break;
                }
            }
            if (!any_alive) break;

            /* Timeout — check for filesystem changes */
            struct timespec now = timespec_now();
            int do_git = timespec_diff_ms(now, last_git_refresh) >= GIT_REFRESH_INTERVAL_MS;
            int old_cursor = state.cursor;
            char *cur_path = strdup(state.items[state.cursor].node->entry.path);

            if (check_and_rescan(&state, trees, tree_count, ctx, &expanded, do_git)) {
                if (do_git) last_git_refresh = now;

                /* Restore cursor: try same path, fall back to same position */
                int found = 0;
                if (cur_path) {
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) {
                    state.cursor = old_cursor;
                }
                snap_cursor(&state, files_only);

                get_terminal_size(&state.term_rows);
                render_view(&state, &render_ctx, &expanded, files_only);
            } else if (do_git) {
                last_git_refresh = now;
            }
            free(cur_path);
            continue;
        }
        if (ready < 0) continue;  /* Signal interrupted poll */

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
                render_view(&state, &render_ctx, &expanded, files_only);
                break;

            case KEY_DOWN:
                if (files_only) {
                    int next = find_next_file(&state, state.cursor, 1);
                    if (next >= 0) state.cursor = next;
                } else {
                    state.cursor = (state.cursor + 1) % state.count;
                }
                render_view(&state, &render_ctx, &expanded, files_only);
                break;

            case KEY_LEFT:
                /* Collapse current directory if it's expanded */
                if (node_is_directory(current->node) &&
                    expanded_contains(&expanded, current->node->entry.path)) {
                    expanded_remove(&expanded, current->node->entry.path);
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &expanded);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    state.cursor = 0;
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &expanded, files_only);
                }
                break;

            case KEY_RIGHT:
                /* Expand current directory */
                if (node_is_directory(current->node) &&
                    !expanded_contains(&expanded, current->node->entry.path)) {
                    /* Load children if not yet loaded */
                    if (current->node->child_count == 0 && !current->node->was_expanded) {
                        tree_expand_node_from_config(current->node, ctx->columns, ctx->git,
                                         ctx->cfg, ctx->icons);
                    }
                    expanded_add(&expanded, current->node->entry.path);
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &expanded);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    state.cursor = 0;
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &expanded, files_only);
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
                render_view(&state, &render_ctx, &expanded, files_only);
                break;

            case KEY_OPEN:
                if (node_is_directory(current->node)) {
                    /* Toggle expand/collapse */
                    if (expanded_contains(&expanded, current->node->entry.path)) {
                        expanded_remove(&expanded, current->node->entry.path);
                    } else {
                        /* Load children if not yet loaded */
                        if (current->node->child_count == 0 && !current->node->was_expanded) {
                            tree_expand_node_from_config(current->node, ctx->columns, ctx->git,
                                             ctx->cfg, ctx->icons);
                        }
                        expanded_add(&expanded, current->node->entry.path);
                    }
                    const char *cur_path = current->node->entry.path;
                    flatten_all(&state, trees, tree_count, ctx->cfg, &expanded);
                    recalculate_columns(&state, ctx->columns, ctx->icons);
                    state.cursor = 0;
                    for (int i = 0; i < state.count; i++) {
                        if (strcmp(state.items[i].node->entry.path, cur_path) == 0) {
                            state.cursor = i;
                            break;
                        }
                    }
                    render_view(&state, &render_ctx, &expanded, files_only);
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

                    /* Restore original stdout */
                    if (saved_stdout_fd >= 0) {
                        fflush(stdout);
                        dup2(saved_stdout_fd, STDOUT_FILENO);
                        close(saved_stdout_fd);
                        saved_stdout_fd = -1;
                    }

                    state_free(&state);
                    expanded_free(&expanded);
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
    fflush(stdout);
    term_disable_raw();

    /* Restore original stdout so the caller can print the result path */
    if (saved_stdout_fd >= 0) {
        fflush(stdout);
        dup2(saved_stdout_fd, STDOUT_FILENO);
        close(saved_stdout_fd);
        saved_stdout_fd = -1;
    }

    state_free(&state);
    expanded_free(&expanded);
    return result;
}
