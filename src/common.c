/*
 * common.c - Shared utility implementations
 */

#include "common.h"
#include <errno.h>

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
    snprintf(path, sizeof(path), "%s/.cache/l/config", home);

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

int path_is_git_dir(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    return strcmp(name, ".git") == 0;
}

int path_should_skip_firmlink(const char *path) {
    /* Only skip /System/Volumes/Data - it's firmlinked and would cause double-counting.
     * Other volumes like Preboot, VM, Update are real storage and should be counted. */
    if (strncmp(path, "/System/Volumes/Data", 20) == 0 &&
        (path[20] == '/' || path[20] == '\0')) return 1;
    if (strncmp(path, "//System/Volumes/Data", 21) == 0 &&
        (path[21] == '/' || path[21] == '\0')) return 1;
    return 0;
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
        strcpy(resolved, "/");
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
        if (pos == 0) strcpy(resolved, "/");
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
    snprintf(buf, len, "%s/.cache/l/sizes.db", home ? home : "/tmp");
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
