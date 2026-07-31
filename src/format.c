/*
 * format.c - Value formatting implementations
 */

#include "format.h"
#include "tree.h"

void format_size(off_t bytes, char *buf, size_t len) {
    if (bytes < 0) {
        snprintf(buf, len, "-");
        return;
    }
    const char *units[] = {"B", "K", "M", "G", "T", "P"};
    int unit_idx = 0;
    double size = (double)bytes;

    while (size >= 1024 && unit_idx < 5) {
        size /= 1024;
        unit_idx++;
    }

    /* Never print four digits: 1000-1023 rolls up to 1.0 of the next unit
     * (still a 1024 divisor, just rounded across the gap). Thresholds sit at
     * the display-rounding boundaries so %.0f/%.1f can't recreate "1000" or
     * "10.0". */
    if (size >= 999.5 && unit_idx < 5) {
        size /= 1024;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, len, "%lld%s", (long long)bytes, units[0]);
    } else if (size < 9.95) {
        snprintf(buf, len, "%.1f%s", size, units[unit_idx]);
    } else {
        snprintf(buf, len, "%.0f%s", size, units[unit_idx]);
    }
}

void format_count(long count, char *buf, size_t len) {
    const char *units[] = {"", "K", "M", "G"};
    int unit_idx = 0;
    double n = (double)count;

    while (n >= 1000 && unit_idx < 3) {
        n /= 1000;
        unit_idx++;
    }

    /* Same rule as format_size: never print four digits. Thresholds sit at
     * the display-rounding boundaries so %.0f/%.1f can't recreate "1000" or
     * "10.0". */
    if (n >= 999.5 && unit_idx < 3) {
        n /= 1000;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, len, "%ld", count);
    } else if (n < 9.95) {
        snprintf(buf, len, "%.1f%s", n, units[unit_idx]);
    } else {
        snprintf(buf, len, "%.0f%s", n, units[unit_idx]);
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

void format_duration(int seconds, char *buf, size_t len) {
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (hours > 0) {
        snprintf(buf, len, "%d:%02d:%02d", hours, mins, s);
    } else {
        snprintf(buf, len, "%d:%02d", mins, s);
    }
}

void format_content_quantity(const struct FileEntry *fe, char *buf, size_t len) {
    if (fe->file_count >= 0) {
        format_count(fe->file_count, buf, len);
        return;
    }
    if (fe->line_count < 0) {
        snprintf(buf, len, "-");
        return;
    }
    switch (fe->content_type) {
        case CONTENT_IMAGE: {
            /* line_count holds megapixels * 10 */
            double mp = fe->line_count / 10.0;
            snprintf(buf, len, mp >= 10.0 ? "%.0fM" : "%.1fM", mp);
            return;
        }
        case CONTENT_AUDIO:
            /* line_count holds duration in seconds */
            format_duration(fe->line_count, buf, len);
            return;
        case CONTENT_PDF:
            /* line_count holds page count */
            snprintf(buf, len, "%d", fe->line_count);
            return;
        default:
            format_count(fe->line_count, buf, len);
            return;
    }
}

const char *content_quantity_icon(const struct FileEntry *fe, const Icons *icons) {
    if (fe->file_count >= 0) {
        return icons->count_files;
    } else if (fe->content_type == CONTENT_IMAGE && fe->line_count >= 0) {
        return icons->count_pixels;
    } else if (fe->content_type == CONTENT_AUDIO && fe->line_count >= 0) {
        return icons->count_duration;
    } else if (fe->content_type == CONTENT_PDF && fe->line_count >= 0) {
        return icons->count_pages;
    } else if (fe->line_count >= 0) {
        return icons->count_lines;
    }
    return "";
}
