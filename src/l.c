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
#include "select.h"

#ifdef HAVE_LIBGIT2
#include <git2.h>

static void cleanup_libgit2(void) {
    git_libgit2_shutdown();
}
#endif

/* ============================================================================
 * Argument Parsing
 * ============================================================================ */

static off_t parse_size(const char *str) {
    char *endptr;
    double val = strtod(str, &endptr);
    if (endptr == str || val < 0) die("--min-size requires a positive number with optional suffix (K, M, G, T)");
    switch (*endptr) {
        case 'k': case 'K': val *= 1024; endptr++; break;
        case 'm': case 'M': val *= 1024 * 1024; endptr++; break;
        case 'g': case 'G': val *= 1024 * 1024 * 1024; endptr++; break;
        case 't': case 'T': val *= 1024.0 * 1024 * 1024 * 1024; endptr++; break;
        case '\0': break;
        default: die("--min-size: unknown suffix (use K, M, G, or T)");
    }
    if (*endptr != '\0') die("--min-size: trailing characters after size");
    return (off_t)val;
}

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
    printf("  -a                      Show hidden files\n");
    printf("  -l, --long              Long format with size, lines, time (default)\n");
    printf("  -s, --short             Short format (no size, lines, time)\n");
    printf("                          Auto-enabled on network filesystems\n");
    printf("  -t, --tree              Show full tree (depth %d)\n", L_MAX_DEPTH);
    printf("  -d, --depth INT         Limit tree depth\n");
    printf("  -p, --path              Show ancestry from ~ (or /) to target\n");
    printf("  -e, --expand-all        Expand all directories (ignore skip list)\n");
    printf("  --list                  Flat list output (no tree structure)\n");
    printf("  --summary               Show summary info for file/directory\n");
    printf("  --no-icons              Hide file/folder/git icons\n");
    printf("  -c, --color-all         Don't gray out gitignored files\n");
    printf("  -g                      Git-changed files, rooted at the repo (fails outside a repo)\n");
    printf("  -f, --filter STRING     Show only files/folders matching pattern (glob or substring)\n");
    printf("  --min-size SIZE         Show only entries >= SIZE (e.g., 100M, 1G)\n");
    printf("  --dir-only              Show only directories\n");
    printf("  -i, --interactive       Interactive selection mode\n");
    printf("\n");
    printf("Sorting:\n");
    printf("  -S                      Sort by size (largest first)\n");
    printf("  -T                      Sort by modification time (newest first)\n");
    printf("  -N                      Sort by name (alphabetical)\n");
    printf("  -r                      Reverse sort order\n");
    printf("\n");
    printf("  -h, --help              Show this help message\n");
    printf("  --version               Show version information\n");
    printf("  --tty                   Force TTY mode (colors, icons) even when piped\n");
    printf("  --daemon                Manage the size caching daemon\n");
}

/* Track which options have been set to detect conflicts/duplicates */
typedef struct {
    const char *depth;   /* -t, -d, --tree, --depth, -g */
    const char *format;  /* -s, -l, --short, --long */
    const char *sort;    /* -S, -T, -N */
    const char *filter;  /* -f, --filter */
} OptionSet;

static void check_conflict(const char **slot, const char *opt, const Config *cfg) {
    if (*slot) {
        fprintf(stderr, "%sError:%s %s conflicts with %s\n",
                CLR(cfg, COLOR_RED), RST(cfg), opt, *slot);
        exit(1);
    }
    *slot = opt;
}

/* Returns: 1 = applied, 0 = unknown, -1 = requires argument */
static int apply_short_flag(char flag, Config *cfg, OptionSet *set) {
    switch (flag) {
        case 'a': cfg->show_hidden = 1; return 1;
        case 's': check_conflict(&set->format, "-s", cfg);
                  cfg->long_format = 0; cfg->long_format_explicit = 1; return 1;
        case 'l': check_conflict(&set->format, "-l", cfg);
                  cfg->long_format = 1; cfg->long_format_explicit = 1; return 1;
        case 't': check_conflict(&set->depth, "-t", cfg);
                  cfg->max_depth = L_MAX_DEPTH; return 1;
        case 'p': cfg->show_ancestry = 1; cfg->ancestry_explicit = 1; return 1;
        case 'e': cfg->expand_all = 1; return 1;
        case 'c': cfg->color_all = 1; return 1;
        case 'i': cfg->interactive = 1; return 1;
        case 'g': cfg->git_only = 1; cfg->show_hidden = 1; cfg->max_depth = L_MAX_DEPTH; return 1;
        case 'S': check_conflict(&set->sort, "-S", cfg);
                  cfg->sort_by = SORT_SIZE; return 1;
        case 'T': check_conflict(&set->sort, "-T", cfg);
                  cfg->sort_by = SORT_TIME; return 1;
        case 'N': check_conflict(&set->sort, "-N", cfg);
                  cfg->sort_by = SORT_NAME; return 1;
        case 'r': cfg->sort_reverse = 1; return 1;
        case 'h': print_usage(); exit(0);
        case 'd': case 'f': return -1;  /* requires argument */
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
    OptionSet set = {0};
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
            /* End of options marker */
            if (arg[2] == '\0') {
                for (i++; i < argc; i++) {
                    (*dir_count)++;
                    *dirs = xrealloc(*dirs, *dir_count * sizeof(char *));
                    (*dirs)[*dir_count - 1] = argv[i];
                }
                break;
            }
            if (MATCH_LONG("help"))            { print_usage(); exit(0); }
            else if (MATCH_LONG("short"))      { check_conflict(&set.format, "--short", cfg);
                                                 cfg->long_format = 0; cfg->long_format_explicit = 1; }
            else if (MATCH_LONG("long"))       { check_conflict(&set.format, "--long", cfg);
                                                 cfg->long_format = 1; cfg->long_format_explicit = 1; }
            else if (MATCH_LONG("tree"))       { check_conflict(&set.depth, "--tree", cfg);
                                                 cfg->max_depth = L_MAX_DEPTH; }
            else if (MATCH_LONG("path"))       { cfg->show_ancestry = 1; cfg->ancestry_explicit = 1; }
            else if (MATCH_LONG("expand-all")) { cfg->expand_all = 1; }
            else if (MATCH_LONG("list"))       { cfg->list_mode = 1; }
            else if (MATCH_LONG("summary"))    { cfg->summary_mode = 1;
                                                 cfg->max_depth = L_MAX_DEPTH;
                                                 cfg->long_format = 1; }
            else if (MATCH_LONG("no-icons"))   { cfg->no_icons = 1; }
            else if (MATCH_LONG("color-all")) { cfg->color_all = 1; }
            else if (MATCH_LONG("interactive")) { cfg->interactive = 1; }
            else if (MATCH_LONG("tty"))         { cfg->is_tty = 1; }
            else if (MATCH_LONG("dir-only"))    { cfg->dir_only = 1; }
            /* Options with arguments */
            else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'd', "depth"))) {
                check_conflict(&set.depth, "--depth", cfg);
                cfg->max_depth = parse_depth(val, "--depth");
            }
            else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'f', "filter"))) {
                check_conflict(&set.filter, "--filter", cfg);
                cfg->grep_pattern = val;
            }
            else if ((val = match_opt_with_arg(arg, &i, argc, argv, 0, "min-size"))) {
                cfg->min_size = parse_size(val);
            }
            else if (strcmp(arg, "--daemon") == 0 || strcmp(arg, "--version") == 0) {
                fprintf(stderr, "%sError:%s %s must be the first argument\n",
                        CLR(cfg, COLOR_RED), RST(cfg), arg);
                exit(1);
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
            check_conflict(&set.depth, "-d", cfg);
            cfg->max_depth = parse_depth(val, "-d");
        } else if ((val = match_opt_with_arg(arg, &i, argc, argv, 'f', "filter"))) {
            check_conflict(&set.filter, "-f", cfg);
            cfg->grep_pattern = val;
        } else {
            /* Combined short flags like -alt, -ad2, -af "*.c" */
            for (int j = 1; arg[j]; j++) {
                int result = apply_short_flag(arg[j], cfg, &set);
                if (result == -1) {
                    /* Flag requires argument: consume rest of string or next argv */
                    const char *flag_arg = arg[j + 1] ? arg + j + 1 : NULL;
                    if (!flag_arg) {
                        if (i + 1 >= argc) {
                            fprintf(stderr, "%sError:%s -%c requires an argument\n",
                                    CLR(cfg, COLOR_RED), RST(cfg), arg[j]);
                            exit(1);
                        }
                        flag_arg = argv[++i];
                    }
                    if (arg[j] == 'd') {
                        check_conflict(&set.depth, "-d", cfg);
                        cfg->max_depth = parse_depth(flag_arg, "-d");
                    } else if (arg[j] == 'f') {
                        check_conflict(&set.filter, "-f", cfg);
                        cfg->grep_pattern = flag_arg;
                    }
                    break;  /* rest of string consumed */
                } else if (result == 0) {
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

/* When stat/lstat on a path fails, walk up its ancestor directories to find the
 * deepest one the user cannot search into. This distinguishes a genuinely
 * missing path from one hidden behind an inaccessible parent. Returns 1 and
 * writes the offending directory into buf if found, else 0. */
static int find_inaccessible_ancestor(const char *path, char *buf, size_t buflen) {
    char work[PATH_MAX];
    if (strlen(path) >= sizeof(work)) return 0;
    strcpy(work, path);

    /* Strip trailing slashes so the leaf component is well-defined. */
    size_t len = strlen(work);
    while (len > 1 && work[len - 1] == '/') work[--len] = '\0';

    for (;;) {
        /* Ascend to the parent directory. */
        char *slash = strrchr(work, '/');
        if (!slash) return 0;       /* no parent component to inspect */
        if (slash == work) work[1] = '\0';  /* keep root "/" */
        else *slash = '\0';

        struct stat st;
        if (stat(work, &st) != 0) {
            /* Can't even stat this ancestor (e.g. a higher dir denies search);
             * keep climbing until we reach one we can examine. */
            continue;
        }
        if (S_ISDIR(st.st_mode) && access(work, X_OK) != 0) {
            snprintf(buf, buflen, "%s", work);
            return 1;
        }
        /* Reached an accessible ancestor: the missing leaf is genuinely absent. */
        return 0;
    }
}

/* For a genuinely missing path, find where the chain of directories stops
 * existing. Writes into *split the length of the prefix of `path` that ends at
 * the first component which does not exist (the deepest existing ancestor plus
 * one more name), so path[0..*split) can be reported as the missing path.
 * Returns 1 if an existing ancestor was found, else 0. */
static int find_missing_boundary(const char *path, size_t *split) {
    char work[PATH_MAX];
    if (strlen(path) >= sizeof(work)) return 0;
    strcpy(work, path);

    size_t len = strlen(work);
    while (len > 1 && work[len - 1] == '/') work[--len] = '\0';

    for (;;) {
        char *slash = strrchr(work, '/');
        if (!slash) return 0;       /* no ancestor to anchor to */
        if (slash == work) work[1] = '\0';  /* keep root "/" */
        else *slash = '\0';

        struct stat st;
        if (stat(work, &st) != 0) continue;  /* this ancestor is missing too */

        /* `work` is the deepest existing ancestor; the boundary is the end of
         * the next path component, i.e. the first name that does not exist. */
        size_t i = strlen(work);
        if (path[i] == '/') i++;             /* skip the separating slash */
        while (path[i] && path[i] != '/') i++;
        *split = i;
        return 1;
    }
}

#ifndef VERSION
#define VERSION "unknown"
#endif

int main(int argc, char **argv) {
    /* Check for --version/--daemon (must be first argument) */
    if (argc >= 2) {
        if (strcmp(argv[1], "--version") == 0) {
            if (argc > 2) {
                fprintf(stderr, "Error: --version takes no other arguments\n");
                return 1;
            }
            int tty = isatty(STDOUT_FILENO);
            const char *bold = tty ? STYLE_BOLD : "";
            const char *cyan = tty ? COLOR_CYAN : "";
            const char *grey = tty ? COLOR_GREY : "";
            const char *rst  = tty ? COLOR_RESET : "";

            char libgit2_ver[32];
#ifdef HAVE_LIBGIT2
            int major = 0, minor = 0, rev = 0;
            git_libgit2_version(&major, &minor, &rev);
            snprintf(libgit2_ver, sizeof(libgit2_ver), "%d.%d.%d", major, minor, rev);
#else
            snprintf(libgit2_ver, sizeof(libgit2_ver), "disabled");
#endif

            printf("%sl%s %s%s%s\n", bold, rst, cyan, VERSION, rst);
            printf("Enhanced directory listing with tree view, icons, and git integration.\n\n");
            printf("  %sAuthor  %s Hamish M. Blair <hmblair@stanford.edu>\n", grey, rst);
            printf("  %sHomepage%s https://github.com/hmblair/l\n", grey, rst);
            printf("  %slibgit2 %s %s\n", grey, rst, libgit2_ver);
            printf("  %sLicense %s MIT\n", grey, rst);
            return 0;
        }
        if (strcmp(argv[1], "--daemon") == 0) {
            const char *subcmd = (argc > 2) ? argv[2] : NULL;
            if (argc > 3) {
                fprintf(stderr, "Error: --daemon takes at most one subcommand\n");
                return 1;
            }
            daemon_run(argv[0], subcmd);
            return 0;
        }
    }

#ifdef HAVE_LIBGIT2
    git_libgit2_init();
    atexit(cleanup_libgit2);
#endif

#ifdef _OPENMP
    /* Cap the thread pool to avoid spin-up overhead on many-core machines.
     * Only lowers the count, so a smaller OMP_NUM_THREADS set by the user wins. */
    if (L_MAX_THREADS > 0 && omp_get_max_threads() > L_MAX_THREADS) {
        omp_set_num_threads(L_MAX_THREADS);
    }
#endif

    /* Initialize config with defaults */
    Config cfg = {
        .max_depth = 1,
        .show_hidden = 0,
        .long_format = 1,
        .long_format_explicit = 0,
        .expand_all = 0,
        .list_mode = 0,
        .summary_mode = 0,
        .no_icons = 0,
        .sort_reverse = 0,
        .git_only = 0,
        .show_ancestry = 0,
        .color_all = 0,
        .interactive = 0,
        .dir_only = 0,
        .is_tty = isatty(STDOUT_FILENO),
        .sort_by = SORT_NONE,
        .cwd = "",
        .home = "",
        .script_dir = "",
        .column_separator = "·",
        .grep_pattern = NULL,
        .min_size = 0,
        .compute = COMPUTE_NONE
    };

    /* Initialize environment paths - prefer $PWD to preserve symlink paths */
    const char *pwd = getenv("PWD");
    char cwd_physical[PATH_MAX];
    if (!getcwd(cwd_physical, sizeof(cwd_physical))) {
        die("Cannot determine current directory");
    }
    if (pwd && pwd[0] == '/') {
        char pwd_resolved[PATH_MAX];
        if (realpath(pwd, pwd_resolved) && strcmp(pwd_resolved, cwd_physical) == 0) {
            strncpy(cfg.cwd, pwd, sizeof(cfg.cwd) - 1);
            cfg.cwd[sizeof(cfg.cwd) - 1] = '\0';
        } else {
            strncpy(cfg.cwd, cwd_physical, sizeof(cfg.cwd) - 1);
            cfg.cwd[sizeof(cfg.cwd) - 1] = '\0';
        }
    } else {
        strncpy(cfg.cwd, cwd_physical, sizeof(cfg.cwd) - 1);
        cfg.cwd[sizeof(cfg.cwd) - 1] = '\0';
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

    /* -g requires a git repo: fail outside one, otherwise show the ancestry up
     * to the repo root (git_only filtering then collapses it to just the repo
     * root when there are no changes). */
    if (cfg.git_only) {
        char repo_root[PATH_MAX];
        const char *target = dir_count > 0 ? dirs[0] : ".";
        if (!git_find_root(target, repo_root, sizeof(repo_root))) {
            fprintf(stderr, "%sError:%s not in a git repository\n",
                    CLR(&cfg, COLOR_RED), RST(&cfg));
            return 1;
        }
        cfg.show_ancestry = 1;
    }

    /* Auto-disable long format on network filesystems */
    if (cfg.long_format && !cfg.long_format_explicit) {
        const char *check_path = (dir_count > 0) ? dirs[0] : cfg.cwd;
        if (path_is_network_fs(check_path)) {
            cfg.long_format = 0;
        }
    }

    /* Set compute options based on mode */
    if (cfg.summary_mode) {
        cfg.compute = COMPUTE_SUMMARY;
    } else if (cfg.long_format) {
        cfg.compute = COMPUTE_LONG;
    } else {
        cfg.compute = COMPUTE_BASIC;
    }

    /* Enable sizes if needed for sorting/filtering in short mode */
    if (cfg.sort_by == SORT_SIZE) cfg.compute.sizes = 1;
    if (cfg.min_size > 0) cfg.compute.sizes = 1;

    /* Load display settings (overrides the built-in separator default) */
    settings_load(cfg.script_dir, cfg.column_separator, sizeof(cfg.column_separator));

    /* Load icons */
    Icons icons;
    icons_init_defaults(&icons);
    icons_load(&icons, cfg.script_dir);

    /* Load file types */
    FileTypes filetypes;
    filetypes_init(&filetypes);
    filetypes_load(&filetypes, cfg.script_dir);

    /* Load shebangs */
    Shebangs shebangs;
    shebangs_init(&shebangs);
    shebangs_load(&shebangs, cfg.script_dir);

    /* Load size cache (only needed when computing sizes or file counts) */
    if (cfg.compute.sizes || cfg.compute.file_counts) {
        cache_load();
    }

    /* Validate all inputs first. Use lstat so a broken symlink (whose target
     * is missing) still counts as existing — the link entry itself is real. */
    for (int i = 0; i < dir_count; i++) {
        struct stat st;
        if (lstat(dirs[i], &st) != 0) {
            char blocker[PATH_MAX];
            size_t split;
            if (find_inaccessible_ancestor(dirs[i], blocker, sizeof(blocker))) {
                fprintf(stderr, "%sError:%s '%s' is inaccessible\n",
                        CLR(&cfg, COLOR_RED), RST(&cfg), blocker);
            } else if (find_missing_boundary(dirs[i], &split)) {
                /* Truncate the path after the first dir that does not exist,
                 * re-abbreviating the home prefix the shell expanded away. */
                char missing[PATH_MAX], shown[PATH_MAX];
                snprintf(missing, sizeof(missing), "%.*s", (int)split, dirs[i]);
                abbreviate_home(missing, shown, sizeof(shown), &cfg);
                fprintf(stderr, "%sError:%s '%s' does not exist\n",
                        CLR(&cfg, COLOR_RED), RST(&cfg), shown);
            } else {
                fprintf(stderr, "%sError:%s '%s' does not exist\n",
                        CLR(&cfg, COLOR_RED), RST(&cfg), dirs[i]);
            }
            return 1;
        }
    }

    /* Auto-enable summary mode for single file arguments */
    if (dir_count == 1) {
        struct stat st;
        if (stat(dirs[0], &st) == 0 && S_ISREG(st.st_mode)) {
            cfg.summary_mode = 1;
        }
    }

    /* Process each directory */
    int continuation[L_MAX_DEPTH] = {0};

    /* Initialize shared columns for consistent alignment */
    Column cols[NUM_COLUMNS];
    columns_init(cols);

    /* Build all trees first (computes column widths across all arguments).
     * The git cache is scoped to the whole invocation, not to a single argument:
     * it is keyed by absolute path and already holds many repo roots at once, so
     * every argument's tree is built into one shared cache. This keeps the data
     * layer input-count-agnostic — the tree, flat-list, and interactive renderers
     * all query the same cache by path (the picker flattens all trees into one
     * list, so a per-argument cache would leave non-first inputs without git
     * data). */
    TreeNode **trees = xmalloc(dir_count * sizeof(TreeNode *));
    GitCache git;
    git_cache_init(&git);

    for (int i = 0; i < dir_count; i++) {
        if (cfg.show_ancestry) {
            trees[i] = build_ancestry_tree_from_config(dirs[i], &git, &cfg, &icons);
        } else {
            trees[i] = build_tree_from_config(dirs[i], &git, &cfg, &icons);
        }
    }

    /* Pre-compute visibility flags for filtering */
    if (cfg.git_only) {
        for (int i = 0; i < dir_count; i++) {
            compute_git_status_flags(trees[i], &git, cfg.show_hidden);
        }
    }
    if (cfg.grep_pattern) {
        for (int i = 0; i < dir_count; i++) {
            compute_grep_flags(trees[i], cfg.grep_pattern);
        }
    }
    /* Data pass: precompute each directory's view summary (git status and diff
     * lines rolled up to the nearest visible ancestor). Must run before both the
     * width measurement and printing, since both read view_git_summary. Depends
     * on the git/grep visibility flags above. Interactive mode recomputes over
     * its own live visible set (select.c). */
    if (cfg.compute.git_status) {
        for (int i = 0; i < dir_count; i++) {
            compute_view_summaries(trees[i], &cfg, &git);
        }
    }
    /* Measure all column widths in one pass over the rows that will render.
     * Must run after the git/grep flags above, which child visibility depends
     * on. Interactive mode re-measures over its own visible set (select.c). */
    int diff_add_width = 0, diff_del_width = 0;
    if (cfg.long_format) {
        measure_columns(trees, dir_count, &git, &icons, &cfg,
                        cols, &diff_add_width, &diff_del_width);
    }

    /* Interactive selection mode */
    if (cfg.interactive) {
        PrintContext ctx = {
            .git = &git,
            .icons = &icons,
            .filetypes = &filetypes,
            .shebangs = &shebangs,
            .cfg = &cfg,
            .columns = cfg.long_format ? cols : NULL,
            .continuation = continuation,
            .diff_add_width = diff_add_width,
            .diff_del_width = diff_del_width,
            .term_width = cfg.is_tty ? get_terminal_width() : 0
        };
        char *selected = select_run(trees, dir_count, &ctx);
        int exit_code = 0;
        if (selected) {
            printf("%s\n", selected);
            free(selected);
        } else {
            exit_code = 1;  /* No selection made (quit/ESC) */
        }
        /* Cleanup and exit */
        for (int i = 0; i < dir_count; i++) {
            tree_node_free(trees[i]);
            free(trees[i]);
        }
        free(trees);
        git_cache_free(&git);
        cache_unload();
        return exit_code;
    }
    {
        /* Print all trees (using consistent column widths) */
        for (int i = 0; i < dir_count; i++) {
            /* Check if filtering produced no visible children. For -g we still
             * show the repo root even with no changes, so skip this. */
            if (is_filtering_active(&cfg) && !cfg.git_only) {
                int has_visible = 0;
                for (size_t j = 0; j < trees[i]->child_count; j++) {
                    if (node_is_hidden(&trees[i]->children[j], &cfg)) continue;
                    if (node_is_visible(&trees[i]->children[j], &cfg)) {
                        has_visible = 1;
                        break;
                    }
                }
                if (!has_visible) {
                    printf("%sNo matches.%s\n",
                           CLR(&cfg, COLOR_RED), RST(&cfg));
                    continue;
                }
            }
            PrintContext ctx = {
                .git = &git,
                .icons = &icons,
                .filetypes = &filetypes,
                .shebangs = &shebangs,
                .cfg = &cfg,
                .columns = cfg.long_format ? cols : NULL,
                .continuation = continuation,
                .diff_add_width = diff_add_width,
                .diff_del_width = diff_del_width,
                .term_width = cfg.is_tty ? get_terminal_width() : 0
            };
            if (cfg.summary_mode) {
                print_summary(trees[i], &ctx);
            } else {
                print_tree_node(trees[i], 0, &ctx);
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < dir_count; i++) {
        tree_node_free(trees[i]);
        free(trees[i]);
    }
    free(trees);
    git_cache_free(&git);

    cache_unload();
    return 0;
}
