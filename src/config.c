/*
 * config.c - Config file discovery and loading
 *
 * All of config.toml is loaded in one toml_read pass (config_load_all); the
 * per-section state lives in a ConfigLoad carried through the callbacks.
 */

#include "config.h"
#include <ctype.h>

/* ============================================================================
 * Derived Request State
 * ============================================================================ */

void request_set_git_only(Request *req, int on) {
    req->git_only = on;
    req->show_ancestry = on || req->ancestry_explicit;
}

/* ============================================================================
 * Config Directory Discovery
 * ============================================================================ */

void resolve_source_dir(const char *argv0, char *src_dir, size_t len) {
    char exe_abs[PATH_MAX];
    char try_path[PATH_MAX];
    int found = 0;

    if (strchr(argv0, '/')) {
        if (realpath(argv0, exe_abs) != NULL) found = 1;
    } else {
        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = xstrdup(path_env);
            char *saveptr;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir) {
                snprintf(try_path, sizeof(try_path), "%s/%s", dir, argv0);
                if (access(try_path, X_OK) == 0 && realpath(try_path, exe_abs) != NULL) {
                    found = 1;
                    break;
                }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
    }

    /* 1. Check same directory as binary (development) */
    if (found) {
        char *slash = strrchr(exe_abs, '/');
        if (slash) {
            *slash = '\0';
            if (strlen(exe_abs) + sizeof("/" L_CONFIG_FILE) <= sizeof(try_path)) {
                snprintf(try_path, sizeof(try_path), "%s/%s", exe_abs, L_CONFIG_FILE);
                if (access(try_path, R_OK) == 0) {
                    strncpy(src_dir, exe_abs, len - 1);
                    src_dir[len - 1] = '\0';
                    return;
                }
            }
        }
    }

    /* 2. Check ~/.config/l/ (installed) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(try_path, sizeof(try_path), "%s/.config/l/%s", home, L_CONFIG_FILE);
        if (access(try_path, R_OK) == 0) {
            snprintf(src_dir, len, "%s/.config/l", home);
            return;
        }
    }

    /* 3. Check /usr/local/share/l/ (system-wide) */
    snprintf(try_path, sizeof(try_path), "/usr/local/share/l/%s", L_CONFIG_FILE);
    if (access(try_path, R_OK) == 0) {
        strncpy(src_dir, "/usr/local/share/l", len - 1);
        src_dir[len - 1] = '\0';
        return;
    }

    /* Fallback to current directory */
    strncpy(src_dir, ".", len - 1);
    src_dir[len - 1] = '\0';
}

/* ============================================================================
 * Icon Key Mapping
 * ============================================================================ */

static const struct { const char *key; size_t offset; } icon_keys[] = {
    #define X(field, key) { key, offsetof(Icons, field) },
    ICON_FIELDS(X)
    #undef X
    { NULL, 0 }
};

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

/* ============================================================================
 * Single-pass Loader
 * ============================================================================ */

typedef struct {
    Icons *icons;
    FileTypes *ft;
    Shebangs *sb;
    char *separator;
    size_t separator_len;
    const char *value;   /* value of the line currently being csv-split */
} ConfigLoad;

static void ext_icon_add(const char *ext, void *ud) {
    ConfigLoad *cl = ud;
    Icons *icons = cl->icons;
    if (icons->ext_count >= L_MAX_EXT_ICONS) return;
    ExtIcon *ei = &icons->ext_icons[icons->ext_count++];
    strncpy(ei->ext, ext, L_MAX_EXT_LEN - 1);
    ei->ext[L_MAX_EXT_LEN - 1] = '\0';
    strncpy(ei->icon, cl->value, L_MAX_ICON_LEN - 1);
    ei->icon[L_MAX_ICON_LEN - 1] = '\0';
}

static void filetype_add(const char *ext, void *ud) {
    ConfigLoad *cl = ud;
    FileTypes *ft = cl->ft;
    if (ft->count >= L_MAX_FILETYPES) return;
    FileTypeMapping *m = &ft->mappings[ft->count++];
    strncpy(m->ext, ext, L_MAX_EXT_LEN - 1);
    m->ext[L_MAX_EXT_LEN - 1] = '\0';
    strncpy(m->name, cl->value, L_MAX_FILETYPE_NAME - 1);
    m->name[L_MAX_FILETYPE_NAME - 1] = '\0';
}

static void shebang_add(const char *interp, void *ud) {
    ConfigLoad *cl = ud;
    Shebangs *sb = cl->sb;
    if (sb->count >= L_MAX_SHEBANGS) return;
    ShebangMapping *m = &sb->mappings[sb->count++];
    strncpy(m->interp, interp, L_MAX_EXT_LEN - 1);
    m->interp[L_MAX_EXT_LEN - 1] = '\0';
    strncpy(m->name, cl->value, L_MAX_FILETYPE_NAME - 1);
    m->name[L_MAX_FILETYPE_NAME - 1] = '\0';
}

static void config_cb(const char *section, const char *key, char *value,
                      void *ud) {
    ConfigLoad *cl = ud;
    char keybuf[L_TOML_LINE_MAX];

    if (strcmp(section, "icons") == 0) {
        icons_set(cl->icons, key, value);
    } else if (strcmp(section, "display") == 0) {
        /* UI icons (git status, counts, cursor, ...) live under [display];
         * non-icon keys simply find no match in icons_set. */
        if (strcmp(key, "column_separator") == 0 && cl->separator) {
            strncpy(cl->separator, value, cl->separator_len - 1);
            cl->separator[cl->separator_len - 1] = '\0';
        }
        icons_set(cl->icons, key, value);
    } else if (strcmp(section, "extensions") == 0) {
        strncpy(keybuf, key, sizeof(keybuf) - 1);
        keybuf[sizeof(keybuf) - 1] = '\0';
        cl->value = value;
        split_csv(keybuf, ext_icon_add, cl);
    } else if (strcmp(section, "filetypes") == 0) {
        strncpy(keybuf, key, sizeof(keybuf) - 1);
        keybuf[sizeof(keybuf) - 1] = '\0';
        cl->value = value;
        split_csv(keybuf, filetype_add, cl);
    } else if (strcmp(section, "shebangs") == 0) {
        strncpy(keybuf, key, sizeof(keybuf) - 1);
        keybuf[sizeof(keybuf) - 1] = '\0';
        cl->value = value;
        split_csv(keybuf, shebang_add, cl);
    }
}

void config_load_all(const char *config_dir, Icons *icons, FileTypes *ft,
                     Shebangs *sb, char *separator, size_t separator_len) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", config_dir, L_CONFIG_FILE);

    ConfigLoad cl = {
        .icons = icons,
        .ft = ft,
        .sb = sb,
        .separator = separator,
        .separator_len = separator_len,
        .value = NULL,
    };
    toml_read(path, config_cb, &cl);
}
