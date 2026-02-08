/*
 * fileinfo.c - File type detection, line counting, and image parsing
 */

#include "fileinfo.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <strings.h>
#include <zlib.h>

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
        case FTYPE_SYMLINK_DIR:    return COLOR_BLUE;
        case FTYPE_SYMLINK_EXEC:   return COLOR_GREEN;
        case FTYPE_SYMLINK_DEVICE: return COLOR_YELLOW;
        case FTYPE_SYMLINK_SOCKET: return COLOR_MAGENTA;
        case FTYPE_SYMLINK_FIFO:   return COLOR_MAGENTA;
        case FTYPE_SYMLINK_BROKEN: return COLOR_WHITE;
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
                    value = little_endian ? (uint32_t)TIFF_READ16(entry, 8) : (value_offset >> 16);
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
 * Audio Duration Parsing
 * ============================================================================ */

/* Check if file has an audio/video extension that uses ISOBMFF container */
static int has_isobmff_audio_extension(const char *path) {
    const char *dot = strrchr(path, '/');
    dot = dot ? strrchr(dot, '.') : strrchr(path, '.');
    if (!dot) return 0;
    dot++;

    static const char *exts[] = {
        "m4b", "m4a", "mp4", "m4v", "mov", "3gp", "3g2",
        "aac",  /* AAC in ADTS container won't work, but .aac could be ISOBMFF */
        NULL
    };
    for (const char **e = exts; *e; e++) {
        if (strcasecmp(dot, *e) == 0) return 1;
    }
    return 0;
}

/* Forward declaration for Matroska parser */
static int get_matroska_duration(const char *path);

/* Check if file has a WAV extension */
static int has_wav_extension(const char *path) {
    const char *dot = strrchr(path, '/');
    dot = dot ? strrchr(dot, '.') : strrchr(path, '.');
    if (!dot) return 0;
    dot++;
    return strcasecmp(dot, "wav") == 0;
}

/* Get duration from WAV (RIFF) file. Returns duration in seconds, or -1. */
static int get_wav_duration(const char *path) {
    if (!has_wav_extension(path)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Read RIFF header: "RIFF" + size(4) + "WAVE" = 12 bytes */
    unsigned char header[12];
    if (fread(header, 1, 12, f) < 12 ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return -1;
    }

    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    int found_fmt = 0, found_data = 0;

    /* Walk chunks looking for "fmt " and "data" */
    unsigned char chunk_hdr[8];
    while (fread(chunk_hdr, 1, 8, f) == 8) {
        uint32_t chunk_size = chunk_hdr[4] | (chunk_hdr[5] << 8) |
                              (chunk_hdr[6] << 16) | (chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16) break;
            unsigned char fmt[16];
            if (fread(fmt, 1, 16, f) < 16) break;
            /* byte_rate is at offset 8 in fmt chunk (little-endian) */
            byte_rate = fmt[8] | (fmt[9] << 8) | (fmt[10] << 16) | (fmt[11] << 24);
            found_fmt = 1;
            /* Skip remainder of fmt chunk */
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = 1;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }

        /* Chunks are word-aligned: skip padding byte if chunk_size is odd */
        if (chunk_size & 1) fseek(f, 1, SEEK_CUR);
    }

    fclose(f);

    if (!found_fmt || !found_data || byte_rate == 0) return -1;
    return (int)(data_size / byte_rate);
}

/* Get audio/video duration from ISOBMFF container (M4B, M4A, MP4, MOV, etc.)
 * Returns duration in seconds, or -1 on failure. */
int get_audio_duration(const char *path) {
    /* Try WAV (RIFF) */
    int dur = get_wav_duration(path);
    if (dur >= 0) return dur;

    /* Try Matroska (MKV, WebM) */
    dur = get_matroska_duration(path);
    if (dur >= 0) return dur;

    if (!has_isobmff_audio_extension(path)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char header[12];
    if (fread(header, 1, 12, f) < 12) {
        fclose(f);
        return -1;
    }

    /* Verify ftyp box */
    if (memcmp(header + 4, "ftyp", 4) != 0) {
        fclose(f);
        return -1;
    }

    /* Skip past ftyp box */
    uint32_t ftyp_size = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    if (ftyp_size < 8) {
        fclose(f);
        return -1;
    }
    fseek(f, ftyp_size, SEEK_SET);

    /* Search for moov box */
    unsigned char box[8];
    while (fread(box, 1, 8, f) == 8) {
        uint32_t size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
        if (size < 8) break;

        if (memcmp(box + 4, "moov", 4) == 0) {
            long moov_end = ftell(f) - 8 + size;

            /* Search within moov for mvhd */
            while (ftell(f) < moov_end && fread(box, 1, 8, f) == 8) {
                uint32_t inner_size = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
                if (inner_size < 8) break;

                if (memcmp(box + 4, "mvhd", 4) == 0) {
                    /* mvhd box found - read version to determine layout */
                    unsigned char mvhd[28];
                    size_t mvhd_read = fread(mvhd, 1, 28, f);
                    if (mvhd_read < 20) break;

                    int version = mvhd[0];
                    uint32_t timescale;
                    uint64_t duration;

                    if (version == 0) {
                        /* Version 0: 4-byte fields
                         * [0] version, [1-3] flags
                         * [4-7] creation_time, [8-11] modification_time
                         * [12-15] timescale, [16-19] duration */
                        timescale = (mvhd[12] << 24) | (mvhd[13] << 16) | (mvhd[14] << 8) | mvhd[15];
                        duration = (mvhd[16] << 24) | (mvhd[17] << 16) | (mvhd[18] << 8) | mvhd[19];
                    } else {
                        /* Version 1: 8-byte time fields
                         * [0] version, [1-3] flags
                         * [4-11] creation_time, [12-19] modification_time
                         * [20-23] timescale, [24-31] duration */
                        if (mvhd_read < 28) break;
                        timescale = (mvhd[20] << 24) | (mvhd[21] << 16) | (mvhd[22] << 8) | mvhd[23];
                        duration = ((uint64_t)mvhd[24] << 56) | ((uint64_t)mvhd[25] << 48) |
                                   ((uint64_t)mvhd[26] << 40) | ((uint64_t)mvhd[27] << 32);
                        /* Need to read 4 more bytes for full duration */
                        unsigned char dur_rest[4];
                        if (fread(dur_rest, 1, 4, f) == 4) {
                            duration |= (dur_rest[0] << 24) | (dur_rest[1] << 16) |
                                        (dur_rest[2] << 8) | dur_rest[3];
                        }
                    }

                    fclose(f);
                    if (timescale == 0) return -1;
                    return (int)(duration / timescale);
                }
                fseek(f, inner_size - 8, SEEK_CUR);
            }
            break;
        }
        fseek(f, size - 8, SEEK_CUR);
    }

    fclose(f);
    return -1;
}

/* Check if file has a Matroska/WebM extension */
static int has_matroska_extension(const char *path) {
    const char *dot = strrchr(path, '/');
    dot = dot ? strrchr(dot, '.') : strrchr(path, '.');
    if (!dot) return 0;
    dot++;

    static const char *exts[] = { "mkv", "webm", "mka", NULL };
    for (const char **e = exts; *e; e++) {
        if (strcasecmp(dot, *e) == 0) return 1;
    }
    return 0;
}

/* Read EBML variable-length integer, return number of bytes read (0 on error) */
static int read_ebml_vint(FILE *f, uint64_t *value, int strip_marker) {
    int c = fgetc(f);
    if (c == EOF) return 0;

    /* Find leading 1 bit to determine length */
    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 8 && !(c & mask)) {
        mask >>= 1;
        len++;
    }
    if (len > 8) return 0;

    *value = strip_marker ? (c & (mask - 1)) : c;
    for (int i = 1; i < len; i++) {
        c = fgetc(f);
        if (c == EOF) return 0;
        *value = (*value << 8) | c;
    }
    return len;
}

/* Read EBML element ID */
static int read_ebml_id(FILE *f, uint64_t *id) {
    return read_ebml_vint(f, id, 0);
}

/* Read EBML element size */
static int read_ebml_size(FILE *f, uint64_t *size) {
    return read_ebml_vint(f, size, 1);
}

/* Get duration from Matroska/WebM file. Returns duration in seconds, or -1. */
static int get_matroska_duration(const char *path) {
    if (!has_matroska_extension(path)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* Verify EBML header (ID 0x1A45DFA3) */
    unsigned char header[4];
    if (fread(header, 1, 4, f) < 4 ||
        header[0] != 0x1A || header[1] != 0x45 ||
        header[2] != 0xDF || header[3] != 0xA3) {
        fclose(f);
        return -1;
    }

    /* Skip EBML header content */
    uint64_t header_size;
    if (!read_ebml_size(f, &header_size)) { fclose(f); return -1; }
    fseek(f, header_size, SEEK_CUR);

    /* Look for Segment element (ID 0x18538067) */
    uint64_t id, size;
    if (!read_ebml_id(f, &id) || id != 0x18538067) { fclose(f); return -1; }
    if (!read_ebml_size(f, &size)) { fclose(f); return -1; }

    long segment_end = ftell(f) + (long)size;
    double duration = -1;
    uint64_t timecode_scale = 1000000; /* Default: 1ms */

    /* Search for Info element within Segment */
    while (ftell(f) < segment_end) {
        long elem_start = ftell(f);
        if (!read_ebml_id(f, &id)) break;
        if (!read_ebml_size(f, &size)) break;

        if (id == 0x1549A966) { /* Info element */
            long info_end = ftell(f) + (long)size;

            /* Search within Info for Duration and TimecodeScale */
            while (ftell(f) < info_end) {
                if (!read_ebml_id(f, &id)) break;
                if (!read_ebml_size(f, &size)) break;

                if (id == 0x2AD7B1 && size <= 8) { /* TimecodeScale */
                    timecode_scale = 0;
                    for (uint64_t i = 0; i < size; i++) {
                        int c = fgetc(f);
                        if (c == EOF) break;
                        timecode_scale = (timecode_scale << 8) | c;
                    }
                } else if (id == 0x4489 && size == 8) { /* Duration (float) */
                    unsigned char buf[8];
                    if (fread(buf, 1, 8, f) == 8) {
                        uint64_t bits = 0;
                        for (int i = 0; i < 8; i++)
                            bits = (bits << 8) | buf[i];
                        memcpy(&duration, &bits, 8);
                    }
                } else if (id == 0x4489 && size == 4) { /* Duration (float32) */
                    unsigned char buf[4];
                    if (fread(buf, 1, 4, f) == 4) {
                        uint32_t bits = (buf[0] << 24) | (buf[1] << 16) |
                                        (buf[2] << 8) | buf[3];
                        float f32;
                        memcpy(&f32, &bits, 4);
                        duration = f32;
                    }
                } else {
                    fseek(f, size, SEEK_CUR);
                }
            }
            break; /* Found Info, done */
        } else {
            /* Skip unknown elements, but limit skip for "unknown size" */
            if (size > 0x00FFFFFFFFFFFFFF) {
                /* Unknown size marker - scan forward */
                break;
            }
            fseek(f, size, SEEK_CUR);
        }

        /* Safety: prevent infinite loop */
        if (ftell(f) <= elem_start) break;
    }

    fclose(f);

    if (duration < 0) return -1;
    /* Duration is in timecode units; convert to seconds */
    return (int)((duration * timecode_scale) / 1000000000.0);
}

/* ============================================================================
 * PDF Page Counting
 * ============================================================================ */

static int has_pdf_extension(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    return strcasecmp(ext, ".pdf") == 0;
}

/* Search buffer for /Type /Pages objects and extract /Count values.
 * Returns the maximum count found, or -1 if none found. */
static int pdf_search_pages_count(const char *data, size_t size) {
    int page_count = -1;

    for (size_t i = 0; i + 20 < size; i++) {
        if (data[i] == '/' && memcmp(data + i, "/Type", 5) == 0) {
            size_t j = i + 5;
            /* Skip whitespace */
            while (j < size && (data[j] == ' ' || data[j] == '\r' || data[j] == '\n'))
                j++;
            if (j + 6 < size && memcmp(data + j, "/Pages", 6) == 0) {
                /* Verify it's not /Page (single page) by checking next char */
                if (j + 6 < size && (data[j+6] == ' ' || data[j+6] == '/' ||
                    data[j+6] == '>' || data[j+6] == '\r' || data[j+6] == '\n')) {
                    /* Found /Type /Pages - find object boundaries (<< and >>) */
                    size_t obj_start = i;
                    while (obj_start > 1 && !(data[obj_start-1] == '<' && data[obj_start-2] == '<'))
                        obj_start--;
                    if (obj_start >= 2) obj_start -= 2;

                    size_t obj_end = j + 6;
                    while (obj_end + 1 < size && !(data[obj_end] == '>' && data[obj_end+1] == '>'))
                        obj_end++;

                    /* Search for /Count within object boundaries */
                    for (size_t k = obj_start; k + 7 < obj_end; k++) {
                        if (data[k] == '/' && memcmp(data + k, "/Count", 6) == 0) {
                            size_t m = k + 6;
                            while (m < obj_end && (data[m] == ' ' || data[m] == '\r' || data[m] == '\n'))
                                m++;
                            if (m < obj_end && data[m] >= '0' && data[m] <= '9') {
                                int count = 0;
                                while (m < obj_end && data[m] >= '0' && data[m] <= '9') {
                                    count = count * 10 + (data[m] - '0');
                                    m++;
                                }
                                if (count > page_count) {
                                    page_count = count;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    return page_count;
}

/* Decompress a FlateDecode stream using zlib.
 * Returns decompressed data (caller must free), or NULL on failure.
 * Sets *out_size to the decompressed size. */
static char *pdf_inflate(const unsigned char *src, size_t src_len, size_t *out_size) {
    /* Start with 4x the compressed size, grow if needed */
    size_t dst_capacity = src_len * 4;
    if (dst_capacity < 4096) dst_capacity = 4096;
    if (dst_capacity > 16 * 1024 * 1024) dst_capacity = 16 * 1024 * 1024; /* Cap at 16MB */

    char *dst = malloc(dst_capacity);
    if (!dst) return NULL;

    z_stream strm = {0};
    strm.next_in = (Bytef *)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef *)dst;
    strm.avail_out = dst_capacity;

    if (inflateInit(&strm) != Z_OK) {
        free(dst);
        return NULL;
    }

    int ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
        inflateEnd(&strm);
        free(dst);
        return NULL;
    }

    /* If buffer was too small, try once more with larger buffer */
    if (ret == Z_BUF_ERROR || strm.avail_in > 0) {
        inflateEnd(&strm);
        dst_capacity *= 4;
        if (dst_capacity > 64 * 1024 * 1024) { /* Hard limit 64MB */
            free(dst);
            return NULL;
        }
        char *new_dst = realloc(dst, dst_capacity);
        if (!new_dst) { free(dst); return NULL; }
        dst = new_dst;

        strm = (z_stream){0};
        strm.next_in = (Bytef *)src;
        strm.avail_in = src_len;
        strm.next_out = (Bytef *)dst;
        strm.avail_out = dst_capacity;

        if (inflateInit(&strm) != Z_OK) { free(dst); return NULL; }
        ret = inflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) {
            inflateEnd(&strm);
            free(dst);
            return NULL;
        }
    }

    *out_size = strm.total_out;
    inflateEnd(&strm);
    return dst;
}

/* Get page count from PDF file.
 * Returns page count, or -1 on failure.
 *
 * PDFs store page count in the document catalog's /Pages dictionary.
 * Modern PDFs often compress objects in ObjStm streams with FlateDecode.
 */
int get_pdf_page_count(const char *path) {
    if (!has_pdf_extension(path)) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 100) {
        close(fd);
        return -1;
    }

    size_t size = (size_t)st.st_size;
    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) return -1;

    /* Verify PDF header */
    if (memcmp(data, "%PDF-", 5) != 0) {
        munmap(data, size);
        return -1;
    }

    /* First try uncompressed search */
    int page_count = pdf_search_pages_count(data, size);

    /* If not found, search inside compressed object streams */
    if (page_count < 0) {
        /* Look for ObjStm with FlateDecode */
        for (size_t i = 0; i + 30 < size && page_count < 0; i++) {
            /* Find "stream" keyword after an ObjStm dictionary */
            if (memcmp(data + i, "stream", 6) == 0 &&
                (data[i+6] == '\r' || data[i+6] == '\n')) {
                /* Look backwards for << /Type /ObjStm ... /Filter /FlateDecode ... /Length N >> */
                size_t dict_start = (i > 512) ? i - 512 : 0;
                int is_objstm = 0;
                int has_flate = 0;
                size_t length = 0;

                for (size_t j = dict_start; j < i; j++) {
                    if (memcmp(data + j, "/Type", 5) == 0) {
                        size_t k = j + 5;
                        while (k < i && (data[k] == ' ' || data[k] == '/')) k++;
                        if (k + 6 <= i && memcmp(data + k - 1, "/ObjStm", 7) == 0)
                            is_objstm = 1;
                    }
                    if (memcmp(data + j, "/Filter", 7) == 0) {
                        size_t k = j + 7;
                        while (k < i && data[k] == ' ') k++;
                        if (k + 12 <= i && memcmp(data + k, "/FlateDecode", 12) == 0)
                            has_flate = 1;
                    }
                    if (memcmp(data + j, "/Length", 7) == 0) {
                        size_t k = j + 7;
                        while (k < i && (data[k] == ' ' || data[k] == '\r' || data[k] == '\n')) k++;
                        if (k < i && data[k] >= '0' && data[k] <= '9') {
                            length = 0;
                            while (k < i && data[k] >= '0' && data[k] <= '9') {
                                length = length * 10 + (data[k] - '0');
                                k++;
                            }
                        }
                    }
                }

                if (is_objstm && has_flate && length > 0 && length < size - i) {
                    /* Skip past "stream\r\n" or "stream\n" */
                    size_t stream_start = i + 6;
                    if (data[stream_start] == '\r') stream_start++;
                    if (data[stream_start] == '\n') stream_start++;

                    if (stream_start + length <= size) {
                        size_t decompressed_size;
                        char *decompressed = pdf_inflate((unsigned char *)data + stream_start,
                                                         length, &decompressed_size);
                        if (decompressed) {
                            int count = pdf_search_pages_count(decompressed, decompressed_size);
                            if (count > page_count) page_count = count;
                            free(decompressed);
                        }
                    }
                }
            }
        }
    }

    munmap(data, size);
    return page_count;
}

/* ============================================================================
 * File Type Name Detection
 * ============================================================================ */

#include <ctype.h>

/* Check shebang line for interpreter type */
static const char *get_type_from_shebang(const char *path, const Shebangs *sb) {
    if (!sb || sb->count == 0) return NULL;

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char line[256];
    const char *result = NULL;

    if (fgets(line, sizeof(line), f) && line[0] == '#' && line[1] == '!') {
        char *interp = line + 2;
        while (*interp == ' ') interp++;

        /* Handle /usr/bin/env interpreter */
        if (strncmp(interp, "/usr/bin/env", 12) == 0) {
            interp += 12;
            while (*interp == ' ') interp++;
        } else {
            /* Get basename of interpreter path */
            char *slash = strrchr(interp, '/');
            if (slash) interp = slash + 1;
        }

        /* Trim trailing whitespace/newline */
        char *end = interp;
        while (*end && !isspace((unsigned char)*end)) end++;
        *end = '\0';

        /* Lookup in config.toml shebangs */
        result = shebangs_lookup(sb, interp);
    }

    fclose(f);
    return result;
}

const char *get_file_type_name(const char *path, const FileTypes *ft,
                                const Shebangs *sb) {
    const char *ext = strrchr(path, '.');
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    /* Special filenames (not extension-based) */
    if (strcmp(basename, "Makefile") == 0 || strcmp(basename, "makefile") == 0 ||
        strcmp(basename, "GNUmakefile") == 0) return "Makefile";
    if (strcmp(basename, "CMakeLists.txt") == 0) return "CMake";
    if (strcmp(basename, "Dockerfile") == 0) return "Dockerfile";
    if (strcmp(basename, "Jenkinsfile") == 0) return "Jenkinsfile";
    if (strcmp(basename, "Vagrantfile") == 0) return "Vagrantfile";

    /* Extension-based lookup from config.toml */
    if (ft && ft->count > 0) {
        const char *type = filetypes_lookup(ft, path);
        if (type) return type;
    }

    /* No extension - try shebang */
    if (!ext || ext < basename || ext == basename) {
        return get_type_from_shebang(path, sb);
    }

    return NULL;
}

/* ============================================================================
 * Type Statistics
 * ============================================================================ */

/* Include ui.h for FileEntry and TreeNode definitions */
#include "ui.h"

void type_stats_init(TypeStats *stats) {
    memset(stats, 0, sizeof(*stats));
}

void type_stats_add(TypeStats *stats, const char *type_name,
                    int lines, ContentType content_type) {
    if (!type_name) type_name = "Other";

    stats->total_files++;

    /* Only count lines for text content */
    int has_lines = (content_type == CONTENT_TEXT && lines > 0);
    if (has_lines) {
        stats->total_lines += lines;
    }

    /* Find existing entry */
    for (int i = 0; i < stats->count; i++) {
        if (strcmp(stats->entries[i].name, type_name) == 0) {
            stats->entries[i].file_count++;
            if (has_lines) {
                stats->entries[i].line_count += lines;
                stats->entries[i].has_lines = 1;
            }
            return;
        }
    }

    /* Add new entry */
    if (stats->count < MAX_TYPE_STATS) {
        stats->entries[stats->count].name = type_name;
        stats->entries[stats->count].file_count = 1;
        stats->entries[stats->count].line_count = has_lines ? lines : 0;
        stats->entries[stats->count].has_lines = has_lines;
        stats->count++;
    }
}

static void type_stats_from_tree_recursive(TypeStats *stats, const TreeNode *node,
                                            const FileTypes *ft, const Shebangs *sb,
                                            int show_hidden) {
    const FileEntry *fe = &node->entry;

    /* Skip hidden files if not showing them */
    if (!show_hidden && fe->name[0] == '.') return;

    /* Skip ignored files (.git, gitignored, etc.) */
    if (fe->is_ignored) return;

    /* Process files */
    if (fe->type == FTYPE_FILE || fe->type == FTYPE_EXEC ||
        fe->type == FTYPE_SYMLINK || fe->type == FTYPE_SYMLINK_EXEC) {
        const char *type_name = get_file_type_name(fe->path, ft, sb);
        type_stats_add(stats, type_name, fe->line_count, fe->content_type);
    }

    /* Recurse into children */
    for (size_t i = 0; i < node->child_count; i++) {
        type_stats_from_tree_recursive(stats, &node->children[i], ft, sb, show_hidden);
    }
}

void type_stats_from_tree(TypeStats *stats, const TreeNode *node,
                          const FileTypes *ft, const Shebangs *sb,
                          int show_hidden) {
    type_stats_init(stats);
    type_stats_from_tree_recursive(stats, node, ft, sb, show_hidden);
}

static int type_stat_cmp(const void *a, const void *b) {
    const TypeStat *ta = a, *tb = b;
    /* Sort by file count descending, then by line count */
    if (tb->file_count > ta->file_count) return 1;
    if (tb->file_count < ta->file_count) return -1;
    if (tb->line_count > ta->line_count) return 1;
    if (tb->line_count < ta->line_count) return -1;
    return 0;
}

void type_stats_sort(TypeStats *stats) {
    qsort(stats->entries, stats->count, sizeof(TypeStat), type_stat_cmp);
}

/* ============================================================================
 * Content Metadata Computation
 * ============================================================================ */

void fileinfo_compute_content(struct FileEntry *fe, int do_line_count, int do_media_info) {
    if (!fe || !fe->path) return;

    /* Try media info first if requested */
    if (do_media_info) {
        int mp = get_image_megapixels(fe->path);
        if (mp >= 0) {
            fe->line_count = mp;
            fe->content_type = CONTENT_IMAGE;
            return;
        }

        int dur = get_audio_duration(fe->path);
        if (dur >= 0) {
            fe->line_count = dur;
            fe->content_type = CONTENT_AUDIO;
            return;
        }

        int pages = get_pdf_page_count(fe->path);
        if (pages >= 0) {
            fe->line_count = pages;
            fe->content_type = CONTENT_PDF;
            return;
        }
    }

    /* Try text line count if requested */
    if (do_line_count) {
        int lines = count_file_lines(fe->path);
        if (lines >= 0) {
            fe->line_count = lines;
            fe->word_count = count_file_words(fe->path);
            fe->content_type = CONTENT_TEXT;
            return;
        }
    }

    /* Binary or unknown */
    fe->line_count = -1;
    fe->word_count = -1;
    fe->content_type = CONTENT_BINARY;
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

int count_file_words(const char *path) {
    if (has_binary_extension(path)) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return st.st_size == 0 ? 0 : -1;
    }

    size_t size = (size_t)st.st_size;
    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) return -1;

    madvise(data, size, MADV_SEQUENTIAL);

    /* Check for binary content at start */
    size_t check_len = size < L_BINARY_CHECK_SIZE ? size : L_BINARY_CHECK_SIZE;
    if (memchr(data, '\0', check_len) != NULL) {
        munmap(data, size);
        return -1;
    }

    /* Count words (transitions from whitespace to non-whitespace) */
    long count = 0;
    int in_word = 0;
    const unsigned char *p = (const unsigned char *)data;
    const unsigned char *end = p + size;

    while (p < end) {
        unsigned char c = *p++;
        int is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                        c == '\v' || c == '\f');
        if (!is_space && !in_word) {
            count++;
            in_word = 1;
        } else if (is_space) {
            in_word = 0;
        }
    }

    munmap(data, size);

    return count > INT_MAX ? INT_MAX : (int)count;
}

/* ============================================================================
 * FileEntry Compute Functions
 * ============================================================================ */

#include "tree.h"

void fileinfo_compute_type_stats(struct FileEntry *fe, const struct TreeNode *node,
                                  const FileTypes *ft, const Shebangs *sb,
                                  int show_hidden) {
    type_stats_from_tree(&fe->type_stats, node, ft, sb, show_hidden);
    fe->has_type_stats = (fe->type_stats.total_files > 0);
}

void fileinfo_compute_git_dir_status(struct FileEntry *fe, GitCache *git) {
    fe->git_dir_status = git_get_dir_summary(git, fe->path);
    fe->has_git_dir_status = 1;
}

/* Helper to format count with K/M suffixes */
static void format_count_local(long count, char *buf, size_t len) {
    if (count >= 1000000) {
        snprintf(buf, len, "%.1fM", count / 1000000.0);
    } else if (count >= 1000) {
        snprintf(buf, len, "%.1fK", count / 1000.0);
    } else {
        snprintf(buf, len, "%ld", count);
    }
}

void fileinfo_compute_git_repo_info(struct FileEntry *fe, GitCache *git) {
    if (!fe->is_git_root) return;

    /* Branch and upstream status */
    GitBranchInfo gi;
    if (git_get_branch_info(fe->path, &gi)) {
        fe->branch = gi.branch;  /* Takes ownership */
        fe->has_upstream = gi.has_upstream;
        fe->out_of_sync = gi.out_of_sync;

        /* Get short hash */
        char hash[64] = "";
        char ref[128];
        snprintf(ref, sizeof(ref), "refs/heads/%s", fe->branch);
        if (git_read_ref(fe->path, ref, hash, sizeof(hash))) {
            snprintf(fe->short_hash, sizeof(fe->short_hash), "%.7s", hash);
        }
    }

    /* Commit count */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "git -C '%s' rev-list --count HEAD 2>/dev/null", fe->path);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            long count = atol(buf);
            if (count > 0) {
                format_count_local(count, fe->commit_count, sizeof(fe->commit_count));
            }
        }
        pclose(fp);
    }

    /* Latest tag with distance */
    snprintf(cmd, sizeof(cmd), "git -C '%s' describe --tags 2>/dev/null", fe->path);
    fp = popen(cmd, "r");
    if (fp) {
        char tag_buf[256];
        if (fgets(tag_buf, sizeof(tag_buf), fp)) {
            tag_buf[strcspn(tag_buf, "\n")] = '\0';
            if (tag_buf[0]) {
                /* Format: tag-name or tag-name-N-gHASH */
                /* Find the last two dashes to extract distance */
                char *last_dash = strrchr(tag_buf, '-');
                char *second_last = NULL;
                if (last_dash && last_dash > tag_buf && last_dash[1] == 'g') {
                    /* Looks like -gHASH suffix, find the distance before it */
                    *last_dash = '\0';
                    second_last = strrchr(tag_buf, '-');
                    if (second_last && second_last > tag_buf) {
                        char *endptr;
                        long dist = strtol(second_last + 1, &endptr, 10);
                        if (*endptr == '\0' && dist > 0) {
                            /* Valid distance found */
                            fe->tag_distance = (int)dist;
                            *second_last = '\0';
                        } else {
                            /* Not a valid distance, restore */
                            *last_dash = '-';
                        }
                    } else {
                        *last_dash = '-';
                    }
                }
                fe->tag = xstrdup(tag_buf);
            }
        }
        pclose(fp);
    }

    /* Remote URL */
    snprintf(cmd, sizeof(cmd), "git -C '%s' remote get-url origin 2>/dev/null", fe->path);
    fp = popen(cmd, "r");
    if (fp) {
        char remote_buf[512];
        if (fgets(remote_buf, sizeof(remote_buf), fp)) {
            remote_buf[strcspn(remote_buf, "\n")] = '\0';
            if (remote_buf[0]) {
                fe->remote = xstrdup(remote_buf);
            }
        }
        pclose(fp);
    }

    /* Repo status */
    fe->repo_status = git_get_dir_summary(git, fe->path);
    fe->has_git_repo_info = 1;
}
