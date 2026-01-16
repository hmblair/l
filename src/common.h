/*
 * common.h - Shared constants, types, and utility declarations
 */

#ifndef L_COMMON_H
#define L_COMMON_H

#define _GNU_SOURCE      /* For O_DIRECTORY, fdopendir, fstatat on Linux */
#define _DEFAULT_SOURCE  /* For DT_* constants on Linux */
#define _BSD_SOURCE      /* For compatibility */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

/* ============================================================================
 * Platform Detection
 * ============================================================================ */

#if defined(__APPLE__) && defined(__MACH__)
    #define PLATFORM_MACOS 1
    #define GET_MTIME(st) ((st).st_mtimespec.tv_sec)
#elif defined(__linux__)
    #define PLATFORM_LINUX 1
    #define GET_MTIME(st) ((st).st_mtim.tv_sec)
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    #define PLATFORM_BSD 1
    #define GET_MTIME(st) ((st).st_mtimespec.tv_sec)
#else
    #define PLATFORM_GENERIC 1
    #define GET_MTIME(st) ((st).st_mtime)
#endif

#ifndef PATH_MAX
    #define PATH_MAX 4096
#endif

/* Noreturn attribute */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    #define L_NORETURN _Noreturn
#elif defined(__GNUC__) || defined(__clang__)
    #define L_NORETURN __attribute__((noreturn))
#else
    #define L_NORETURN
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Size limits */
#define L_HASH_SIZE             4096
#define L_MAX_DEPTH             50
#define L_MAX_ICON_LEN          16
#define L_MAX_EXT_LEN           16
#define L_MAX_EXT_ICONS         256
#define L_INITIAL_FILE_CAPACITY 64

/* Line counting */
#define L_BINARY_CHECK_SIZE     512

/* Git */
#define L_GIT_HEAD_BUF_SIZE     256
#define L_GIT_STATUS_LINE_MAX   (PATH_MAX + 8)
#define L_SHELL_CMD_BUF_SIZE    (PATH_MAX * 2 + 64)
#define L_GIT_INDICATOR_SIZE    64

/* TOML parsing */
#define L_TOML_LINE_MAX         256

/* Time constants for relative time formatting */
#define L_SECONDS_PER_MINUTE    60
#define L_SECONDS_PER_HOUR      3600
#define L_SECONDS_PER_DAY       86400
#define L_SECONDS_PER_WEEK      604800

/* Daemon constants (defaults, can be overridden by config) */
#define L_SCAN_INTERVAL         1800    /* 30 minutes between scans */
#define L_FILE_COUNT_THRESHOLD  1000    /* Cache directories with >= this many files */
#define L_MAX_LOG_SIZE          (1024 * 1024)  /* 1MB max log size */

/* Daemon configuration (reads from ~/.cache/l/config) */
int config_get_interval(void);   /* Scan interval in seconds */
int config_get_threshold(void);  /* Min files to cache a directory */

/* Error codes */
#define L_OK                    0
#define L_ERR_NOMEM             (-1)
#define L_ERR_IO                (-2)
#define L_ERR_INVALID           (-3)

/* ============================================================================
 * Tree Drawing Characters (UTF-8)
 * ============================================================================ */

#define TREE_VERT   "│  "
#define TREE_BRANCH "├─ "
#define TREE_LAST   "└─ "
#define TREE_SPACE  "   "

/* ============================================================================
 * ANSI Color Codes
 * ============================================================================ */

extern const char *COLOR_RESET;
extern const char *COLOR_RED;
extern const char *COLOR_GREEN;
extern const char *COLOR_YELLOW;
extern const char *COLOR_BLUE;
extern const char *COLOR_MAGENTA;
extern const char *COLOR_CYAN;
extern const char *COLOR_GREY;
extern const char *COLOR_WHITE;
extern const char *COLOR_YELLOW_BOLD;
extern const char *STYLE_BOLD;
extern const char *STYLE_ITALIC;

/* ============================================================================
 * Path Utility Macros
 * ============================================================================ */

/* Efficient check for . or .. */
#define PATH_IS_DOT_OR_DOTDOT(name) \
    ((name)[0] == '.' && ((name)[1] == '\0' || ((name)[1] == '.' && (name)[2] == '\0')))

/* ============================================================================
 * Memory Allocation (die on failure)
 * ============================================================================ */

L_NORETURN void die(const char *msg);
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

/* ============================================================================
 * Hashing
 * ============================================================================ */

unsigned int hash_string(const char *str);

/* ============================================================================
 * Path Utilities
 * ============================================================================ */

/* Join directory and filename, handling trailing slashes */
void path_join(char *dest, size_t dest_len, const char *dir, const char *name);

/* Check if path is or ends with .git */
int path_is_git_dir(const char *path);

/* Check if path should be skipped (macOS APFS firmlinks) */
int path_should_skip_firmlink(const char *path);

/* Check if directory is a git repository root (contains .git) */
int path_is_git_root(const char *path);

/* Get absolute path, resolving symlinks */
void path_get_realpath(const char *path, char *resolved, const char *cwd);

/* Get absolute path WITHOUT resolving symlinks, but normalize . and .. */
void path_get_abspath(const char *path, char *resolved, const char *cwd);

/* Abbreviate home directory with ~ */
void path_abbreviate_home(const char *path, char *buf, size_t len, const char *home);

/* Check if path is on a network filesystem */
int path_is_network_fs(const char *path);

/* Check if path is on a virtual filesystem (proc, sysfs, etc.) */
int path_is_virtual_fs(const char *path);

/* Get cache database path */
void cache_get_path(char *buf, size_t len);

#endif /* L_COMMON_H */
