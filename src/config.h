/*
 * config.h - Config file discovery and loading
 */

#ifndef L_CONFIG_H
#define L_CONFIG_H

#include "common.h"
#include "icons.h"

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
