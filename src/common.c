/*
 * common.c - Shared utility implementations
 */

#include "common.h"
#include <errno.h>
#include <ctype.h>
#include <fnmatch.h>

#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif

/* ============================================================================
 * ANSI Color Code Definitions
 * ============================================================================ */

const char *COLOR_RESET      = "\033[0m";
const char *COLOR_RED        = "\033[0;31m";
const char *COLOR_GREEN      = "\033[0;32m";
const char *COLOR_YELLOW     = "\033[0;33m";
const char *COLOR_BLUE       = "\033[0;34m";
const char *COLOR_MAGENTA    = "\033[0;35m";
const char *COLOR_CYAN       = "\033[0;36m";
const char *COLOR_GREY       = "\033[90m";
const char *COLOR_WHITE      = "\033[0;37m";
const char *COLOR_YELLOW_BOLD = "\033[1;33m";
const char *STYLE_BOLD       = "\033[1m";
const char *STYLE_ITALIC     = "\033[3m";

/* ============================================================================
 * Daemon Configuration
 * ============================================================================ */

static int g_scan_interval = L_SCAN_INTERVAL;
static int g_file_threshold = L_FILE_COUNT_THRESHOLD;
static int g_config_loaded = 0;

static void config_load(void) {
    if (g_config_loaded) return;
    g_config_loaded = 1;

    const char *home = getenv("HOME");
    if (!home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/l/daemon.conf", home);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        int val = atoi(eq + 1);
        if (val <= 0) continue;

        if (strcmp(line, "scan_interval") == 0) {
            g_scan_interval = val;
        } else if (strcmp(line, "file_threshold") == 0) {
            g_file_threshold = val;
        }
    }
    fclose(f);
}

int config_get_interval(void) {
    config_load();
    return g_scan_interval;
}

int config_get_threshold(void) {
    config_load();
    return g_file_threshold;
}

/* ============================================================================
 * Opaque Directories
 * ============================================================================ */

/* Populated once at startup (or lazily on first query) and read-only during the
 * possibly-multithreaded tree/scan walk, so no lock is needed. */
static char g_opaque_dirs[L_MAX_OPAQUE_DIRS][L_MAX_OPAQUE_NAME];
static int g_opaque_count = 0;
static int g_opaque_loaded = 0;

static void opaque_dir_add(const char *name) {
    if (!name || !*name || g_opaque_count >= L_MAX_OPAQUE_DIRS) return;
    for (int i = 0; i < g_opaque_count; i++) {
        if (strcmp(g_opaque_dirs[i], name) == 0) return;  /* de-dup */
    }
    strncpy(g_opaque_dirs[g_opaque_count], name, L_MAX_OPAQUE_NAME - 1);
    g_opaque_dirs[g_opaque_count][L_MAX_OPAQUE_NAME - 1] = '\0';
    g_opaque_count++;
}

/* Parse the comma-separated names in the [opaque] section's `names` key,
 * e.g. `names = "__pycache__, node_modules, .venv"`. */
static void opaque_dirs_parse_file(const char *config_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", config_dir, L_CONFIG_FILE);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[L_TOML_LINE_MAX];
    int in_section = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;
        if (*p == '[') {
            in_section = (strncmp(p, "[opaque]", 8) == 0);
            continue;
        }
        if (!in_section) continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        /* Trim the key and require it to be `names`. */
        char *kend = eq - 1;
        while (kend >= p && isspace((unsigned char)*kend)) *kend-- = '\0';
        if (strcmp(p, "names") != 0) continue;

        /* Take the quoted value, then split on commas. */
        char *v = eq + 1;
        while (*v && isspace((unsigned char)*v)) v++;
        if (*v == '"') v++;
        char *vend = v + strlen(v);
        while (vend > v && (isspace((unsigned char)vend[-1]) || vend[-1] == '"'))
            *--vend = '\0';

        char *name;
        while ((name = strsep(&v, ",")) != NULL) {
            while (*name && isspace((unsigned char)*name)) name++;
            char *nend = name + strlen(name);
            while (nend > name && isspace((unsigned char)nend[-1])) *--nend = '\0';
            if (*name) opaque_dir_add(name);
        }
    }
    fclose(f);
}

void opaque_dirs_load(const char *config_dir) {
    g_opaque_loaded = 1;
    if (config_dir && *config_dir) opaque_dirs_parse_file(config_dir);
}

/* Fall back to the installed config if nothing loaded it explicitly. This lets
 * the cache daemon (which has no resolved config dir) share l's opaque list, so
 * cached file counts match what a direct scan would produce. */
static void opaque_dirs_ensure_loaded(void) {
    if (g_opaque_loaded) return;
    g_opaque_loaded = 1;
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.config/l", home);
    opaque_dirs_parse_file(dir);
}

int path_name_is_opaque(const char *name) {
    opaque_dirs_ensure_loaded();
    for (int i = 0; i < g_opaque_count; i++) {
        /* fnmatch treats a literal pattern as an exact match, so plain names
         * (node_modules) and globs (*.egg-info) are handled uniformly. */
        if (fnmatch(g_opaque_dirs[i], name, 0) == 0) return 1;
    }
    return 0;
}

int path_is_opaque_dir(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return path_name_is_opaque(name);
}

/* ============================================================================
 * Memory Allocation
 * ============================================================================ */

L_NORETURN void die(const char *msg) {
    int tty = isatty(STDERR_FILENO);
    fprintf(stderr, "%sError:%s %s\n",
            tty ? COLOR_RED : "", tty ? COLOR_RESET : "", msg);
    exit(1);
}

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) die("Out of memory");
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p) die("Out of memory");
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count, size);
    if (!p) die("Out of memory");
    return p;
}

char *xstrdup(const char *s) {
    char *dup = strdup(s);
    if (!dup) die("Out of memory");
    return dup;
}

/* ============================================================================
 * Hashing
 * ============================================================================ */

/* Simple djb2 hash */
unsigned int hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % L_HASH_SIZE;
}

/* ============================================================================
 * Path Utilities
 * ============================================================================ */

void path_join(char *dest, size_t dest_len, const char *dir, const char *name) {
    size_t dir_len = strlen(dir);
    int need_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
    snprintf(dest, dest_len, need_slash ? "%s/%s" : "%s%s", dir, name);
}

int path_is_git_root(const char *path) {
    char git_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", path);
    struct stat st;
    return stat(git_path, &st) == 0;
}

void path_get_realpath(const char *path, char *resolved, const char *cwd) {
    char *rp = realpath(path, resolved);
    if (!rp) {
        /* If realpath fails, try to at least get the absolute path */
        if (path[0] == '/') {
            strncpy(resolved, path, PATH_MAX - 1);
            resolved[PATH_MAX - 1] = '\0';
            return;
        }
        snprintf(resolved, PATH_MAX, "%s/%s", cwd, path);
    }
}

void path_get_abspath(const char *path, char *resolved, const char *cwd) {
    char tmp[PATH_MAX];

    /* Make absolute */
    if (path[0] == '/') {
        strncpy(tmp, path, PATH_MAX - 1);
        tmp[PATH_MAX - 1] = '\0';
    } else {
        snprintf(tmp, PATH_MAX, "%s/%s", cwd, path);
    }

    /* Normalize . and .. components */
    char *components[PATH_MAX / 2];
    int depth = 0;
    int max_depth = PATH_MAX / 2;

    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;  /* Skip slashes */
        if (*p == '\0') break;

        char *start = p;
        while (*p && *p != '/') p++;  /* Find end of component */

        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            /* Skip . */
            continue;
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* Go up for .. */
            if (depth > 0) depth--;
        } else if (depth < max_depth) {
            /* Save component (with bounds check) */
            components[depth] = start;
            if (*p) *p++ = '\0';  /* Null-terminate */
            depth++;
        }
    }

    /* Rebuild path with bounds checking */
    if (depth == 0) {
        resolved[0] = '/';
        resolved[1] = '\0';
    } else {
        size_t pos = 0;
        for (int i = 0; i < depth && pos < PATH_MAX - 1; i++) {
            size_t comp_len = strlen(components[i]);
            if (pos + 1 + comp_len >= PATH_MAX) break;
            resolved[pos++] = '/';
            memcpy(resolved + pos, components[i], comp_len);
            pos += comp_len;
        }
        resolved[pos] = '\0';
        if (pos == 0) {
            resolved[0] = '/';
            resolved[1] = '\0';
        }
    }
}

void path_abbreviate_home(const char *path, char *buf, size_t len, const char *home) {
    size_t home_len = strlen(home);
    if (strncmp(path, home, home_len) == 0 &&
        (path[home_len] == '/' || path[home_len] == '\0')) {
        snprintf(buf, len, "~%s", path + home_len);
    } else {
        strncpy(buf, path, len - 1);
        buf[len - 1] = '\0';
    }
}

void cache_get_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/sizes-v2.db", home ? home : "/tmp");
}

int path_is_network_fs(const char *path) {
#ifdef __linux__
    /* Network filesystem magic numbers */
    #define NFS_SUPER_MAGIC     0x6969
    #define LUSTRE_SUPER_MAGIC  0x0BD00BD0
    #define GPFS_SUPER_MAGIC    0x47504653
    #define CIFS_MAGIC_NUMBER   0xFF534D42
    #define SMB_SUPER_MAGIC     0x517B
    #define CEPH_SUPER_MAGIC    0x00C36400
    #define AFS_SUPER_MAGIC     0x5346414F

    struct statfs st;
    if (statfs(path, &st) != 0) return 0;
    switch ((unsigned long)st.f_type) {
        case NFS_SUPER_MAGIC:
        case LUSTRE_SUPER_MAGIC:
        case GPFS_SUPER_MAGIC:
        case CIFS_MAGIC_NUMBER:
        case SMB_SUPER_MAGIC:
        case CEPH_SUPER_MAGIC:
        case AFS_SUPER_MAGIC:
            return 1;
    }
#else
    (void)path;  /* macOS/BSD: assume local (cache daemon handles slow dirs) */
#endif
    return 0;
}

int path_is_virtual_fs(const char *path) {
#ifdef __linux__
    /* Virtual/pseudo filesystem magic numbers */
    #define PROC_SUPER_MAGIC    0x9fa0
    #define SYSFS_MAGIC         0x62656572
    #define DEVTMPFS_MAGIC      0x01021994
    #define DEBUGFS_MAGIC       0x64626720
    #define SECURITYFS_MAGIC    0x73636673
    #define CGROUP_SUPER_MAGIC  0x27e0eb
    #define CGROUP2_SUPER_MAGIC 0x63677270

    struct statfs st;
    if (statfs(path, &st) != 0) return 0;
    switch ((unsigned long)st.f_type) {
        case PROC_SUPER_MAGIC:
        case SYSFS_MAGIC:
        case DEVTMPFS_MAGIC:
        case DEBUGFS_MAGIC:
        case SECURITYFS_MAGIC:
        case CGROUP_SUPER_MAGIC:
        case CGROUP2_SUPER_MAGIC:
            return 1;
    }
#else
    (void)path;
#endif
    return 0;
}
