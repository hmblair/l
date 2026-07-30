/*
 * l - Enhanced directory listing with tree view
 *
 * A fast, portable directory listing tool with tree visualization,
 * git integration, icons, and colors.
 */

#include "common.h"
#include "cache.h"
#include "config.h"
#include "git.h"
#include "view.h"
#include "render.h"
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
    printf("  -m                      Git-changed files, rooted at the repo (fails outside a repo)\n");
    printf("  -g                      Hide gitignored files and folders\n");
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

/* ----------------------------------------------------------------------------
 * Option table - single source of truth for every flag: its spellings,
 * whether it takes an argument, which conflict group it belongs to, and the
 * handler that applies it to the Config. The parse loop below handles all
 * spellings (-x, -xVAL, -xyz combined, --long, --long VAL, --long=VAL, --).
 * ---------------------------------------------------------------------------- */

/* Options in the same group (other than GROUP_NONE) conflict with each other
 * and with themselves when given twice. */
typedef enum {
    GROUP_NONE,
    GROUP_DEPTH,    /* -t/--tree, -d/--depth */
    GROUP_FORMAT,   /* -s/--short, -l/--long */
    GROUP_SORT,     /* -S, -T, -N */
    GROUP_FILTER    /* -f/--filter */
} OptGroup;

/* label is the spelling used on the command line ("-d" or "--depth"): it names
 * the option in conflict and value-parse error messages. val is NULL for
 * options without arguments. */
typedef void (*OptApply)(Config *cfg, const char *val, const char *label);

typedef struct {
    char short_c;             /* 0 if no short spelling */
    const char *short_label;  /* "-d" */
    const char *long_name;    /* NULL if no long spelling */
    const char *long_label;   /* "--depth" */
    int takes_arg;
    OptGroup group;
    OptApply apply;
} OptSpec;

static void check_conflict(const char **slot, const char *opt, const Config *cfg) {
    if (*slot) {
        fprintf(stderr, "%sError:%s %s conflicts with %s\n",
                CLR(cfg, COLOR_RED), RST(cfg), opt, *slot);
        exit(1);
    }
    *slot = opt;
}

#define OPT_HANDLER(name) \
    static void name(Config *cfg, const char *val, const char *label)

OPT_HANDLER(opt_hidden)      { (void)val; (void)label; cfg->req.show_hidden = 1; }
OPT_HANDLER(opt_short_fmt)   { (void)val; (void)label; cfg->disp.long_format = 0; cfg->disp.long_format_explicit = 1; }
OPT_HANDLER(opt_long_fmt)    { (void)val; (void)label; cfg->disp.long_format = 1; cfg->disp.long_format_explicit = 1; }
OPT_HANDLER(opt_tree)        { (void)val; (void)label; cfg->req.max_depth = L_MAX_DEPTH; }
OPT_HANDLER(opt_depth)       { (void)cfg; cfg->req.max_depth = parse_depth(val, label); }
OPT_HANDLER(opt_path)        { (void)val; (void)label; cfg->req.show_ancestry = 1; cfg->req.ancestry_explicit = 1; }
OPT_HANDLER(opt_expand)      { (void)val; (void)label; cfg->req.expand_all = 1; }
OPT_HANDLER(opt_color_all)   { (void)val; (void)label; cfg->disp.color_all = 1; }
OPT_HANDLER(opt_interactive) { (void)val; (void)label; cfg->req.interactive = 1; }
OPT_HANDLER(opt_git_only)    { (void)val; (void)label; cfg->req.git_only = 1; cfg->req.show_hidden = 1; }
OPT_HANDLER(opt_hide_ignored){ (void)val; (void)label; cfg->req.hide_gitignored = 1; }
OPT_HANDLER(opt_sort_size)   { (void)val; (void)label; cfg->req.sort_by = SORT_SIZE; }
OPT_HANDLER(opt_sort_time)   { (void)val; (void)label; cfg->req.sort_by = SORT_TIME; }
OPT_HANDLER(opt_sort_name)   { (void)val; (void)label; cfg->req.sort_by = SORT_NAME; }
OPT_HANDLER(opt_reverse)     { (void)val; (void)label; cfg->req.sort_reverse = 1; }
OPT_HANDLER(opt_help)        { (void)cfg; (void)val; (void)label; print_usage(); exit(0); }
OPT_HANDLER(opt_filter)      { (void)label; cfg->req.grep_pattern = val; }
OPT_HANDLER(opt_list)        { (void)val; (void)label; cfg->req.list_mode = 1; }
OPT_HANDLER(opt_summary)     { (void)val; (void)label; cfg->req.summary_mode = 1;
                               cfg->req.max_depth = L_MAX_DEPTH; cfg->disp.long_format = 1; }
OPT_HANDLER(opt_no_icons)    { (void)val; (void)label; cfg->disp.no_icons = 1; }
OPT_HANDLER(opt_tty)         { (void)val; (void)label; cfg->disp.is_tty = 1; }
OPT_HANDLER(opt_dir_only)    { (void)val; (void)label; cfg->req.dir_only = 1; }
OPT_HANDLER(opt_min_size)    { (void)label; cfg->req.min_size = parse_size(val); }

#undef OPT_HANDLER

static const OptSpec OPT_SPECS[] = {
    { 'a', "-a", NULL,          NULL,            0, GROUP_NONE,   opt_hidden },
    { 's', "-s", "short",       "--short",       0, GROUP_FORMAT, opt_short_fmt },
    { 'l', "-l", "long",        "--long",        0, GROUP_FORMAT, opt_long_fmt },
    { 't', "-t", "tree",        "--tree",        0, GROUP_DEPTH,  opt_tree },
    { 'd', "-d", "depth",       "--depth",       1, GROUP_DEPTH,  opt_depth },
    { 'p', "-p", "path",        "--path",        0, GROUP_NONE,   opt_path },
    { 'e', "-e", "expand-all",  "--expand-all",  0, GROUP_NONE,   opt_expand },
    { 'c', "-c", "color-all",   "--color-all",   0, GROUP_NONE,   opt_color_all },
    { 'i', "-i", "interactive", "--interactive", 0, GROUP_NONE,   opt_interactive },
    { 'm', "-m", NULL,          NULL,            0, GROUP_NONE,   opt_git_only },
    { 'g', "-g", NULL,          NULL,            0, GROUP_NONE,   opt_hide_ignored },
    { 'S', "-S", NULL,          NULL,            0, GROUP_SORT,   opt_sort_size },
    { 'T', "-T", NULL,          NULL,            0, GROUP_SORT,   opt_sort_time },
    { 'N', "-N", NULL,          NULL,            0, GROUP_SORT,   opt_sort_name },
    { 'r', "-r", NULL,          NULL,            0, GROUP_NONE,   opt_reverse },
    { 'h', "-h", "help",        "--help",        0, GROUP_NONE,   opt_help },
    { 'f', "-f", "filter",      "--filter",      1, GROUP_FILTER, opt_filter },
    { 0,   NULL, "list",        "--list",        0, GROUP_NONE,   opt_list },
    { 0,   NULL, "summary",     "--summary",     0, GROUP_NONE,   opt_summary },
    { 0,   NULL, "no-icons",    "--no-icons",    0, GROUP_NONE,   opt_no_icons },
    { 0,   NULL, "tty",         "--tty",         0, GROUP_NONE,   opt_tty },
    { 0,   NULL, "dir-only",    "--dir-only",    0, GROUP_NONE,   opt_dir_only },
    { 0,   NULL, "min-size",    "--min-size",    1, GROUP_NONE,   opt_min_size },
};
#define NUM_OPT_SPECS (sizeof(OPT_SPECS) / sizeof(OPT_SPECS[0]))

/* Track which option of each group has been seen, for conflict messages */
typedef struct {
    const char *slots[5];   /* indexed by OptGroup */
} OptionSet;

static const OptSpec *find_short_opt(char c) {
    for (size_t i = 0; i < NUM_OPT_SPECS; i++) {
        if (OPT_SPECS[i].short_c == c) return &OPT_SPECS[i];
    }
    return NULL;
}

static const OptSpec *find_long_opt(const char *name, size_t len) {
    for (size_t i = 0; i < NUM_OPT_SPECS; i++) {
        const char *ln = OPT_SPECS[i].long_name;
        if (ln && strlen(ln) == len && strncmp(ln, name, len) == 0)
            return &OPT_SPECS[i];
    }
    return NULL;
}

/* Run one matched option: conflict check under the spelling used, then apply */
static void apply_opt(const OptSpec *sp, const char *label, const char *val,
                      Config *cfg, OptionSet *set) {
    if (sp->group != GROUP_NONE) {
        check_conflict(&set->slots[sp->group], label, cfg);
    }
    sp->apply(cfg, val, label);
}

static void parse_args(int argc, char **argv, Config *cfg,
                       char ***dirs, int *dir_count) {
    static char *default_dirs[] = {"."};
    OptionSet set = {{0}};

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

            const char *eq = strchr(arg + 2, '=');
            size_t name_len = eq ? (size_t)(eq - (arg + 2)) : strlen(arg + 2);
            const OptSpec *sp = find_long_opt(arg + 2, name_len);

            if (sp && sp->takes_arg) {
                const char *val;
                if (eq) {
                    val = eq + 1;
                } else {
                    if (i + 1 >= argc) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "--%s requires an argument", sp->long_name);
                        die(msg);
                    }
                    val = argv[++i];
                }
                apply_opt(sp, sp->long_label, val, cfg, &set);
                continue;
            }
            if (sp && !eq) {  /* --list=x etc. stays an unknown option */
                apply_opt(sp, sp->long_label, NULL, cfg, &set);
                continue;
            }
            if (strcmp(arg, "--daemon") == 0 || strcmp(arg, "--version") == 0) {
                fprintf(stderr, "%sError:%s %s must be the first argument\n",
                        CLR(cfg, COLOR_RED), RST(cfg), arg);
                exit(1);
            }
            fprintf(stderr, "%sError:%s Unknown option: %s\n",
                    CLR(cfg, COLOR_RED), RST(cfg), arg);
            exit(1);
        }

        /* Standalone short option with argument: -d VAL or -dVAL */
        const OptSpec *sp0 = find_short_opt(arg[1]);
        if (sp0 && sp0->takes_arg) {
            const char *val;
            if (arg[2] != '\0') {
                val = arg + 2;
            } else {
                if (i + 1 >= argc) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "-%c/--%s requires an argument",
                             sp0->short_c, sp0->long_name);
                    die(msg);
                }
                val = argv[++i];
            }
            apply_opt(sp0, sp0->short_label, val, cfg, &set);
            continue;
        }

        /* Combined short flags like -alt, -ad2, -af "*.c" */
        for (int j = 1; arg[j]; j++) {
            const OptSpec *sp = find_short_opt(arg[j]);
            if (!sp) {
                fprintf(stderr, "%sError:%s Unknown option: -%c\n",
                        CLR(cfg, COLOR_RED), RST(cfg), arg[j]);
                exit(1);
            }
            if (sp->takes_arg) {
                /* Consume the rest of the string, or the next argv */
                const char *flag_arg = arg[j + 1] ? arg + j + 1 : NULL;
                if (!flag_arg) {
                    if (i + 1 >= argc) {
                        fprintf(stderr, "%sError:%s -%c requires an argument\n",
                                CLR(cfg, COLOR_RED), RST(cfg), arg[j]);
                        exit(1);
                    }
                    flag_arg = argv[++i];
                }
                apply_opt(sp, sp->short_label, flag_arg, cfg, &set);
                break;  /* rest of string consumed */
            }
            apply_opt(sp, sp->short_label, NULL, cfg, &set);
        }
    }

    if (*dir_count == 0) {
        *dirs = default_dirs;
        *dir_count = 1;
    }
}

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
        .req = {
            .max_depth = 1,
            .sort_by = SORT_NONE,
        },
        .disp = {
            .long_format = 1,
            .is_tty = isatty(STDOUT_FILENO),
            .column_separator = "·",
        },
        .compute = COMPUTE_NONE,
        .env = {
            .cwd = "",
            .home = "",
            .config_dir = "",
        },
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
            strncpy(cfg.env.cwd, pwd, sizeof(cfg.env.cwd) - 1);
            cfg.env.cwd[sizeof(cfg.env.cwd) - 1] = '\0';
        } else {
            strncpy(cfg.env.cwd, cwd_physical, sizeof(cfg.env.cwd) - 1);
            cfg.env.cwd[sizeof(cfg.env.cwd) - 1] = '\0';
        }
    } else {
        strncpy(cfg.env.cwd, cwd_physical, sizeof(cfg.env.cwd) - 1);
        cfg.env.cwd[sizeof(cfg.env.cwd) - 1] = '\0';
    }

    const char *home = getenv("HOME");
    if (home) {
        strncpy(cfg.env.home, home, sizeof(cfg.env.home) - 1);
        cfg.env.home[sizeof(cfg.env.home) - 1] = '\0';
    }

    resolve_source_dir(argv[0], cfg.env.config_dir, sizeof(cfg.env.config_dir));

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

    /* -m requires a git repo: fail outside one, otherwise show the ancestry up
     * to the repo root (git_only filtering then collapses it to just the repo
     * root when there are no changes). */
    if (cfg.req.git_only) {
        char repo_root[PATH_MAX];
        const char *target = dir_count > 0 ? dirs[0] : ".";
        if (!git_find_root(target, repo_root, sizeof(repo_root))) {
            fprintf(stderr, "%sError:%s not in a git repository\n",
                    CLR(&cfg, COLOR_RED), RST(&cfg));
            return 1;
        }
        cfg.req.show_ancestry = 1;
    }

    /* Auto-disable long format on network filesystems */
    if (cfg.disp.long_format && !cfg.disp.long_format_explicit) {
        const char *check_path = (dir_count > 0) ? dirs[0] : cfg.env.cwd;
        if (path_is_network_fs(check_path)) {
            cfg.disp.long_format = 0;
        }
    }

    /* Set compute options based on mode */
    if (cfg.req.summary_mode) {
        cfg.compute = COMPUTE_SUMMARY;
    } else if (cfg.disp.long_format) {
        cfg.compute = COMPUTE_LONG;
    } else {
        cfg.compute = COMPUTE_BASIC;
    }

    /* Enable sizes if needed for sorting/filtering in short mode */
    if (cfg.req.sort_by == SORT_SIZE) cfg.compute.sizes = 1;
    if (cfg.req.min_size > 0) cfg.compute.sizes = 1;

    /* Load the opaque-directory list (dirs shown but never descended into)
     * and the firmlink pair table (macOS) before any parallel work */
    opaque_dirs_load(cfg.env.config_dir);
    firmlinks_load();

    /* Load icons, file types, shebangs, and display settings in one pass over
     * config.toml (the separator keeps its built-in default if absent). */
    Icons icons;
    FileTypes filetypes;
    Shebangs shebangs;
    icons_init_defaults(&icons);
    filetypes_init(&filetypes);
    shebangs_init(&shebangs);
    config_load_all(cfg.env.config_dir, &icons, &filetypes, &shebangs,
                    cfg.disp.column_separator, sizeof(cfg.disp.column_separator));

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
                path_abbreviate_home(missing, shown, sizeof(shown), cfg.env.home);
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
            cfg.req.summary_mode = 1;
        }
    }

    int continuation[L_MAX_DEPTH] = {0};

    /* Build all trees first. The git cache is scoped to the whole invocation,
     * not to a single argument: it is keyed by absolute path and already holds
     * many repo roots at once, so every argument's tree is built into one
     * shared cache. This keeps the data layer input-count-agnostic — the
     * view, flat-list, and interactive renderers all query the same cache by
     * path (the picker flattens all trees into one list, so a per-argument
     * cache would leave non-first inputs without git data). */
    TreeNode **trees = xmalloc(dir_count * sizeof(TreeNode *));
    GitCache git;
    git_cache_init(&git);

    for (int i = 0; i < dir_count; i++) {
        if (cfg.req.show_ancestry) {
            trees[i] = build_ancestry_tree_from_config(dirs[i], &git, &cfg, &icons);
        } else {
            trees[i] = build_tree_from_config(dirs[i], &git, &cfg, &icons);
        }
    }

    /* Pre-compute visibility flags for filtering */
    if (cfg.req.git_only) {
        for (int i = 0; i < dir_count; i++) {
            compute_git_status_flags(trees[i], &git);
        }
    }
    if (cfg.req.grep_pattern) {
        for (int i = 0; i < dir_count; i++) {
            compute_grep_flags(trees[i], cfg.req.grep_pattern);
        }
    }

    /* Flatten the forest into the exact rows to draw, attribute git changes
     * to their nearest visible ancestor, and measure column widths — all
     * over the same row set (view.c). Interactive mode re-derives its own
     * visible set live but starts from these widths and summaries. */
    View *view = view_build(trees, dir_count, &cfg, &git, &icons);

    PrintContext ctx = {
        .git = &git,
        .icons = &icons,
        .filetypes = &filetypes,
        .shebangs = &shebangs,
        .cfg = &cfg,
        .columns = cfg.disp.long_format ? view->cols : NULL,
        .continuation = continuation,
        .diff_add_width = view->diff_add_width,
        .diff_del_width = view->diff_del_width,
        .term_width = cfg.disp.is_tty ? get_terminal_width() : 0
    };

    int exit_code = 0;
    if (cfg.req.interactive) {
        char *selected = select_run(trees, dir_count, &ctx);
        if (selected) {
            printf("%s\n", selected);
            free(selected);
        } else {
            exit_code = 1;  /* No selection made (quit/ESC) */
        }
    } else if (cfg.req.summary_mode) {
        for (int i = 0; i < dir_count; i++) {
            if (view->tree_no_matches[i]) {
                printf("%sNo matches.%s\n", CLR(&cfg, COLOR_RED), RST(&cfg));
                continue;
            }
            summary_prepare(trees[i], &ctx);
            print_summary(trees[i], &ctx);
        }
    } else {
        render_view(view, &ctx);
    }

    /* Cleanup */
    view_free(view);
    for (int i = 0; i < dir_count; i++) {
        tree_node_free(trees[i]);
        free(trees[i]);
    }
    free(trees);
    git_cache_free(&git);

    cache_unload();
    return exit_code;
}
