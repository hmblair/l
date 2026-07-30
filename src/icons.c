/*
 * icons.c - Icon loading and lookup
 */

#include "icons.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* ============================================================================
 * Icons Functions
 * ============================================================================ */

void icons_init_defaults(Icons *icons) {
    memset(icons, 0, sizeof(Icons));
}

const char *get_ext_icon(const Icons *icons, const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return NULL;

    const char *ext = dot + 1;
    for (int i = 0; i < icons->ext_count; i++) {
        if (strcmp(ext, icons->ext_icons[i].ext) == 0) {
            return icons->ext_icons[i].icon;
        }
    }
    return NULL;
}

const char *get_icon(const Icons *icons, FileType type, int is_expanded,
                     int is_binary, const char *name) {
    switch (type) {
        case FTYPE_DIR:
            return is_expanded ? icons->open_directory : icons->closed_directory;
        case FTYPE_FILE: {
            const char *ext_icon = get_ext_icon(icons, name);
            if (ext_icon) return ext_icon;
            if (is_binary && icons->binary[0]) return icons->binary;
            return icons->file;
        }
        case FTYPE_EXEC:
            return icons->executable;
        case FTYPE_DEVICE:
            return icons->device;
        case FTYPE_SOCKET:
            return icons->socket;
        case FTYPE_FIFO:
            return icons->fifo;
        case FTYPE_SYMLINK: {
            const char *ext_icon = get_ext_icon(icons, name);
            if (ext_icon) return ext_icon;
            if (is_binary && icons->binary[0]) return icons->binary;
            return icons->file;
        }
        case FTYPE_SYMLINK_DIR:
            return is_expanded ? icons->open_directory : icons->closed_directory;
        case FTYPE_SYMLINK_EXEC:
            return icons->executable;
        case FTYPE_SYMLINK_DEVICE:
            return icons->device;
        case FTYPE_SYMLINK_SOCKET:
            return icons->socket;
        case FTYPE_SYMLINK_FIFO:
            return icons->fifo;
        case FTYPE_SYMLINK_BROKEN:
            return icons->file;
        case FTYPE_UNKNOWN:
        default:
            return icons->default_icon;
    }
}

/* ============================================================================
 * File Types Functions
 * ============================================================================ */

void filetypes_init(FileTypes *ft) {
    memset(ft, 0, sizeof(FileTypes));
}

const char *filetypes_lookup(const FileTypes *ft, const char *path) {
    const char *ext = strrchr(path, '.');
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    /* No extension or extension at start of basename */
    if (!ext || ext < basename || ext == basename) {
        return NULL;
    }
    ext++;  /* skip the dot */

    for (int i = 0; i < ft->count; i++) {
        if (strcasecmp(ext, ft->mappings[i].ext) == 0) {
            return ft->mappings[i].name;
        }
    }
    return NULL;
}

/* ============================================================================
 * Shebang Functions
 * ============================================================================ */

void shebangs_init(Shebangs *sb) {
    memset(sb, 0, sizeof(Shebangs));
}

const char *shebangs_lookup(const Shebangs *sb, const char *interp) {
    for (int i = 0; i < sb->count; i++) {
        if (strcmp(interp, sb->mappings[i].interp) == 0) {
            return sb->mappings[i].name;
        }
    }
    return NULL;
}

