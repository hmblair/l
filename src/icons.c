/*
 * icons.c - Icon loading and lookup
 */

#include "icons.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* ============================================================================
 * Icon Key Mapping
 * ============================================================================ */

static const struct { const char *key; size_t offset; } icon_keys[] = {
    { "default",           offsetof(Icons, default_icon) },
    { "closed_directory",  offsetof(Icons, closed_directory) },
    { "open_directory",    offsetof(Icons, open_directory) },
    { "locked_dir",        offsetof(Icons, locked_dir) },
    { "file",           offsetof(Icons, file) },
    { "binary",         offsetof(Icons, binary) },
    { "executable",     offsetof(Icons, executable) },
    { "device",         offsetof(Icons, device) },
    { "socket",         offsetof(Icons, socket) },
    { "fifo",           offsetof(Icons, fifo) },
    { "symlink",        offsetof(Icons, symlink) },
    { "symlink_dir",    offsetof(Icons, symlink_dir) },
    { "symlink_exec",   offsetof(Icons, symlink_exec) },
    { "symlink_file",   offsetof(Icons, symlink_file) },
    { "symlink_broken", offsetof(Icons, symlink_broken) },
    { "git_modified",   offsetof(Icons, git_modified) },
    { "git_untracked",  offsetof(Icons, git_untracked) },
    { "git_staged",     offsetof(Icons, git_staged) },
    { "git_deleted",    offsetof(Icons, git_deleted) },
    { "git_upstream",   offsetof(Icons, git_upstream) },
    { "readonly",       offsetof(Icons, readonly) },
    { "count_files",    offsetof(Icons, count_files) },
    { "count_lines",    offsetof(Icons, count_lines) },
    { "count_pixels",   offsetof(Icons, count_pixels) },
    { "count_duration", offsetof(Icons, count_duration) },
    { "count_pages",    offsetof(Icons, count_pages) },
    { "cursor",         offsetof(Icons, cursor) },
    { NULL, 0 }
};

/* ============================================================================
 * Icons Functions
 * ============================================================================ */

void icons_init_defaults(Icons *icons) {
    memset(icons, 0, sizeof(Icons));
}

static void icons_set(Icons *icons, const char *key, const char *value) {
    for (int i = 0; icon_keys[i].key; i++) {
        if (strcmp(key, icon_keys[i].key) == 0) {
            char *dest = (char *)icons + icon_keys[i].offset;
            strncpy(dest, value, L_MAX_ICON_LEN - 1);
            dest[L_MAX_ICON_LEN - 1] = '\0';
            return;
        }
    }
}

static int parse_toml_line(const char *line, char *key, size_t key_len,
                           char *value, size_t value_len) {
    const char *p = line;

    while (*p && isspace(*p)) p++;
    if (*p == '#' || *p == '\0') return 0;

    const char *key_start = p;
    while (*p && *p != '=' && !isspace(*p)) p++;
    size_t klen = p - key_start;
    if (klen == 0 || klen >= key_len) return 0;

    memcpy(key, key_start, klen);
    key[klen] = '\0';

    while (*p && isspace(*p)) p++;
    if (*p != '=') return 0;
    p++;

    while (*p && isspace(*p)) p++;
    if (*p != '"') return 0;
    p++;

    const char *val_start = p;
    while (*p && *p != '"') p++;
    size_t vlen = p - val_start;
    if (vlen >= value_len) return 0;

    memcpy(value, val_start, vlen);
    value[vlen] = '\0';

    return 1;
}

void icons_load(Icons *icons, const char *script_dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/icons.toml", script_dir);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[L_TOML_LINE_MAX];
    char key[64], value[L_MAX_ICON_LEN];
    int in_extensions = 0;

    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '[') {
            in_extensions = (strncmp(p, "[extensions]", 12) == 0);
            continue;
        }

        if (!parse_toml_line(line, key, sizeof(key), value, sizeof(value)))
            continue;

        if (in_extensions) {
            /* Parse comma-separated extensions (e.g., "jpg,png,gif") */
            char *ext_list = key;
            char *ext;
            while ((ext = strsep(&ext_list, ",")) != NULL) {
                /* Skip leading whitespace */
                while (*ext && isspace(*ext)) ext++;
                /* Trim trailing whitespace */
                char *end = ext + strlen(ext) - 1;
                while (end > ext && isspace(*end)) *end-- = '\0';

                if (*ext && icons->ext_count < L_MAX_EXT_ICONS) {
                    size_t ext_len = strlen(ext);
                    size_t icon_len = strlen(value);
                    if (ext_len >= L_MAX_EXT_LEN) ext_len = L_MAX_EXT_LEN - 1;
                    if (icon_len >= L_MAX_ICON_LEN) icon_len = L_MAX_ICON_LEN - 1;
                    memcpy(icons->ext_icons[icons->ext_count].ext, ext, ext_len);
                    icons->ext_icons[icons->ext_count].ext[ext_len] = '\0';
                    memcpy(icons->ext_icons[icons->ext_count].icon, value, icon_len);
                    icons->ext_icons[icons->ext_count].icon[icon_len] = '\0';
                    icons->ext_count++;
                }
            }
        } else {
            icons_set(icons, key, value);
        }
    }

    fclose(f);
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
                     int is_locked, int is_binary, const char *name) {
    switch (type) {
        case FTYPE_DIR:
            if (is_locked) return icons->locked_dir;
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
        case FTYPE_SYMLINK:
            return ICON_OR_FALLBACK(icons->symlink_file, icons->symlink);
        case FTYPE_SYMLINK_DIR:
            return ICON_OR_FALLBACK(icons->symlink_dir, icons->symlink);
        case FTYPE_SYMLINK_EXEC:
            return ICON_OR_FALLBACK(icons->symlink_exec, icons->symlink);
        case FTYPE_SYMLINK_DEVICE:
            return icons->device;
        case FTYPE_SYMLINK_SOCKET:
            return icons->socket;
        case FTYPE_SYMLINK_FIFO:
            return icons->fifo;
        case FTYPE_SYMLINK_BROKEN:
            return ICON_OR_FALLBACK(icons->symlink_broken, icons->symlink);
        case FTYPE_UNKNOWN:
        default:
            return icons->default_icon;
    }
}
