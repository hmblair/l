/*
 * ui.c - Display functions: icons, columns, tree, colors
 */

#include "ui.h"
#include "cache.h"
#include <dirent.h>
#include <ctype.h>
#include <fnmatch.h>
#include <time.h>

/* ============================================================================
 * Path Wrappers (using Config)
 * ============================================================================ */

void get_realpath(const char *path, char *resolved, const Config *cfg) {
    path_get_realpath(path, resolved, cfg->cwd);
}

void get_abspath(const char *path, char *resolved, const Config *cfg) {
    path_get_abspath(path, resolved, cfg->cwd);
}

void abbreviate_home(const char *path, char *buf, size_t len, const Config *cfg) {
    path_abbreviate_home(path, buf, len, cfg->home);
}

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
            snprintf(try_path, sizeof(try_path), "%s/icons.toml", exe_abs);
            if (access(try_path, R_OK) == 0) {
                strncpy(src_dir, exe_abs, len - 1);
                src_dir[len - 1] = '\0';
                return;
            }
        }
    }

    /* 2. Check ~/.config/l/ (installed) */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(try_path, sizeof(try_path), "%s/.config/l/icons.toml", home);
        if (access(try_path, R_OK) == 0) {
            snprintf(src_dir, len, "%s/.config/l", home);
            return;
        }
    }

    /* 3. Check /usr/local/share/l/ (system-wide) */
    if (access("/usr/local/share/l/icons.toml", R_OK) == 0) {
        strncpy(src_dir, "/usr/local/share/l", len - 1);
        src_dir[len - 1] = '\0';
        return;
    }

    /* Fallback to current directory */
    strncpy(src_dir, ".", len - 1);
    src_dir[len - 1] = '\0';
}

/* ============================================================================
 * Size and Time Formatting
 * ============================================================================ */

void format_size(off_t bytes, char *buf, size_t len) {
    const char *units[] = {"B", "K", "M", "G", "T", "P"};
    int unit_idx = 0;
    double size = (double)bytes;

    while (size >= 1024 && unit_idx < 5) {
        size /= 1024;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, len, "%lld%s", (long long)bytes, units[0]);
    } else if (size < 10) {
        snprintf(buf, len, "%.1f%s", size, units[unit_idx]);
    } else {
        snprintf(buf, len, "%.0f%s", size, units[unit_idx]);
    }
}

void format_relative_time(time_t mtime, char *buf, size_t len) {
    time_t now = time(NULL);
    long diff = (long)(now - mtime);

    if (diff < L_SECONDS_PER_MINUTE) {
        snprintf(buf, len, "now");
    } else if (diff < L_SECONDS_PER_HOUR) {
        snprintf(buf, len, "%ldm ago", diff / L_SECONDS_PER_MINUTE);
    } else if (diff < L_SECONDS_PER_DAY) {
        snprintf(buf, len, "%ldh ago", diff / L_SECONDS_PER_HOUR);
    } else if (diff < L_SECONDS_PER_WEEK) {
        snprintf(buf, len, "%ldd ago", diff / L_SECONDS_PER_DAY);
    } else {
        struct tm *tm = localtime(&mtime);
        strftime(buf, len, "%b %d", tm);
    }
}

/* ============================================================================
 * Column Formatters
 * ============================================================================ */

static void col_format_size(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;
    if (fe->size < 0) {
        snprintf(buf, len, "-");
    } else {
        format_size(fe->size, buf, len);
    }
}

static void col_format_lines(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;

    if (fe->file_count >= 0) {
        if (fe->file_count >= 1000000) {
            double m = fe->file_count / 1000000.0;
            snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
        } else if (fe->file_count >= 1000) {
            double k = fe->file_count / 1000.0;
            snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
        } else {
            snprintf(buf, len, "%ld", fe->file_count);
        }
    } else if (fe->is_image && fe->line_count >= 0) {
        /* line_count holds megapixels * 10 */
        double mp = fe->line_count / 10.0;
        if (mp >= 10.0) {
            snprintf(buf, len, "%.0fM", mp);
        } else {
            snprintf(buf, len, "%.1fM", mp);
        }
    } else if (fe->line_count == L_LINE_COUNT_EXCEEDED) {
        snprintf(buf, len, ">1M");
    } else if (fe->line_count >= 1000000) {
        double m = fe->line_count / 1000000.0;
        snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
    } else if (fe->line_count >= 1000) {
        double k = fe->line_count / 1000.0;
        snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
    } else if (fe->line_count >= 0) {
        snprintf(buf, len, "%d", fe->line_count);
    } else {
        snprintf(buf, len, "-");
    }
}

static void col_format_time(const FileEntry *fe, const Icons *icons, char *buf, size_t len) {
    (void)icons;
    format_relative_time(fe->mtime, buf, len);
}

void columns_init(Column *cols) {
    cols[COL_SIZE].name = "size";
    cols[COL_SIZE].width = 1;
    cols[COL_SIZE].format = col_format_size;

    cols[COL_LINES].name = "lines";
    cols[COL_LINES].width = 1;
    cols[COL_LINES].format = col_format_lines;

    cols[COL_TIME].name = "time";
    cols[COL_TIME].width = 1;
    cols[COL_TIME].format = col_format_time;
}

void columns_update_widths(Column *cols, const FileEntry *fe, const Icons *icons) {
    char buf[32];
    for (int i = 0; i < NUM_COLUMNS; i++) {
        cols[i].format(fe, icons, buf, sizeof(buf));
        int len = (int)strlen(buf);
        if (len > cols[i].width) cols[i].width = len;
    }
}

static void columns_reset_widths(Column *cols) {
    for (int i = 0; i < NUM_COLUMNS; i++) {
        cols[i].width = 1;
    }
}

static int is_filtering_active(const Config *cfg) {
    return cfg->git_only || cfg->grep_pattern;
}

static int node_is_visible(const TreeNode *node, const Config *cfg) {
    if (cfg->git_only && !node->has_git_status) return 0;
    if (cfg->grep_pattern && !node->matches_grep) return 0;
    return 1;
}

static void columns_update_visible_recursive(Column *cols, const TreeNode *node,
                                             const Icons *icons, const Config *cfg) {
    if (node_is_visible(node, cfg)) {
        columns_update_widths(cols, &node->entry, icons);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        columns_update_visible_recursive(cols, &node->children[i], icons, cfg);
    }
}

void columns_recalculate_visible(Column *cols, TreeNode **trees, int tree_count,
                                 const Icons *icons, const Config *cfg) {
    columns_reset_widths(cols);
    for (int i = 0; i < tree_count; i++) {
        columns_update_visible_recursive(cols, trees[i], icons, cfg);
    }
}

static int count_digits(int n) {
    if (n == 0) return 1;
    int count = 0;
    if (n < 0) { count = 1; n = -n; }  /* for minus sign */
    while (n > 0) { count++; n /= 10; }
    return count;
}

static void compute_diff_widths_recursive(const TreeNode *node, int *add_width, int *del_width,
                                          const Config *cfg) {
    if (node_is_visible(node, cfg)) {
        if (node->entry.diff_added > 0) {
            int w = count_digits(node->entry.diff_added);
            if (w > *add_width) *add_width = w;
        }
        if (node->entry.diff_removed > 0) {
            int w = count_digits(node->entry.diff_removed);
            if (w > *del_width) *del_width = w;
        }
    }
    for (size_t i = 0; i < node->child_count; i++) {
        compute_diff_widths_recursive(&node->children[i], add_width, del_width, cfg);
    }
}

void compute_diff_widths(TreeNode **trees, int tree_count, int *add_width, int *del_width,
                         const Config *cfg) {
    *add_width = 0;
    *del_width = 0;
    for (int i = 0; i < tree_count; i++) {
        compute_diff_widths_recursive(trees[i], add_width, del_width, cfg);
    }
}

const char *get_count_icon(const FileEntry *fe, const Icons *icons) {
    if (fe->file_count >= 0) {
        return icons->count_files;
    } else if (fe->is_image && fe->line_count >= 0) {
        return icons->count_pixels;
    } else if (fe->line_count >= 0 || fe->line_count == L_LINE_COUNT_EXCEEDED) {
        return icons->count_lines;
    }
    return "";
}

/* ============================================================================
 * Icons
 * ============================================================================ */

void icons_init_defaults(Icons *icons) {
    memset(icons, 0, sizeof(Icons));
}

static const struct { const char *key; size_t offset; } icon_keys[] = {
    { "default",        offsetof(Icons, default_icon) },
    { "directory",      offsetof(Icons, directory) },
    { "current_dir",    offsetof(Icons, current_dir) },
    { "locked_dir",     offsetof(Icons, locked_dir) },
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
                    strncpy(icons->ext_icons[icons->ext_count].ext, ext, L_MAX_EXT_LEN - 1);
                    icons->ext_icons[icons->ext_count].ext[L_MAX_EXT_LEN - 1] = '\0';
                    strncpy(icons->ext_icons[icons->ext_count].icon, value, L_MAX_ICON_LEN - 1);
                    icons->ext_icons[icons->ext_count].icon[L_MAX_ICON_LEN - 1] = '\0';
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

const char *get_icon(const Icons *icons, FileType type, int is_cwd,
                     int is_locked, int is_binary, const char *name) {
    switch (type) {
        case FTYPE_DIR:
            if (is_locked) return icons->locked_dir;
            return is_cwd ? icons->current_dir : icons->directory;
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

/* ============================================================================
 * File Type Detection
 * ============================================================================ */

static char *resolve_symlink(const char *path) {
    char target[PATH_MAX];
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len <= 0) return NULL;
    target[len] = '\0';

    char abs_target[PATH_MAX];
    if (realpath(path, abs_target) != NULL) {
        return xstrdup(abs_target);
    }

    if (target[0] == '/') {
        return xstrdup(target);
    }

    char dir[PATH_MAX];
    strncpy(dir, path, PATH_MAX - 1);
    dir[PATH_MAX - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = '\0';
        snprintf(abs_target, PATH_MAX, "%s%s", dir, target);
        return xstrdup(abs_target);
    }
    return xstrdup(target);
}

static FileType get_symlink_target_type(const struct stat *target_st) {
    if (S_ISDIR(target_st->st_mode)) return FTYPE_SYMLINK_DIR;
    if (S_ISCHR(target_st->st_mode) || S_ISBLK(target_st->st_mode)) return FTYPE_SYMLINK_DEVICE;
    if (S_ISSOCK(target_st->st_mode)) return FTYPE_SYMLINK_SOCKET;
    if (S_ISFIFO(target_st->st_mode)) return FTYPE_SYMLINK_FIFO;
    if (S_ISREG(target_st->st_mode) && (target_st->st_mode & S_IXUSR)) return FTYPE_SYMLINK_EXEC;
    return FTYPE_SYMLINK;
}

FileType detect_file_type(const char *path, struct stat *st, char **symlink_target) {
    struct stat lst;
    *symlink_target = NULL;

    if (lstat(path, &lst) != 0) return FTYPE_UNKNOWN;
    *st = lst;

    if (S_ISLNK(lst.st_mode)) {
        *symlink_target = resolve_symlink(path);
        if (!*symlink_target) return FTYPE_SYMLINK_BROKEN;

        struct stat target_st;
        if (stat(path, &target_st) != 0) return FTYPE_SYMLINK_BROKEN;

        *st = target_st;
        return get_symlink_target_type(&target_st);
    } else if (S_ISDIR(lst.st_mode)) {
        return FTYPE_DIR;
    } else if (S_ISCHR(lst.st_mode) || S_ISBLK(lst.st_mode)) {
        return FTYPE_DEVICE;
    } else if (S_ISSOCK(lst.st_mode)) {
        return FTYPE_SOCKET;
    } else if (S_ISFIFO(lst.st_mode)) {
        return FTYPE_FIFO;
    } else if (!S_ISREG(lst.st_mode)) {
        return FTYPE_UNKNOWN;
    } else if (lst.st_mode & S_IXUSR) {
        return FTYPE_EXEC;
    } else {
        return FTYPE_FILE;
    }
}

const char *get_file_color(FileType type, int is_cwd, int is_ignored, const Config *cfg) {
    if (!cfg->is_tty) return "";

    if (is_cwd) return COLOR_YELLOW_BOLD;
    if (is_ignored) return COLOR_GREY;

    switch (type) {
        case FTYPE_DIR:            return COLOR_BLUE;
        case FTYPE_EXEC:           return COLOR_GREEN;
        case FTYPE_DEVICE:         return COLOR_YELLOW;
        case FTYPE_SOCKET:         return COLOR_MAGENTA;
        case FTYPE_FIFO:           return COLOR_MAGENTA;
        case FTYPE_SYMLINK:        return COLOR_WHITE;
        case FTYPE_SYMLINK_DIR:    return COLOR_CYAN;
        case FTYPE_SYMLINK_EXEC:   return COLOR_GREEN;
        case FTYPE_SYMLINK_DEVICE: return COLOR_YELLOW;
        case FTYPE_SYMLINK_SOCKET: return COLOR_MAGENTA;
        case FTYPE_SYMLINK_FIFO:   return COLOR_MAGENTA;
        case FTYPE_SYMLINK_BROKEN: return COLOR_RED;
        default:                   return COLOR_WHITE;
    }
}

/* ============================================================================
 * Line Counting
 * ============================================================================ */

static int has_binary_extension(const char *path) {
    const char *dot = strrchr(path, '/');
    dot = dot ? strrchr(dot, '.') : strrchr(path, '.');
    if (!dot) return 0;
    dot++;

    static const char *exts[] = {
        "pdf", "png", "jpg", "jpeg", "gif", "bmp", "ico", "webp", "svg",
        "mp3", "mp4", "wav", "flac", "ogg", "avi", "mkv", "mov", "webm",
        "zip", "tar", "gz", "bz2", "xz", "7z", "rar", "dmg", "iso",
        "exe", "dll", "so", "dylib", "o", "a", "class", "pyc",
        "ttf", "otf", "woff", "woff2", "eot",
        "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods",
        "sqlite", "db", "bin", "dat",
        NULL
    };
    for (const char **e = exts; *e; e++) {
        if (strcasecmp(dot, *e) == 0) return 1;
    }
    return 0;
}

/* Get image dimensions from file header. Returns megapixels * 10, or -1 on failure. */
static int get_image_megapixels(const char *path) {
    const char *dot = strrchr(path, '/');
    dot = dot ? strrchr(dot, '.') : strrchr(path, '.');
    if (!dot) return -1;
    dot++;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char header[32];
    size_t n = fread(header, 1, sizeof(header), f);
    if (n < 24) { fclose(f); return -1; }

    int width = 0, height = 0;

    /* PNG: 89 50 4E 47 0D 0A 1A 0A, width at 16, height at 20 (big-endian) */
    if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
        width = (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19];
        height = (header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23];
    }
    /* GIF: 47 49 46, width at 6, height at 8 (little-endian) */
    else if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
        width = header[6] | (header[7] << 8);
        height = header[8] | (header[9] << 8);
    }
    /* JPEG: FF D8 FF, need to find SOF marker */
    else if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        fseek(f, 2, SEEK_SET);
        unsigned char buf[10];
        while (fread(buf, 1, 2, f) == 2) {
            if (buf[0] != 0xFF) break;
            int marker = buf[1];
            /* SOF0, SOF1, SOF2 markers contain dimensions */
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                if (fread(buf, 1, 7, f) == 7) {
                    height = (buf[3] << 8) | buf[4];
                    width = (buf[5] << 8) | buf[6];
                }
                break;
            }
            /* Skip other segments */
            if (fread(buf, 1, 2, f) != 2) break;
            int len = (buf[0] << 8) | buf[1];
            if (len < 2) break;
            fseek(f, len - 2, SEEK_CUR);
        }
    }
    /* BMP: 42 4D, width at 18, height at 22 (little-endian, signed for height) */
    else if (header[0] == 'B' && header[1] == 'M' && n >= 26) {
        width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
        int h = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
        height = h < 0 ? -h : h;  /* Height can be negative (top-down DIB) */
    }

    fclose(f);

    if (width <= 0 || height <= 0) return -1;

    /* Return megapixels * 10 for one decimal place precision */
    double mp = (double)width * height / 1000000.0;
    return (int)(mp * 10 + 0.5);
}

static int is_binary_file(FILE *f) {
    unsigned char buf[L_BINARY_CHECK_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    rewind(f);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0') return 1;
    }
    return 0;
}

int count_file_lines(const char *path) {
    if (has_binary_extension(path)) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    if (is_binary_file(f)) {
        fclose(f);
        return -1;
    }

    int count = 0;
    char buf[L_READ_BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                count++;
                if (count > L_LINE_COUNT_LIMIT) {
                    fclose(f);
                    return L_LINE_COUNT_EXCEEDED;
                }
            }
        }
    }
    fclose(f);
    return count;
}

/* ============================================================================
 * File List Management
 * ============================================================================ */

void file_list_init(FileList *list) {
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void file_list_add(FileList *list, FileEntry *entry) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : L_INITIAL_FILE_CAPACITY;
        list->entries = xrealloc(list->entries, list->capacity * sizeof(FileEntry));
    }
    list->entries[list->count++] = *entry;
}

void file_entry_free(FileEntry *entry) {
    free(entry->path);
    free(entry->symlink_target);
}

void file_list_free(FileList *list) {
    for (size_t i = 0; i < list->count; i++) {
        file_entry_free(&list->entries[i]);
    }
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* ============================================================================
 * Directory Reading
 * ============================================================================ */

static int entry_cmp_name(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    return strcasecmp(ea->name, eb->name);
}

static int entry_cmp_size(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    if (eb->size > ea->size) return 1;
    if (eb->size < ea->size) return -1;
    return 0;
}

static int entry_cmp_time(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    if (eb->mtime > ea->mtime) return 1;
    if (eb->mtime < ea->mtime) return -1;
    return 0;
}

static void reverse_file_list(FileList *list) {
    for (size_t i = 0; i < list->count / 2; i++) {
        FileEntry tmp = list->entries[i];
        list->entries[i] = list->entries[list->count - 1 - i];
        list->entries[list->count - 1 - i] = tmp;
    }
}

static void sort_file_list(FileList *list, const Config *cfg) {
    if (list->count == 0) return;

    int (*cmp)(const void *, const void *) = NULL;

    switch (cfg->sort_by) {
        case SORT_NAME: cmp = entry_cmp_name; break;
        case SORT_SIZE: cmp = entry_cmp_size; break;
        case SORT_TIME: cmp = entry_cmp_time; break;
        default: return;
    }

    qsort(list->entries, list->count, sizeof(FileEntry), cmp);

    if (cfg->sort_reverse) {
        reverse_file_list(list);
    }
}

int read_directory(const char *dir_path, FileList *list, const Config *cfg) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (PATH_IS_DOT_OR_DOTDOT(entry->d_name))
            continue;

        if (!cfg->show_hidden && entry->d_name[0] == '.')
            continue;

        char full_path[PATH_MAX];
        path_join(full_path, sizeof(full_path), dir_path, entry->d_name);

        FileEntry fe;
        memset(&fe, 0, sizeof(fe));
        fe.path = xstrdup(full_path);
        fe.name = strrchr(fe.path, '/');
        fe.name = fe.name ? fe.name + 1 : fe.path;
        fe.line_count = -1;
        fe.file_count = -1;
        fe.git_status[0] = '\0';

        struct stat st;
        fe.type = detect_file_type(full_path, &st, &fe.symlink_target);
        fe.mode = st.st_mode;
        fe.mtime = GET_MTIME(st);
        fe.size = st.st_size;

        file_list_add(list, &fe);
    }

    closedir(dir);

    /* Compute expensive data in parallel */
    if (cfg->long_format && list->count > 0) {
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < list->count; i++) {
            FileEntry *fe = &list->entries[i];
            if (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) {
                DirStats stats = get_dir_stats_cached(fe->path);
                fe->size = stats.size;
                fe->file_count = stats.file_count;
            } else if (fe->type == FTYPE_FILE || fe->type == FTYPE_EXEC ||
                       fe->type == FTYPE_SYMLINK || fe->type == FTYPE_SYMLINK_EXEC) {
                int mp = get_image_megapixels(fe->path);
                if (mp >= 0) {
                    fe->line_count = mp;
                    fe->is_image = 1;
                } else {
                    fe->line_count = count_file_lines(fe->path);
                }
            }
        }
    }

    qsort(list->entries, list->count, sizeof(FileEntry), entry_cmp_name);

    if (cfg->sort_by != SORT_NONE && cfg->sort_by != SORT_NAME) {
        sort_file_list(list, cfg);
    } else if (cfg->sort_reverse) {
        reverse_file_list(list);
    }

    return 0;
}

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

static char *append_git_icon(char *buf, size_t *remaining,
                             const char *icon, const char *color, const Config *cfg) {
    int written = snprintf(buf, *remaining, "%s%s%s ", color, icon, RST(cfg));
    *remaining -= written;
    return buf + written;
}

const char *get_git_indicator(GitCache *cache, const char *path,
                              const Icons *icons, const Config *cfg) {
    static __thread char indicator[L_GIT_INDICATOR_SIZE];
    indicator[0] = '\0';

    if (cfg->no_icons) return indicator;

    const char *status = git_cache_get(cache, path);
    if (!status || strcmp(status, "!!") == 0) return indicator;

    char *p = indicator;
    size_t remaining = sizeof(indicator);

    if (strcmp(status, "??") == 0) {
        append_git_icon(p, &remaining, icons->git_untracked, CLR(cfg, COLOR_RED), cfg);
    } else {
        if (status[1] == 'M') {
            p = append_git_icon(p, &remaining, icons->git_modified, CLR(cfg, COLOR_RED), cfg);
        } else if (status[1] == 'D') {
            p = append_git_icon(p, &remaining, icons->git_deleted, CLR(cfg, COLOR_RED), cfg);
        }
        if (status[0] != ' ' && status[0] != '?' && status[0] != '!') {
            const char *staged_icon = (status[0] == 'D') ? icons->git_deleted : icons->git_staged;
            append_git_icon(p, &remaining, staged_icon, CLR(cfg, COLOR_YELLOW), cfg);
        }
    }

    return indicator;
}

/* ============================================================================
 * Tree Building
 * ============================================================================ */

void tree_node_free(TreeNode *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) {
        tree_node_free(&node->children[i]);
    }
    free(node->children);
    file_entry_free(&node->entry);
}

static int should_skip_dir(const char *name, int is_ignored, const Config *cfg) {
    if (cfg->expand_all) return 0;
    if (is_ignored) return 1;
    if (strcmp(name, ".git") == 0) return 1;
    return 0;
}

static void build_tree_children(TreeNode *parent, int depth, Column *cols,
                                 GitCache *git, const Config *cfg, const Icons *icons,
                                 int in_git_repo) {
    if (depth >= cfg->max_depth) return;
    if (access(parent->entry.path, R_OK) != 0) return;

    FileList list;
    file_list_init(&list);

    if (read_directory(parent->entry.path, &list, cfg) != 0) {
        file_list_free(&list);
        return;
    }

    if (list.count == 0) {
        file_list_free(&list);
        return;
    }

    /* Find git repo roots before allocating children array */
    char **git_repos = NULL;
    size_t git_repo_count = 0;
    int *is_git_repo_root = xmalloc(list.count * sizeof(int));
    int *is_submodule = xmalloc(list.count * sizeof(int));

    for (size_t i = 0; i < list.count; i++) {
        is_git_repo_root[i] = 0;
        is_submodule[i] = 0;
        FileEntry *fe = &list.entries[i];
        if ((fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) &&
            strcmp(fe->name, ".git") != 0 &&
            path_is_git_root(fe->path)) {
            is_git_repo_root[i] = 1;
            if (in_git_repo) {
                /* This is a submodule/nested repo - treat as ignored */
                is_submodule[i] = 1;
            } else {
                /* Top-level repo - populate its git status */
                git_repos = xrealloc(git_repos, (git_repo_count + 1) * sizeof(char *));
                git_repos[git_repo_count++] = fe->path;
            }
        }
    }

    /* Populate git repos (excluding submodules) */
    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < git_repo_count; i++) {
        git_populate_repo(git, git_repos[i], cfg->long_format);
    }
    free(git_repos);

    /* Now allocate children after we know the final count */
    parent->children = xmalloc(list.count * sizeof(TreeNode));
    parent->child_count = list.count;

    for (size_t i = 0; i < list.count; i++) {
        TreeNode *child = &parent->children[i];
        memset(child, 0, sizeof(TreeNode));
        child->entry = list.entries[i];
    }

    for (size_t i = 0; i < list.count; i++) {
        TreeNode *child = &parent->children[i];

        GitStatusNode *git_node = git_cache_get_node(git, child->entry.path);
        if (git_node) {
            strncpy(child->entry.git_status, git_node->status, sizeof(child->entry.git_status) - 1);
            child->entry.git_status[sizeof(child->entry.git_status) - 1] = '\0';
            child->entry.diff_added = git_node->lines_added;
            child->entry.diff_removed = git_node->lines_removed;
        }
        const char *git_status = git_node ? git_node->status : NULL;
        child->entry.is_ignored = (git_status && strcmp(git_status, "!!") == 0) ||
                                   strcmp(child->entry.name, ".git") == 0 ||
                                   is_submodule[i];

        if (cfg->long_format && cols) {
            columns_update_widths(cols, &child->entry, icons);
        }

        if ((child->entry.type == FTYPE_DIR || child->entry.type == FTYPE_SYMLINK_DIR) &&
            !should_skip_dir(child->entry.name, child->entry.is_ignored, cfg)) {

            int child_in_git_repo = in_git_repo || is_git_repo_root[i];

            build_tree_children(child, depth + 1, cols, git, cfg, icons, child_in_git_repo);

            /* Set deleted lines for directory (from deleted files directly in it) */
            child->entry.diff_removed = git_deleted_lines_direct(git, child->entry.path);

            if (cfg->long_format && cfg->show_hidden && child->child_count > 0) {
                off_t total_size = 0;
                long total_count = 0;
                for (size_t j = 0; j < child->child_count; j++) {
                    total_size += child->children[j].entry.size;
                    FileType t = child->children[j].entry.type;
                    if (t == FTYPE_FILE || t == FTYPE_EXEC ||
                        t == FTYPE_SYMLINK || t == FTYPE_SYMLINK_EXEC) {
                        total_count++;
                    } else if (child->children[j].entry.file_count >= 0) {
                        total_count += child->children[j].entry.file_count;
                    }
                }
                child->entry.size = total_size;
                child->entry.file_count = total_count;
            }
        }
    }

    free(is_git_repo_root);
    free(is_submodule);
    free(list.entries);
}

TreeNode *build_tree(const char *path, Column *cols,
                     GitCache *git, const Config *cfg, const Icons *icons) {
    char abs_path[PATH_MAX];
    get_abspath(path, abs_path, cfg);

    char git_root[PATH_MAX];
    int in_git_repo = git_find_root(abs_path, git_root, sizeof(git_root));
    if (in_git_repo) {
        git_populate_repo(git, git_root, cfg->long_format);
    }

    struct stat st;
    char *symlink_target = NULL;
    FileType type = detect_file_type(abs_path, &st, &symlink_target);

    TreeNode *root = xmalloc(sizeof(TreeNode));
    memset(root, 0, sizeof(TreeNode));

    root->entry.path = xstrdup(abs_path);
    root->entry.name = strrchr(root->entry.path, '/');
    root->entry.name = root->entry.name ? root->entry.name + 1 : root->entry.path;
    root->entry.type = type;
    root->entry.symlink_target = symlink_target;
    root->entry.mode = st.st_mode;
    root->entry.mtime = GET_MTIME(st);
    root->entry.line_count = -1;
    root->entry.file_count = -1;

    if (cfg->long_format && (type == FTYPE_FILE || type == FTYPE_EXEC ||
                             type == FTYPE_SYMLINK || type == FTYPE_SYMLINK_EXEC)) {
        int mp = get_image_megapixels(abs_path);
        if (mp >= 0) {
            root->entry.line_count = mp;
            root->entry.is_image = 1;
        } else {
            root->entry.line_count = count_file_lines(abs_path);
        }
    }

    root->entry.size = st.st_size;
    if (cfg->long_format && (type == FTYPE_DIR || type == FTYPE_SYMLINK_DIR) && !cfg->show_hidden) {
        DirStats stats = get_dir_stats_cached(abs_path);
        root->entry.size = stats.size;
        root->entry.file_count = stats.file_count;
    }

    const char *git_status = git_cache_get(git, abs_path);
    root->entry.is_ignored = (git_status && strcmp(git_status, "!!") == 0) ||
                              strcmp(root->entry.name, ".git") == 0;

    if (cfg->long_format && cols) {
        columns_update_widths(cols, &root->entry, icons);
    }

    if (type == FTYPE_DIR || type == FTYPE_SYMLINK_DIR) {
        build_tree_children(root, 0, cols, git, cfg, icons, in_git_repo);

        if (cfg->long_format && cfg->show_hidden) {
            off_t total_size = 0;
            long total_count = 0;
            for (size_t i = 0; i < root->child_count; i++) {
                total_size += root->children[i].entry.size;
                FileType t = root->children[i].entry.type;
                if (t == FTYPE_FILE || t == FTYPE_EXEC ||
                    t == FTYPE_SYMLINK || t == FTYPE_SYMLINK_EXEC) {
                    total_count++;
                } else if (root->children[i].entry.file_count >= 0) {
                    total_count += root->children[i].entry.file_count;
                }
            }
            root->entry.size = total_size;
            root->entry.file_count = total_count;
            if (cols) {
                columns_update_widths(cols, &root->entry, icons);
            }
        }
    }

    return root;
}

/* Build a single ancestor node (directory only, no children yet) */
static TreeNode *build_ancestor_node(const char *path, Column *cols,
                                      const Config *cfg, const Icons *icons) {
    struct stat st;
    char *symlink_target = NULL;
    FileType type = detect_file_type(path, &st, &symlink_target);

    TreeNode *node = xmalloc(sizeof(TreeNode));
    memset(node, 0, sizeof(TreeNode));

    node->entry.path = xstrdup(path);
    node->entry.name = strrchr(node->entry.path, '/');
    node->entry.name = node->entry.name ? node->entry.name + 1 : node->entry.path;

    /* Special case for root */
    if (node->entry.path[0] == '/' && node->entry.path[1] == '\0') {
        node->entry.name = "/";
    }

    node->entry.type = type;
    node->entry.symlink_target = symlink_target;
    node->entry.mode = st.st_mode;
    node->entry.mtime = GET_MTIME(st);
    node->entry.size = st.st_size;
    node->entry.line_count = -1;
    node->entry.file_count = -1;

    /* Compute size/file count for long format */
    if (cfg->long_format && (type == FTYPE_DIR || type == FTYPE_SYMLINK_DIR)) {
        DirStats stats = get_dir_stats_cached(path);
        node->entry.size = stats.size;
        node->entry.file_count = stats.file_count;
    }

    if (cfg->long_format && cols) {
        columns_update_widths(cols, &node->entry, icons);
    }

    return node;
}

TreeNode *build_ancestry_tree(const char *path, Column *cols, GitCache *git,
                               const Config *cfg, const Icons *icons) {
    char abs_path[PATH_MAX];

    /* For ancestry, preserve symlinks in path by using $PWD for relative paths */
    if (path[0] == '/') {
        /* Absolute path - use as given, just normalize . and .. */
        path_get_abspath(path, abs_path, "/");
    } else {
        /* Relative path - try $PWD first (preserves symlinks), fall back to cwd */
        const char *pwd = getenv("PWD");
        if (pwd && pwd[0] == '/') {
            path_get_abspath(path, abs_path, pwd);
        } else {
            get_abspath(path, abs_path, cfg);
        }
    }

    /* Determine base path: home if path is under home, otherwise root */
    const char *base = "/";
    size_t base_len = 1;
    int use_home = 0;

    if (cfg->home[0] && strncmp(abs_path, cfg->home, strlen(cfg->home)) == 0) {
        char after = abs_path[strlen(cfg->home)];
        if (after == '\0' || after == '/') {
            base = cfg->home;
            base_len = strlen(cfg->home);
            use_home = 1;
        }
    }

    /* If path equals the base, just build a normal tree */
    if (strcmp(abs_path, base) == 0) {
        return build_tree(path, cols, git, cfg, icons);
    }

    /* Build list of path components from base to target */
    char *components[PATH_MAX / 2];
    int comp_count = 0;

    /* Start after the base */
    const char *p = abs_path + base_len;
    if (*p == '/') p++;

    char path_so_far[PATH_MAX];
    strncpy(path_so_far, base, sizeof(path_so_far) - 1);
    path_so_far[sizeof(path_so_far) - 1] = '\0';

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);

        if (comp_len > 0) {
            /* Build the full path to this component */
            size_t path_len = strlen(path_so_far);
            if (path_so_far[path_len - 1] != '/') {
                strncat(path_so_far, "/", sizeof(path_so_far) - path_len - 1);
            }
            strncat(path_so_far, p, comp_len);
            path_so_far[sizeof(path_so_far) - 1] = '\0';

            components[comp_count++] = xstrdup(path_so_far);
        }

        if (!slash) break;
        p = slash + 1;
    }

    /* Build the root node (home or /) */
    TreeNode *root;
    if (use_home) {
        root = build_ancestor_node(cfg->home, cols, cfg, icons);
    } else {
        root = build_ancestor_node("/", cols, cfg, icons);
    }

    /* Build the chain of ancestors */
    TreeNode *current = root;
    for (int i = 0; i < comp_count; i++) {
        TreeNode *child;

        if (i == comp_count - 1) {
            /* This is the target - build it with full tree */
            child = build_tree(components[i], cols, git, cfg, icons);
            /* The child was allocated by build_tree, we need to use it as-is */
        } else {
            /* This is an ancestor - just a skeleton node */
            child = build_ancestor_node(components[i], cols, cfg, icons);
        }

        /* Allocate the children array for current and add child */
        current->children = xmalloc(sizeof(TreeNode));
        current->child_count = 1;
        current->children[0] = *child;

        /* Free the temporary node structure (but not its contents) */
        free(child);

        current = &current->children[0];
        free(components[i]);
    }

    return root;
}

/* ============================================================================
 * Git Status Flags
 * ============================================================================ */

static int entry_has_git_status(const FileEntry *fe) {
    if (fe->git_status[0] == '\0') return 0;
    if (strcmp(fe->git_status, "!!") == 0) return 0;
    return 1;
}

int compute_git_status_flags(TreeNode *node) {
    int result = entry_has_git_status(&node->entry);
    for (size_t i = 0; i < node->child_count; i++) {
        if (compute_git_status_flags(&node->children[i])) {
            result = 1;
        }
    }
    node->has_git_status = result;
    return result;
}

int compute_grep_flags(TreeNode *node, const char *pattern) {
    int result = (fnmatch(pattern, node->entry.name, FNM_CASEFOLD) == 0);
    for (size_t i = 0; i < node->child_count; i++) {
        if (compute_grep_flags(&node->children[i], pattern)) {
            result = 1;
        }
    }
    node->matches_grep = result;
    return result;
}

/* ============================================================================
 * Tree Printing
 * ============================================================================ */

static void print_prefix(int depth, int *continuation, const Config *cfg) {
    if (cfg->list_mode) return;

    if (!cfg->is_tty) {
        for (int i = 0; i < depth; i++) {
            printf("  ");
        }
        return;
    }

    printf("%s", COLOR_GREY);
    for (int i = 0; i < depth - 1; i++) {
        printf("%s", continuation[i] ? TREE_VERT : TREE_SPACE);
    }
    if (depth > 0) {
        printf("%s", continuation[depth - 1] ? TREE_BRANCH : TREE_LAST);
    }
    printf("%s", COLOR_RESET);
}

static void print_entry(const FileEntry *fe, int depth, int has_visible_children, const PrintContext *ctx) {
    char abs_path[PATH_MAX];
    get_realpath(fe->path, abs_path, ctx->cfg);

    int is_cwd = (strcmp(abs_path, ctx->cfg->cwd) == 0);
    int is_hidden = (fe->name[0] == '.');

    if (ctx->cfg->long_format && ctx->columns) {
        char buf[32];
        for (int i = 0; i < NUM_COLUMNS; i++) {
            ctx->columns[i].format(fe, ctx->icons, buf, sizeof(buf));
            printf("%s%*s%s", CLR(ctx->cfg, COLOR_GREY), ctx->columns[i].width, buf, RST(ctx->cfg));
            if (i == COL_LINES) {
                const char *count_icon = get_count_icon(fe, ctx->icons);
                if (count_icon[0]) {
                    printf(" %s%s%s", CLR(ctx->cfg, COLOR_GREY), count_icon, RST(ctx->cfg));
                } else {
                    printf("  ");
                }
            }
            printf("  ");
        }
        /* Diff columns (only shown when there are diffs) */
        if (ctx->diff_add_width > 0) {
            if (fe->diff_added > 0) {
                printf("%s%*d%s ", CLR(ctx->cfg, COLOR_GREEN),
                       ctx->diff_add_width, fe->diff_added, RST(ctx->cfg));
            } else {
                printf("%s%*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_add_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_del_width > 0) {
            if (fe->diff_removed > 0) {
                printf("%s%-*d%s ", CLR(ctx->cfg, COLOR_RED),
                       ctx->diff_del_width, fe->diff_removed, RST(ctx->cfg));
            } else {
                printf("%s%-*s%s ", CLR(ctx->cfg, COLOR_GREY),
                       ctx->diff_del_width, "-", RST(ctx->cfg));
            }
        }
        if (ctx->diff_add_width > 0 || ctx->diff_del_width > 0) {
            printf(" ");
        }
    }

    print_prefix(depth, ctx->continuation, ctx->cfg);

    int is_readonly = (access(fe->path, W_OK) != 0 && access(fe->path, R_OK) == 0);
    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    if (!ctx->cfg->no_icons && is_readonly && !is_dir) {
        printf("%s%s%s ", CLR(ctx->cfg, COLOR_YELLOW), ctx->icons->readonly, RST(ctx->cfg));
    }

    if (is_dir && !has_visible_children && !ctx->cfg->no_icons) {
        /* Collapsed directory: show full recursive summary */
        GitSummary gs = git_get_dir_summary(ctx->git, abs_path);
        if (gs.modified) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.modified, ctx->icons->git_modified, RST(ctx->cfg));
        }
        if (gs.untracked) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.untracked, ctx->icons->git_untracked, RST(ctx->cfg));
        }
        if (gs.staged) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_YELLOW), gs.staged, ctx->icons->git_staged, RST(ctx->cfg));
        }
        if (gs.deleted) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), gs.deleted, ctx->icons->git_deleted, RST(ctx->cfg));
        }
    } else if (is_dir && has_visible_children && !ctx->cfg->no_icons) {
        /* Expanded directory: show only directly deleted files count */
        int deleted_direct = git_count_deleted_direct(ctx->git, abs_path);
        if (deleted_direct) {
            printf("%s%d %s%s ", CLR(ctx->cfg, COLOR_RED), deleted_direct, ctx->icons->git_deleted, RST(ctx->cfg));
        }
    } else {
        const char *git_ind = get_git_indicator(ctx->git, abs_path, ctx->icons, ctx->cfg);
        printf("%s", git_ind);
    }

    int is_locked = (fe->type == FTYPE_DIR && (fe->size < 0 || is_readonly));
    const char *color = (fe->type == FTYPE_DIR && fe->size < 0) ? CLR(ctx->cfg, COLOR_RED) :
                        get_file_color(fe->type, is_cwd, fe->is_ignored, ctx->cfg);
    const char *style = is_hidden ? CLR(ctx->cfg, STYLE_ITALIC) : "";

    if (!ctx->cfg->no_icons) {
        int is_binary = (fe->file_count < 0 && fe->line_count == -1);
        printf("%s%s%s ", color, get_icon(ctx->icons, fe->type, is_cwd, is_locked, is_binary, fe->name), RST(ctx->cfg));
    }

    if (ctx->cfg->list_mode) {
        char abbrev[PATH_MAX];
        abbreviate_home(abs_path, abbrev, sizeof(abbrev), ctx->cfg);
        printf("%s%s%s%s", color, style, abbrev, RST(ctx->cfg));
    } else {
        printf("%s%s%s%s", color, style, fe->name, RST(ctx->cfg));
    }

    if (is_dir && path_is_git_root(fe->path)) {
        char *branch = git_get_branch(fe->path);
        if (branch) {
            char local_hash[64], remote_hash[64];
            char local_ref[128], remote_ref[128];
            snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
            snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);
            git_read_ref(fe->path, local_ref, local_hash, sizeof(local_hash));
            int has_upstream = git_read_ref(fe->path, remote_ref, remote_hash, sizeof(remote_hash));
            if (has_upstream) {
                int out_of_sync = strcmp(local_hash, remote_hash) != 0;
                const char *cloud_color = out_of_sync ? COLOR_RED : COLOR_GREY;
                printf(" %s%s%s%s %s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), branch, RST(ctx->cfg), CLR(ctx->cfg, cloud_color), ctx->icons->git_upstream, RST(ctx->cfg));
            } else {
                printf(" %s%s%s%s", CLR(ctx->cfg, COLOR_GREY), CLR(ctx->cfg, STYLE_ITALIC), branch, RST(ctx->cfg));
            }
            free(branch);
        }
    }

    if (fe->symlink_target) {
        char abbrev[PATH_MAX];
        abbreviate_home(fe->symlink_target, abbrev, sizeof(abbrev), ctx->cfg);
        const char *target_base = strrchr(fe->symlink_target, '/');
        target_base = target_base ? target_base + 1 : fe->symlink_target;
        const char *target_style = (target_base[0] == '.') ? CLR(ctx->cfg, STYLE_ITALIC) : "";
        printf(" %s %s%s%s%s", ctx->icons->symlink, color, target_style, abbrev, RST(ctx->cfg));
    }

    printf("\n");
}

static void print_tree_children(const TreeNode *parent, int depth, PrintContext *ctx);

void print_tree_node(const TreeNode *node, int depth, PrintContext *ctx) {
    int filtering = is_filtering_active(ctx->cfg);

    if (ctx->cfg->list_mode && node->entry.type == FTYPE_DIR) {
        size_t visible_count = 0;
        size_t *visible_indices = NULL;

        if (filtering) {
            visible_indices = xmalloc(node->child_count * sizeof(size_t));
            for (size_t i = 0; i < node->child_count; i++) {
                if (node_is_visible(&node->children[i], ctx->cfg)) {
                    visible_indices[visible_count++] = i;
                }
            }
        } else {
            visible_count = node->child_count;
        }

        for (size_t vi = 0; vi < visible_count; vi++) {
            size_t i = filtering ? visible_indices[vi] : vi;
            const TreeNode *child = &node->children[i];
            int is_last = (vi == visible_count - 1);
            if (depth > 0) ctx->continuation[depth - 1] = !is_last;

            int has_visible_children = 0;
            if (child->child_count > 0) {
                if (filtering) {
                    for (size_t j = 0; j < child->child_count; j++) {
                        if (node_is_visible(&child->children[j], ctx->cfg)) {
                            has_visible_children = 1;
                            break;
                        }
                    }
                } else {
                    has_visible_children = 1;
                }
            }

            print_entry(&child->entry, depth, has_visible_children, ctx);

            if (child->child_count > 0) {
                print_tree_children(child, depth, ctx);
            }
        }
        free(visible_indices);
        return;
    }

    int has_visible_children = 0;
    if (node->child_count > 0) {
        if (filtering) {
            for (size_t i = 0; i < node->child_count; i++) {
                if (node_is_visible(&node->children[i], ctx->cfg)) {
                    has_visible_children = 1;
                    break;
                }
            }
        } else {
            has_visible_children = 1;
        }
    }

    print_entry(&node->entry, depth, has_visible_children, ctx);

    if (node->child_count > 0) {
        print_tree_children(node, depth, ctx);
    }
}

static void print_tree_children(const TreeNode *parent, int depth, PrintContext *ctx) {
    int filtering = is_filtering_active(ctx->cfg);
    size_t visible_count = 0;
    size_t *visible_indices = NULL;

    if (filtering) {
        visible_indices = xmalloc(parent->child_count * sizeof(size_t));
        for (size_t i = 0; i < parent->child_count; i++) {
            if (node_is_visible(&parent->children[i], ctx->cfg)) {
                visible_indices[visible_count++] = i;
            }
        }
    } else {
        visible_count = parent->child_count;
    }

    for (size_t vi = 0; vi < visible_count; vi++) {
        size_t i = filtering ? visible_indices[vi] : vi;
        const TreeNode *child = &parent->children[i];
        int is_last = (vi == visible_count - 1);

        ctx->continuation[depth] = !is_last;

        int has_visible_children = 0;
        if (child->child_count > 0) {
            if (filtering) {
                for (size_t j = 0; j < child->child_count; j++) {
                    if (node_is_visible(&child->children[j], ctx->cfg)) {
                        has_visible_children = 1;
                        break;
                    }
                }
            } else {
                has_visible_children = 1;
            }
        }

        print_entry(&child->entry, depth + 1, has_visible_children, ctx);

        if (child->child_count > 0) {
            print_tree_children(child, depth + 1, ctx);
        }
    }

    free(visible_indices);
}
