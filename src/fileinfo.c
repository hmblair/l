/*
 * fileinfo.c - File type detection, line counting, and image parsing
 */

#include "fileinfo.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <strings.h>

/* ============================================================================
 * Symlink Resolution
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
        size_t dir_len = strlen(dir);
        size_t target_len = strlen(target);
        if (dir_len + target_len < PATH_MAX) {
            memcpy(abs_target, dir, dir_len);
            memcpy(abs_target + dir_len, target, target_len + 1);
            return xstrdup(abs_target);
        }
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

/* ============================================================================
 * File Type Detection
 * ============================================================================ */

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

const char *get_file_color(FileType type, int is_cwd, int is_ignored, int is_tty, int color_all) {
    if (!is_tty) return "";

    if (is_cwd) return COLOR_YELLOW;
    if (is_ignored && !color_all) return COLOR_GREY;

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
 * Binary Detection
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

/* ============================================================================
 * Image Dimension Parsing
 * ============================================================================ */

/* Get image dimensions from file header. Returns megapixels * 10, or -1 on failure. */
int get_image_megapixels(const char *path) {
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
    /* HEIC/HEIF: ftyp box with heic/mif1 brand, dimensions in ispe box */
    else if (n >= 12 && memcmp(header + 4, "ftyp", 4) == 0 &&
             (memcmp(header + 8, "heic", 4) == 0 ||
              memcmp(header + 8, "mif1", 4) == 0 ||
              memcmp(header + 8, "msf1", 4) == 0 ||
              memcmp(header + 8, "heix", 4) == 0)) {
        /* Navigate box structure to find ispe box */
            fseek(f, 0, SEEK_SET);
            unsigned char box[8];
            while (fread(box, 1, 8, f) == 8) {
                uint32_t size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                if (size < 8) break;

                /* meta box contains iprp which contains ipco which contains ispe */
                if (memcmp(box + 4, "meta", 4) == 0) {
                    /* meta is a full box, skip 4-byte version/flags */
                    long meta_end = ftell(f) - 8 + size;
                    fseek(f, 4, SEEK_CUR);

                    /* Search within meta for iprp */
                    while (ftell(f) < meta_end && fread(box, 1, 8, f) == 8) {
                        uint32_t inner_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                        if (inner_size < 8) break;

                        if (memcmp(box + 4, "iprp", 4) == 0) {
                            long iprp_end = ftell(f) - 8 + inner_size;

                            /* Search within iprp for ipco */
                            while (ftell(f) < iprp_end && fread(box, 1, 8, f) == 8) {
                                uint32_t ipco_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                                if (ipco_size < 8) break;

                                if (memcmp(box + 4, "ipco", 4) == 0) {
                                    long ipco_end = ftell(f) - 8 + ipco_size;

                                    /* Search within ipco for ispe */
                                    while (ftell(f) < ipco_end && fread(box, 1, 8, f) == 8) {
                                        uint32_t ispe_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                                        if (ispe_size < 8) break;

                                        if (memcmp(box + 4, "ispe", 4) == 0) {
                                            /* ispe: 4-byte version/flags, 4-byte width, 4-byte height */
                                            unsigned char ispe_data[12];
                                            if (fread(ispe_data, 1, 12, f) == 12) {
                                                width = (ispe_data[4] << 24) | (ispe_data[5] << 16) |
                                                        (ispe_data[6] << 8) | ispe_data[7];
                                                height = (ispe_data[8] << 24) | (ispe_data[9] << 16) |
                                                         (ispe_data[10] << 8) | ispe_data[11];
                                            }
                                            break;
                                        }
                                        fseek(f, ispe_size - 8, SEEK_CUR);
                                    }
                                    break;
                                }
                                fseek(f, ipco_size - 8, SEEK_CUR);
                            }
                            break;
                        }
                        fseek(f, inner_size - 8, SEEK_CUR);
                    }
                    break;
                }
                fseek(f, size - 8, SEEK_CUR);
            }
    }
    /* TIFF-based RAW formats: CR2, NEF, ARW, DNG, ORF, RW2, PEF, SRW, etc.
     * TIFF header: "II" (little-endian) or "MM" (big-endian) + magic 42 */
    else if ((header[0] == 'I' && header[1] == 'I' && header[2] == 0x2A && header[3] == 0x00) ||
             (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 && header[3] == 0x2A)) {
        int little_endian = (header[0] == 'I');

        #define TIFF_READ16(buf, off) (little_endian ? \
            ((buf)[off] | ((buf)[(off)+1] << 8)) : \
            (((buf)[off] << 8) | (buf)[(off)+1]))
        #define TIFF_READ32(buf, off) (little_endian ? \
            ((buf)[off] | ((buf)[(off)+1] << 8) | ((buf)[(off)+2] << 16) | ((buf)[(off)+3] << 24)) : \
            (((buf)[off] << 24) | ((buf)[(off)+1] << 16) | ((buf)[(off)+2] << 8) | (buf)[(off)+3]))

        uint32_t ifd_offsets[32];
        int ifd_count = 0;
        ifd_offsets[ifd_count++] = TIFF_READ32(header, 4);

        /* Traverse IFDs to find largest dimensions (RAW files have multiple IFDs) */
        while (ifd_count > 0 && ifd_count < 32) {
            uint32_t ifd_offset = ifd_offsets[--ifd_count];
            if (ifd_offset == 0 || ifd_offset > 100000000) continue;

            fseek(f, ifd_offset, SEEK_SET);
            unsigned char ifd_header[2];
            if (fread(ifd_header, 1, 2, f) != 2) continue;

            int entry_count = TIFF_READ16(ifd_header, 0);
            if (entry_count <= 0 || entry_count > 1000) continue;

            int cur_width = 0, cur_height = 0;
            long entry_pos = ftell(f);

            for (int i = 0; i < entry_count; i++) {
                fseek(f, entry_pos + i * 12, SEEK_SET);
                unsigned char entry[12];
                if (fread(entry, 1, 12, f) != 12) break;

                uint16_t tag = TIFF_READ16(entry, 0);
                uint16_t type = TIFF_READ16(entry, 2);
                uint32_t count = TIFF_READ32(entry, 4);
                uint32_t value_offset = TIFF_READ32(entry, 8);

                /* For SHORT (type 3), value is at different position based on endianness */
                uint32_t value = value_offset;
                if (type == 3 && count == 1) {
                    value = little_endian ? TIFF_READ16(entry, 8) : (value_offset >> 16);
                }

                if (tag == 0x0100) cur_width = value;       /* ImageWidth */
                else if (tag == 0x0101) cur_height = value; /* ImageLength */
                else if (tag == 0x014A && ifd_count < 30) { /* SubIFDs */
                    if (count == 1) {
                        ifd_offsets[ifd_count++] = value_offset;
                    } else {
                        /* Multiple SubIFD offsets stored at value_offset */
                        long saved = ftell(f);
                        fseek(f, value_offset, SEEK_SET);
                        for (uint32_t j = 0; j < count && ifd_count < 30; j++) {
                            unsigned char off_buf[4];
                            if (fread(off_buf, 1, 4, f) != 4) break;
                            ifd_offsets[ifd_count++] = TIFF_READ32(off_buf, 0);
                        }
                        fseek(f, saved, SEEK_SET);
                    }
                }
                else if (tag == 0x8769 && ifd_count < 30) { /* EXIF IFD */
                    ifd_offsets[ifd_count++] = value_offset;
                }
            }

            /* Keep largest dimensions found */
            if (cur_width > width) width = cur_width;
            if (cur_height > height) height = cur_height;

            /* Read next IFD offset */
            fseek(f, entry_pos + entry_count * 12, SEEK_SET);
            unsigned char next_ifd[4];
            if (fread(next_ifd, 1, 4, f) == 4) {
                uint32_t next = TIFF_READ32(next_ifd, 0);
                if (next != 0 && next < 100000000 && ifd_count < 30) {
                    ifd_offsets[ifd_count++] = next;
                }
            }
        }
        #undef TIFF_READ16
        #undef TIFF_READ32
    }
    /* CR3 (Canon RAW v3): ISOBMFF container with 'crx ' brand
     * Dimensions are in tkhd (track header) boxes inside moov > trak */
    else if (n >= 12 && memcmp(header + 4, "ftyp", 4) == 0 &&
             memcmp(header + 8, "crx ", 4) == 0) {
        fseek(f, 0, SEEK_SET);
        unsigned char box[8];
        while (fread(box, 1, 8, f) == 8) {
            uint32_t size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
            if (size < 8) break;

            if (memcmp(box + 4, "moov", 4) == 0) {
                long moov_end = ftell(f) - 8 + size;
                while (ftell(f) < moov_end && fread(box, 1, 8, f) == 8) {
                    uint32_t trak_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                    if (trak_size < 8) break;

                    if (memcmp(box + 4, "trak", 4) == 0) {
                        long trak_end = ftell(f) - 8 + trak_size;
                        /* Search for tkhd inside trak */
                        while (ftell(f) < trak_end && fread(box, 1, 8, f) == 8) {
                            uint32_t inner_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                            if (inner_size < 8) break;

                            if (memcmp(box + 4, "tkhd", 4) == 0) {
                                /* tkhd: version(1) + flags(3) + times... + width(4) + height(4) at end
                                 * Width/height are 16.16 fixed point */
                                unsigned char tkhd[92];
                                size_t tkhd_size = inner_size - 8;
                                if (tkhd_size > sizeof(tkhd)) tkhd_size = sizeof(tkhd);
                                if (fread(tkhd, 1, tkhd_size, f) >= 84) {
                                    int version = tkhd[0];
                                    size_t off = (version == 1) ? 84 : 76;
                                    if (off + 8 <= tkhd_size) {
                                        uint32_t w = (tkhd[off] << 24) | (tkhd[off+1] << 16) |
                                                     (tkhd[off+2] << 8) | tkhd[off+3];
                                        uint32_t h = (tkhd[off+4] << 24) | (tkhd[off+5] << 16) |
                                                     (tkhd[off+6] << 8) | tkhd[off+7];
                                        w >>= 16; h >>= 16; /* 16.16 fixed point */
                                        if ((int)w > width) width = w;
                                        if ((int)h > height) height = h;
                                    }
                                }
                                break;
                            }
                            fseek(f, inner_size - 8, SEEK_CUR);
                        }
                        fseek(f, trak_end, SEEK_SET);
                    } else {
                        fseek(f, trak_size - 8, SEEK_CUR);
                    }
                }
                break;
            }
            fseek(f, size - 8, SEEK_CUR);
        }
    }

    fclose(f);

    if (width <= 0 || height <= 0) return -1;

    /* Return megapixels * 10 for one decimal place precision */
    double mp = (double)width * height / 1000000.0;
    return (int)(mp * 10 + 0.5);
}

/* ============================================================================
 * Line Counting
 * ============================================================================ */

int count_file_lines(const char *path) {
    if (has_binary_extension(path)) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return st.st_size == 0 ? 0 : -1;
    }

    /* For very large files, use mmap for better performance */
    size_t size = (size_t)st.st_size;
    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) return -1;

    /* Hint to OS about sequential access */
    madvise(data, size, MADV_SEQUENTIAL);

    /* Check for binary content at start */
    size_t check_len = size < L_BINARY_CHECK_SIZE ? size : L_BINARY_CHECK_SIZE;
    if (memchr(data, '\0', check_len) != NULL) {
        munmap(data, size);
        return -1;
    }

    /* Count newlines using memchr (typically SIMD-optimized by libc) */
    long count = 0;
    const char *p = data;
    const char *end = data + size;

    while ((p = memchr(p, '\n', end - p)) != NULL) {
        count++;
        p++;
    }

    munmap(data, size);

    /* Clamp to INT_MAX for return value */
    return count > INT_MAX ? INT_MAX : (int)count;
}
