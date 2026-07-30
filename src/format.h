/*
 * format.h - Value formatting: sizes, counts, times, and the single
 * interpretation point for the polymorphic content quantity (a FileEntry's
 * line_count means lines, megapixels*10, seconds, or pages depending on
 * content_type; file_count covers directories).
 */

#ifndef L_FORMAT_H
#define L_FORMAT_H

#include "common.h"
#include "icons.h"
#include <time.h>

struct FileEntry;

/* Human-readable byte size ("-", "512B", "1.5K", "23M") */
void format_size(off_t bytes, char *buf, size_t len);

/* Compact count ("999", "1.5K", "23M") */
void format_count(long count, char *buf, size_t len);

/* Relative time ("now", "5m ago", "3d ago", "Jan 02") */
void format_relative_time(time_t mtime, char *buf, size_t len);

/* Duration in seconds as M:SS or H:MM:SS */
void format_duration(int seconds, char *buf, size_t len);

/* The one place that turns content_type x line_count/file_count into the
 * value drawn in the lines column: directory file counts, text line counts,
 * image megapixels, audio/video duration, PDF pages, or "-". */
void format_content_quantity(const struct FileEntry *fe, char *buf, size_t len);

/* The matching icon for the quantity drawn by format_content_quantity
 * (files/lines/pixels/duration/pages), or "" if there is none. */
const char *content_quantity_icon(const struct FileEntry *fe, const Icons *icons);

#endif /* L_FORMAT_H */
