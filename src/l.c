/*
 * l - Enhanced directory listing with tree view
 *
 * A fast, portable directory listing tool with tree visualization,
 * git integration, icons, and colors.
 */

#include "common.h"
#include "cache.h"
#include "git.h"
#include "ui.h"
#include "daemon.h"

#ifdef HAVE_LIBGIT2
#include <git2.h>

static void cleanup_libgit2(void) {
    git_libgit2_shutdown();
}
#endif

/* ============================================================================
 * Argument Parsing
 * ============================================================================ */

static int parse_depth(const char *str, const char *opt_name) {
    char *endptr;
    long val = strtol(str, &endptr, 10);
    char msg[128];
    if (*endptr != '\0' || endptr == str) {
        snprintf(msg, sizeof(msg), "%s requires an integer", opt_name);
        die(msg);
    }
    if (val < 0) {
        snprintf(msg, sizeof(msg), "%s requires a non-negative integer", opt_name);
        die(msg);
    }
    if (val > INT_MAX) val = INT_MAX;
    return (int)val;
}

static void print_usage(void) {
    printf("Usage: l [OPTIONS] [FILE ...]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -a              Show hidden files\n");
    printf("  -l, --long      Long format with size, lines, time (default)\n");
    printf("  -s, --short     Short format (no size, lines, time)\n");
    printf("                  Auto-enabled on network filesystems\n");
    printf("  -t, --tree      Show full tree (depth %d)\n", L_MAX_DEPTH);
    printf("  -d, --depth INT Limit tree depth\n");
    printf("  -p, --path      Show ancestry from ~ (or /) to target\n");
    printf("  --expand-all    Expand all directories (ignore skip list)\n");
    printf("  --list          Flat list output (no tree structure)\n");
    printf("  --no-icons      Hide file/folder/git icons\n");
    printf("  -g              Show only git-modified/untracked files (implies -at)\n");
    printf("  -f, --filter PATTERN  Show only files/folders matching pattern\n");
    printf("\n");
    printf("Sorting:\n");
    printf("  -S              Sort by size (largest first)\n");
    printf("  -T              Sort by modification time (newest first)\n");
    printf("  -N              Sort by name (alphabetical)\n");
    printf("  -r              Reverse sort order\n");
    printf("\n");
    printf("  -h, --help      Show this help message\n");
    printf("  --daemon        Manage the size caching daemon\n");
}

static int apply_short_flag(char flag, Config *cfg) {
    switch (flag) {
        case 'a': cfg->show_hidden = 1; return 1;
        case 's': cfg->long_format = 0; cfg->long_format_explicit = 1; return 1;
        case 'l': cfg->long_format = 1; cfg->long_format_explicit = 1; return 1;
        case 't': cfg->max_depth = L_MAX_DEPTH; return 1;
        case 'p': cfg->show_ancestry = 1; return 1;
        case 'g': cfg->git_only = 1; cfg->show_hidden = 1; cfg->max_depth = L_MAX_DEPTH; return 1;
        case 'S': cfg->sort_by = SORT_SIZE; return 1;
        case 'T': cfg->sort_by = SORT_TIME; return 1;
        case 'N': cfg->sort_by = SORT_NAME; return 1;
        case 'r': cfg->sort_reverse = 1; return 1;
        case 'h': print_usage(); exit(0);
        default: return 0;
    }
}

/*
 * Match an option that takes a required argument.
 * Handles: -x VAL, -xVAL, --xxx VAL, --xxx=VAL
 * Returns the argument value, or NULL if no match.
 * Updates *i if a separate argument was consumed.
 */
static const char *match_opt_with_arg(const char *arg, int *i, int argc, char **argv,
                                      char short_opt, const char *long_opt) {
    size_t long_len = long_opt ? strlen(long_opt) : 0;

    /* -x VAL */
    if (short_opt && arg[0] == '-' && arg[1] == short_opt && arg[2] == '\0') {
        if (*i + 1 >= argc) {
            char msg[64];
            snprintf(msg, sizeof(msg), "-%c/--%s requires an argument", short_opt, long_opt);
            die(msg);
        }
        return argv[++(*i)];
    }
    /* -xVAL */
    if (short_opt && arg[0] == '-' && arg[1] == short_opt && arg[2] != '\0') {
        return arg + 2;
    }
    /* --xxx VAL */
    if (long_opt && strcmp(arg + 2, long_opt) == 0) {
        if (*i + 1 >= argc) {
            char msg[64];
            snprintf(msg, sizeof(msg), "--%s requires an argument", long_opt);
            die(msg);
        }
        return argv[++(*i)];
    }
    /* --xxx=VAL */
    if (long_opt && strncmp(arg + 2, long_opt, long_len) == 0 && arg[2 + long_len] == '=') {
        return arg + 2 + long_len + 1;
    }
    return NULL;
}

#define MATCH_LONG(opt) (strcmp(arg, "--" opt) == 0)

static void parse_args(int argc, char **argv, Config *cfg,
                       char ***dirs, int *dir_count) {
    static char *default_dirs[] = {"."};
    const char *val;

    *dirs = NULL;
    *dir_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (arg[0] != '-' || arg[1] == '\0') {
            /* Positional argument */
            (*dir_count)++;
            *dirs = xrealloc(*dirs, *dir_count * sizeof(char *));
            (*dirs)[*dir_count - 1] = (char *)arg;
            continue;
        }

        /* Long options */
        if (arg[1] == '-') {
            if (MATCH_LONG("help"))            { print_usage(); exit(0); }
            else if (MATCH_LONG("short"))      { cfg->long_format = 0; cfg->long_format_explicit = 1; }
            else if (MATCH_LONG("long"))       { cfg->long_format = 1; cfg->long_format_explicit = 1; }
            else if (MATCH_LONG("tree"))       { cfg->max_depth = L_MAX_DEPTH; }
            else if (MATCH_LONG("path"))       { cfg->show_ancestry = 1; }
            else if (MATCH_LONG("expand-all")) { cfg->expand_all = 1; }
            else if (MATCH_LONG("list"))       { cfg->list_mode = 1; }
            else if (MATCH_LONG("no-icons"))   { cfg->no_icons = 1; }
            /* Options with arguments */
            else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'd', "depth"))) {
                cfg->max_depth = parse_depth(val, "--depth");
            }
            else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'f', "filter"))) {
                cfg->grep_pattern = val;
            }
            else {
                fprintf(stderr, "%sError:%s Unknown option: %s\n",
                        CLR(cfg, COLOR_RED), RST(cfg), arg);
                exit(1);
            }
            continue;
        }

        /* Short options: -x or -xVAL or -xyz (combined flags) */
        if ((val = match_opt_with_arg(arg, &i, argc, argv, 'd', "depth"))) {
            cfg->max_depth = parse_depth(val, "-d");
        } else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'f', "filter"))) {
            cfg->grep_pattern = val;
        } else {
            /* Combined short flags like -alt */
            for (int j = 1; arg[j]; j++) {
                if (!apply_short_flag(arg[j], cfg)) {
                    fprintf(stderr, "%sError:%s Unknown option: -%c\n",
                            CLR(cfg, COLOR_RED), RST(cfg), arg[j]);
                    exit(1);
                }
            }
        }
    }

    if (*dir_count == 0) {
        *dirs = default_dirs;
        *dir_count = 1;
    }
}

#undef MATCH_LONG

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    /* Check for --daemon early (before other initialization) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            daemon_run(argv[0]);
            return 0;
        }
    }

#ifdef HAVE_LIBGIT2
    git_libgit2_init();
    atexit(cleanup_libgit2);
#endif

    /* Initialize config with defaults */
    Config cfg = {
        .max_depth = 1,
        .show_hidden = 0,
        .long_format = 1,
        .long_format_explicit = 0,
        .expand_all = 0,
        .list_mode = 0,
        .no_icons = 0,
        .sort_reverse = 0,
        .git_only = 0,
        .show_ancestry = 0,
        .is_tty = isatty(STDOUT_FILENO),
        .sort_by = SORT_NONE,
        .cwd = "",
        .home = "",
        .script_dir = "",
        .grep_pattern = NULL
    };

    /* Initialize environment paths */
    if (!getcwd(cfg.cwd, sizeof(cfg.cwd))) {
        die("Cannot determine current directory");
    }

    const char *home = getenv("HOME");
    if (home) {
        strncpy(cfg.home, home, sizeof(cfg.home) - 1);
        cfg.home[sizeof(cfg.home) - 1] = '\0';
    }

    resolve_source_dir(argv[0], cfg.script_dir, sizeof(cfg.script_dir));

    /* Check if current directory exists */
    char *cwd_check = getcwd(NULL, 0);
    if (cwd_check == NULL) {
        fprintf(stderr, "%sError:%s Current directory no longer exists\n",
                CLR(&cfg, COLOR_RED), RST(&cfg));
        return 1;
    }
    free(cwd_check);

    /* Parse arguments */
    char **dirs;
    int dir_count;
    parse_args(argc, argv, &cfg, &dirs, &dir_count);

    /* Auto-disable long format on network filesystems */
    if (cfg.long_format && !cfg.long_format_explicit) {
        const char *check_path = (dir_count > 0) ? dirs[0] : cfg.cwd;
        if (path_is_network_fs(check_path)) {
            cfg.long_format = 0;
        }
    }

    /* Load icons */
    Icons icons;
    icons_init_defaults(&icons);
    icons_load(&icons, cfg.script_dir);

    /* Load size cache */
    cache_load();

    /* Validate all inputs first */
    for (int i = 0; i < dir_count; i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) {
            fprintf(stderr, "%sError:%s '%s' does not exist\n",
                    CLR(&cfg, COLOR_RED), RST(&cfg), dirs[i]);
            return 1;
        }
    }

    /* Process each directory */
    int continuation[L_MAX_DEPTH] = {0};

    /* Initialize shared columns for consistent alignment */
    Column cols[NUM_COLUMNS];
    columns_init(cols);

    /* Build all trees first (computes column widths across all arguments) */
    TreeNode **trees = xmalloc(dir_count * sizeof(TreeNode *));
    GitCache *gits = xmalloc(dir_count * sizeof(GitCache));

    for (int i = 0; i < dir_count; i++) {
        git_cache_init(&gits[i]);
        if (cfg.show_ancestry) {
            trees[i] = build_ancestry_tree(dirs[i], cfg.long_format ? cols : NULL, &gits[i], &cfg, &icons);
        } else {
            trees[i] = build_tree(dirs[i], cfg.long_format ? cols : NULL, &gits[i], &cfg, &icons);
        }
    }

    /* Pre-compute visibility flags for filtering */
    if (cfg.git_only) {
        for (int i = 0; i < dir_count; i++) {
            compute_git_status_flags(trees[i]);
        }
    }
    if (cfg.grep_pattern) {
        for (int i = 0; i < dir_count; i++) {
            compute_grep_flags(trees[i], cfg.grep_pattern);
        }
    }
    /* Recalculate column widths for visible entries only */
    if ((cfg.git_only || cfg.grep_pattern) && cfg.long_format) {
        columns_recalculate_visible(cols, trees, dir_count, &icons, &cfg);
    }

    /* Print all trees (using consistent column widths) */
    for (int i = 0; i < dir_count; i++) {
        /* In git-only mode, show message if no changes and in sync with upstream */
        if (cfg.git_only && !trees[i]->has_git_status) {
            int in_sync = 1;
            if (path_is_git_root(trees[i]->entry.path)) {
                char *branch = git_get_branch(trees[i]->entry.path);
                if (branch) {
                    char local_hash[64], remote_hash[64];
                    char local_ref[128], remote_ref[128];
                    snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
                    snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);
                    git_read_ref(trees[i]->entry.path, local_ref, local_hash, sizeof(local_hash));
                    if (git_read_ref(trees[i]->entry.path, remote_ref, remote_hash, sizeof(remote_hash))) {
                        in_sync = (strcmp(local_hash, remote_hash) == 0);
                    }
                    free(branch);
                }
            }
            if (in_sync) {
                printf("%sUp to date.%s\n", COLOR_GREEN, COLOR_RESET);
                continue;
            }
        }
        PrintContext ctx = {
            .git = &gits[i],
            .icons = &icons,
            .cfg = &cfg,
            .columns = cfg.long_format ? cols : NULL,
            .continuation = continuation
        };
        print_tree_node(trees[i], 0, &ctx);
    }

    /* Cleanup */
    for (int i = 0; i < dir_count; i++) {
        tree_node_free(trees[i]);
        free(trees[i]);
        git_cache_free(&gits[i]);
    }
    free(trees);
    free(gits);

    cache_unload();
    return 0;
}
