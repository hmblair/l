/*
 * fileinfo.h - File type detection, line counting, and image parsing
 */

#ifndef L_FILEINFO_H
#define L_FILEINFO_H

#include "common.h"
#include "icons.h"  /* For FileType enum */

/* ============================================================================
 * File Type Detection
 * ============================================================================ */

FileType detect_file_type(const char *path, struct stat *st, char **symlink_target);
const char *get_file_color(FileType type, int is_cwd, int is_ignored, int is_tty, int color_all);

/* ============================================================================
 * Line Counting and Image Parsing
 * ============================================================================ */

int count_file_lines(const char *path);
int get_image_megapixels(const char *path);

#endif /* L_FILEINFO_H */
