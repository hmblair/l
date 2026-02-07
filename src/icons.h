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

/* Icons configuration */
typedef struct Icons {
    char default_icon[L_MAX_ICON_LEN];
    char symlink[L_MAX_ICON_LEN];
    char symlink_dir[L_MAX_ICON_LEN];
    char symlink_exec[L_MAX_ICON_LEN];
    char symlink_file[L_MAX_ICON_LEN];
    char closed_directory[L_MAX_ICON_LEN];
    char open_directory[L_MAX_ICON_LEN];
    char locked_dir[L_MAX_ICON_LEN];
    char executable[L_MAX_ICON_LEN];
    char device[L_MAX_ICON_LEN];
    char socket[L_MAX_ICON_LEN];
    char fifo[L_MAX_ICON_LEN];
    char file[L_MAX_ICON_LEN];
    char binary[L_MAX_ICON_LEN];
    char git_modified[L_MAX_ICON_LEN];
    char git_untracked[L_MAX_ICON_LEN];
    char git_staged[L_MAX_ICON_LEN];
    char git_deleted[L_MAX_ICON_LEN];
    char git_upstream[L_MAX_ICON_LEN];
    char readonly[L_MAX_ICON_LEN];
    char count_files[L_MAX_ICON_LEN];
    char count_lines[L_MAX_ICON_LEN];
    char count_pixels[L_MAX_ICON_LEN];
    char count_duration[L_MAX_ICON_LEN];
    char count_pages[L_MAX_ICON_LEN];
    char cursor[L_MAX_ICON_LEN];
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
