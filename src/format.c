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

    if (unit_idx == 0) {
        snprintf(buf, len, "%lld%s", (long long)bytes, units[0]);
    } else if (size < 10) {
        snprintf(buf, len, "%.1f%s", size, units[unit_idx]);
    } else {
        snprintf(buf, len, "%.0f%s", size, units[unit_idx]);
    }
}

void format_count(long count, char *buf, size_t len) {
    if (count >= 1000000) {
        double m = count / 1000000.0;
        snprintf(buf, len, m < 10 ? "%.1fM" : "%.0fM", m);
    } else if (count >= 1000) {
        double k = count / 1000.0;
        snprintf(buf, len, k < 10 ? "%.1fK" : "%.0fK", k);
    } else {
        snprintf(buf, len, "%ld", count);
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
