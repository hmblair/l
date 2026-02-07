/*
 * icons.h - Icon types, loading, and lookup
 */

#ifndef L_ICONS_H
#define L_ICONS_H

#include "common.h"

/* ============================================================================
 * File Types
 * ============================================================================ */

typedef enum {
    FTYPE_UNKNOWN,
    FTYPE_DIR,
    FTYPE_FILE,
    FTYPE_EXEC,
    FTYPE_DEVICE,
    FTYPE_SOCKET,
    FTYPE_FIFO,
    FTYPE_SYMLINK,
    FTYPE_SYMLINK_DIR,
    FTYPE_SYMLINK_EXEC,
    FTYPE_SYMLINK_DEVICE,
    FTYPE_SYMLINK_SOCKET,
    FTYPE_SYMLINK_FIFO,
    FTYPE_SYMLINK_BROKEN
} FileType;

/* ============================================================================
 * Icons Configuration
 * ============================================================================ */

/* Extension icon mapping */
typedef struct {
    char ext[L_MAX_EXT_LEN];
    char icon[L_MAX_ICON_LEN];
} ExtIcon;

/* Extension to file type name mapping */
#define L_MAX_FILETYPE_NAME 32
#define L_MAX_FILETYPES 256
#define L_MAX_SHEBANGS 64

typedef struct {
    char ext[L_MAX_EXT_LEN];
    char name[L_MAX_FILETYPE_NAME];
} FileTypeMapping;

typedef struct {
    FileTypeMapping mappings[L_MAX_FILETYPES];
    int count;
} FileTypes;

/* Shebang interpreter to file type name mapping */
typedef struct {
    char interp[L_MAX_EXT_LEN];
    char name[L_MAX_FILETYPE_NAME];
} ShebangMapping;

typedef struct {
    ShebangMapping mappings[L_MAX_SHEBANGS];
    int count;
} Shebangs;

/* Icon fields: X(field_name, config_key) — single source of truth for struct + config */
#define ICON_FIELDS(X) \
    X(default_icon,     "default")          \
    X(symlink,          "symlink")          \
    X(symlink_dir,      "symlink_dir")      \
    X(symlink_exec,     "symlink_exec")     \
    X(symlink_file,     "symlink_file")     \
    X(closed_directory, "closed_directory") \
    X(open_directory,   "open_directory")   \
    X(locked_dir,       "locked_dir")       \
    X(executable,       "executable")       \
    X(device,           "device")           \
    X(socket,           "socket")           \
    X(fifo,             "fifo")             \
    X(file,             "file")             \
    X(binary,           "binary")           \
    X(git_modified,     "git_modified")     \
    X(git_untracked,    "git_untracked")    \
    X(git_staged,       "git_staged")       \
    X(git_deleted,      "git_deleted")      \
    X(git_upstream,     "git_upstream")     \
    X(readonly,         "readonly")         \
    X(count_files,      "count_files")      \
    X(count_lines,      "count_lines")      \
    X(count_pixels,     "count_pixels")     \
    X(count_duration,   "count_duration")   \
    X(count_pages,      "count_pages")      \
    X(cursor,           "cursor")

/* Icons configuration */
typedef struct Icons {
    #define X(field, key) char field[L_MAX_ICON_LEN];
    ICON_FIELDS(X)
    #undef X
    ExtIcon ext_icons[L_MAX_EXT_ICONS];
    int ext_count;
} Icons;

/* Helper macro */
#define ICON_OR_FALLBACK(preferred, fallback) \
    ((preferred)[0] ? (preferred) : (fallback))

/* ============================================================================
 * Icons Functions
 * ============================================================================ */

void icons_init_defaults(Icons *icons);
void icons_load(Icons *icons, const char *script_dir);
const char *get_icon(const Icons *icons, FileType type, int is_expanded,
                     int is_locked, int is_binary, const char *name);
const char *get_ext_icon(const Icons *icons, const char *name);

/* File type functions */
void filetypes_init(FileTypes *ft);
void filetypes_load(FileTypes *ft, const char *script_dir);
const char *filetypes_lookup(const FileTypes *ft, const char *path);

/* Shebang functions */
void shebangs_init(Shebangs *sb);
void shebangs_load(Shebangs *sb, const char *script_dir);
const char *shebangs_lookup(const Shebangs *sb, const char *interp);

#endif /* L_ICONS_H */
