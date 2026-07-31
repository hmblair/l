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
 * Config File Reading
 * ============================================================================ */

int toml_read(const char *path, toml_cb cb, void *ud) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[L_TOML_LINE_MAX];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                size_t n = (size_t)(end - p) - 1;
                if (n >= sizeof(section)) n = sizeof(section) - 1;
                memcpy(section, p + 1, n);
                section[n] = '\0';
            }
            continue;
        }

        /* key = "value" -- key ends at '=' or whitespace */
        char *key = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p)) p++;
        if (p == key) continue;
        char *key_end = p;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '=') continue;
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p != '"') continue;
        p++;

        char *value = p;
        while (*p && *p != '"') p++;
        *p = '\0';
        *key_end = '\0';

        cb(section, key, value, ud);
    }

    fclose(f);
    return 0;
}

void split_csv(char *list, csv_cb cb, void *ud) {
    char *item;
    while ((item = strsep(&list, ",")) != NULL) {
        while (*item && isspace((unsigned char)*item)) item++;
        char *end = item + strlen(item);
        while (end > item && isspace((unsigned char)end[-1])) *--end = '\0';
        if (*item) cb(item, ud);
    }
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

static void opaque_add_cb(const char *name, void *ud) {
    (void)ud;
    opaque_dir_add(name);
}

static void opaque_toml_cb(const char *section, const char *key, char *value,
                           void *ud) {
    (void)ud;
    if (strcmp(section, "opaque") == 0 && strcmp(key, "names") == 0) {
        split_csv(value, opaque_add_cb, NULL);
    }
}

/* Parse the comma-separated names in the [opaque] section's `names` key,
 * e.g. `names = "__pycache__, node_modules, .venv"`. */
static void opaque_dirs_parse_file(const char *config_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", config_dir, L_CONFIG_FILE);
    toml_read(path, opaque_toml_cb, NULL);
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
 * Firmlinks (macOS)
 * ============================================================================ */

#include <pthread.h>

#define L_MAX_FIRMLINKS 64

typedef struct {
    ino_t ino;
    /* memoized full stats of the shared content (write-once per run) */
    off_t size;
    long file_count;
    dev_t dev;
    int have_stats;
    char alias[PATH_MAX];    /* user-visible side, e.g. /Users */
    char target[PATH_MAX];   /* data-volume side */
    char lca[PATH_MAX];      /* deepest directory containing both */
} FirmlinkPair;

static FirmlinkPair g_firmlinks[L_MAX_FIRMLINKS];
static int g_firmlink_count = 0;
static int g_firmlinks_loaded = 0;
static pthread_mutex_t g_firmlink_lock = PTHREAD_MUTEX_INITIALIZER;

/* Deepest directory that is an ancestor of both paths */
static void path_common_dir(const char *a, const char *b, char *out, size_t n) {
    size_t i = 0, last_slash = 0;
    while (a[i] && a[i] == b[i]) {
        if (a[i] == '/') last_slash = i;
        i++;
    }
    if (last_slash == 0) {
        snprintf(out, n, "/");
    } else {
        if (last_slash >= n) last_slash = n - 1;
        memcpy(out, a, last_slash);
        out[last_slash] = '\0';
    }
}

void firmlinks_load(void) {
    if (g_firmlinks_loaded) return;
    g_firmlinks_loaded = 1;

#if defined(__APPLE__) && defined(__MACH__)
    FILE *f = fopen("/usr/share/firmlinks", "r");
    if (!f) return;

    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), f) && g_firmlink_count < L_MAX_FIRMLINKS) {
        char *tab = strchr(line, '\t');
        if (!tab || line[0] != '/') continue;
        *tab = '\0';
        char *rel = tab + 1;
        rel[strcspn(rel, "\r\n")] = '\0';
        if (!*rel) continue;

        FirmlinkPair *p = &g_firmlinks[g_firmlink_count];
        snprintf(p->alias, sizeof(p->alias), "%s", line);
        snprintf(p->target, sizeof(p->target), "/System/Volumes/Data/%s", rel);

        /* Only keep pairs that exist and really are the same directory */
        struct stat sa, st;
        if (stat(p->alias, &sa) != 0 || stat(p->target, &st) != 0) continue;
        if (sa.st_dev != st.st_dev || sa.st_ino != st.st_ino) continue;
        if (!S_ISDIR(sa.st_mode)) continue;

        p->dev = sa.st_dev;
        p->ino = sa.st_ino;
        path_common_dir(p->alias, p->target, p->lca, sizeof(p->lca));
        p->have_stats = 0;
        g_firmlink_count++;
    }
    fclose(f);
#endif
}

void firmlink_stats_reset(void) {
    pthread_mutex_lock(&g_firmlink_lock);
    for (int i = 0; i < g_firmlink_count; i++) {
        g_firmlinks[i].have_stats = 0;
    }
    pthread_mutex_unlock(&g_firmlink_lock);
}

int firmlink_count(void) {
    return g_firmlink_count;
}

const char *firmlink_alias(int idx) {
    return g_firmlinks[idx].alias;
}

const char *firmlink_lca(int idx) {
    return g_firmlinks[idx].lca;
}

int firmlink_lookup_inode(dev_t dev, ino_t ino) {
    for (int i = 0; i < g_firmlink_count; i++) {
        if (g_firmlinks[i].dev == dev && g_firmlinks[i].ino == ino) return i;
    }
    return -1;
}

int firmlink_lookup_path(const char *path) {
    for (int i = 0; i < g_firmlink_count; i++) {
        if (strcmp(g_firmlinks[i].alias, path) == 0 ||
            strcmp(g_firmlinks[i].target, path) == 0) return i;
    }
    return -1;
}

int firmlink_stats_get(int idx, off_t *size, long *file_count) {
    int have;
    pthread_mutex_lock(&g_firmlink_lock);
    have = g_firmlinks[idx].have_stats;
    if (have) {
        *size = g_firmlinks[idx].size;
        *file_count = g_firmlinks[idx].file_count;
    }
    pthread_mutex_unlock(&g_firmlink_lock);
    return have;
}

void firmlink_stats_offer(int idx, off_t size, long file_count) {
    pthread_mutex_lock(&g_firmlink_lock);
    if (!g_firmlinks[idx].have_stats) {
        g_firmlinks[idx].size = size;
        g_firmlinks[idx].file_count = file_count;
        g_firmlinks[idx].have_stats = 1;
    }
    pthread_mutex_unlock(&g_firmlink_lock);
}

void firmlink_adjust_dir(const char *dir_path, off_t *size, long *file_count) {
    for (int i = 0; i < g_firmlink_count; i++) {
        if (strcmp(g_firmlinks[i].lca, dir_path) != 0) continue;
        off_t s;
        long c;
        if (!firmlink_stats_get(i, &s, &c)) continue;  /* endpoint never walked */
        if (*size >= 0) {
            *size -= s;
            if (*size < 0) *size = 0;
        }
        if (*file_count >= 0 && c >= 0) {
            *file_count -= c;
            if (*file_count < 0) *file_count = 0;
        }
    }
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
    unsigned char c;
    while ((c = (unsigned char)*str++))
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
    /* v3: firmlink-aware values (both endpoints stored full, dedup only at
     * each pair's common ancestor). v2 databases encode the old first-wins
     * attribution with 0|0 duplicate markers and must not be read. */
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/sizes-v3.db", home ? home : "/tmp");
}

void daemon_status_get_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/status", home ? home : "/tmp");
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
