/*
 * config.h - Config file discovery and loading
 */

#ifndef L_CONFIG_H
#define L_CONFIG_H

#include "common.h"
#include "icons.h"
#include "tree.h"

/* ============================================================================
 * Run Configuration
 *
 * Split by concern: Request is what to list (paths aside) and how to filter
 * and order it; DisplayOpts is how to draw it; ComputeOpts (tree.h) is what
 * metadata to gather; Env is where we are. Code that only renders takes no
 * dependency on filtering fields and vice versa.
 * ============================================================================ */

typedef struct {
    int max_depth;
    int show_hidden;
    int expand_all;
    int git_only;            /* -m: show only git-changed entries, rooted at repo */
    int hide_gitignored;     /* -g: hide entries that are gitignored */
    int show_ancestry;
    int ancestry_explicit;   /* -p was given explicitly (vs. implied by -m),
                              * so anchor the ancestry at ~ (or /), not the
                              * enclosing repo root. */
    int summary_mode;
    int list_mode;
    int interactive;
    int dir_only;
    SortMode sort_by;
    int sort_reverse;
    const char *grep_pattern;
    const char *git_base;    /* -b: ref to report git changes against instead
                              * of HEAD (NULL = HEAD) */
    off_t min_size;          /* Minimum size filter (0 = disabled) */
} Request;

typedef struct {
    int long_format;
    int long_format_explicit;
    int no_icons;
    int color_all;
    int is_tty;
    char column_separator[L_MAX_SEPARATOR_LEN];  /* glyph between long-mode
                                                  * columns; blank = spaces */
} DisplayOpts;

typedef struct {
    char cwd[PATH_MAX];
    char home[PATH_MAX];
    char config_dir[PATH_MAX];   /* directory holding config.toml */
} Env;

typedef struct {
    Request req;
    DisplayOpts disp;
    ComputeOpts compute;
    Env env;
} Config;

/* Find the directory holding config.toml: next to the binary (development),
 * ~/.config/l (installed), /usr/local/share/l (system-wide), else ".". */
void resolve_source_dir(const char *argv0, char *src_dir, size_t len);

/* Load everything l reads from config.toml in a single pass over the file:
 * [icons]/[display] icon glyphs, [extensions] icon overrides, [filetypes],
 * [shebangs], and the [display] column_separator (written into separator only
 * when the key is present, so callers seed it with a default beforehand).
 * The [opaque] section is handled separately by opaque_dirs_load (common.c)
 * because the cache daemon shares it. */
void config_load_all(const char *config_dir, Icons *icons, FileTypes *ft,
                     Shebangs *sb, char *separator, size_t separator_len);

#endif /* L_CONFIG_H */
