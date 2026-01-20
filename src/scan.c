/*
 * scan.c - Shared directory scanning with parallel processing
 *
 * Uses getattrlistbulk on macOS for faster metadata fetching.
 * Falls back to readdir+fstatat on Linux/other platforms.
 * Both paths use OMP task-based parallelism for subdirectories.
 */

#include "scan.h"
#include "common.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/attr.h>
#include <sys/vnode.h>
#define ATTR_BUF_SIZE (128 * 1024)
#endif

/* Scan context passed to recursive calls */
typedef struct {
    scan_store_fn store_fn;
    scan_cache_fn cache_fn;
    volatile int *shutdown;
    long threshold;
} ScanContext;

static ScanResult scan_impl(const char *path, dev_t root_dev, const ScanContext *ctx);

ScanResult scan_directory(const char *path,
                          scan_store_fn store_fn,
                          scan_cache_fn cache_fn,
                          volatile int *shutdown,
                          long threshold) {
    ScanResult result;
    ScanContext ctx = {store_fn, cache_fn, shutdown, threshold};

    #pragma omp parallel
    #pragma omp single
    {
        result = scan_impl(path, 0, &ctx);
    }
    return result;
}

#ifdef __APPLE__
/* macOS: use getattrlistbulk for faster metadata fetching */
static ScanResult scan_impl(const char *path, dev_t root_dev, const ScanContext *ctx) {
    (void)root_dev;  /* Not used on macOS - firmlinks cross device boundaries */
    ScanResult result = {0, 0};
    if (ctx->shutdown && *ctx->shutdown) return result;

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return (ScanResult){-1, -1};

    /* Skip network and virtual filesystems */
    if (path_is_network_fs(path) || path_is_virtual_fs(path)) {
        close(dirfd);
        return (ScanResult){0, 0};
    }

    int skip_file_count = path_is_git_dir(path);

    struct attrlist attrList = {0};
    attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrList.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME |
                          ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE;
    attrList.fileattr = ATTR_FILE_ALLOCSIZE;

    char *attrBuf = malloc(ATTR_BUF_SIZE);
    if (!attrBuf) {
        close(dirfd);
        return (ScanResult){-1, -1};
    }

    char **subdirs = NULL;
    size_t subdir_count = 0;
    size_t subdir_cap = 0;

    int count;
    while ((count = getattrlistbulk(dirfd, &attrList, attrBuf, ATTR_BUF_SIZE, 0)) > 0) {
        if (ctx->shutdown && *ctx->shutdown) break;

        char *ptr = attrBuf;
        for (int i = 0; i < count; i++) {
            uint32_t length = *(uint32_t *)ptr;
            char *p = ptr + sizeof(uint32_t);

            attribute_set_t returned = *(attribute_set_t *)p;
            p += sizeof(attribute_set_t);

            if (returned.commonattr & ATTR_CMN_ERROR) {
                p += sizeof(uint32_t);
            }

            attrreference_t name_ref = *(attrreference_t *)p;
            char *name = p + name_ref.attr_dataoffset;
            p += sizeof(attrreference_t);

            fsobj_type_t obj_type = *(fsobj_type_t *)p;
            p += sizeof(fsobj_type_t);

            off_t file_size = 0;
            if (returned.fileattr & ATTR_FILE_ALLOCSIZE) {
                file_size = *(off_t *)p;
            }

            if (!PATH_IS_DOT_OR_DOTDOT(name)) {
                if (obj_type == VREG) {
                    result.size += file_size;
                    if (!skip_file_count) result.file_count++;
                } else if (obj_type == VDIR) {
                    char *full = malloc(PATH_MAX);
                    if (full) {
                        size_t plen = strlen(path);
                        int need_slash = (plen > 0 && path[plen - 1] != '/');
                        snprintf(full, PATH_MAX, need_slash ? "%s/%s" : "%s%s",
                                 path, name);

                        if (path_should_skip_firmlink(full)) {
                            free(full);
                        } else {
                            if (subdir_count >= subdir_cap) {
                                subdir_cap = subdir_cap ? subdir_cap * 2 : 16;
                                char **new_subdirs = realloc(subdirs, subdir_cap * sizeof(char *));
                                if (!new_subdirs) {
                                    free(full);
                                } else {
                                    subdirs = new_subdirs;
                                    subdirs[subdir_count++] = full;
                                }
                            } else {
                                subdirs[subdir_count++] = full;
                            }
                        }
                    }
                }
            }
            ptr += length;
        }
    }

    free(attrBuf);
    close(dirfd);

    /* Process subdirectories with OMP tasks */
    ScanResult *sub_stats = NULL;
    if (subdir_count > 0) {
        sub_stats = malloc(subdir_count * sizeof(ScanResult));
        if (sub_stats) {
            for (size_t i = 0; i < subdir_count; i++) {
                /* Check cache first if available */
                off_t cached_size;
                long cached_count;
                if (ctx->cache_fn && ctx->cache_fn(subdirs[i], &cached_size, &cached_count)) {
                    sub_stats[i].size = cached_size;
                    sub_stats[i].file_count = cached_count;
                } else {
                    #pragma omp task shared(sub_stats) firstprivate(i, root_dev, ctx)
                    sub_stats[i] = scan_impl(subdirs[i], root_dev, ctx);
                }
            }
            #pragma omp taskwait

            for (size_t i = 0; i < subdir_count; i++) {
                if (sub_stats[i].size >= 0) result.size += sub_stats[i].size;
                if (!skip_file_count && sub_stats[i].file_count >= 0)
                    result.file_count += sub_stats[i].file_count;
                free(subdirs[i]);
            }
            free(sub_stats);
        } else {
            for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
        }
        free(subdirs);
    }

    if (ctx->store_fn && !skip_file_count &&
        result.file_count >= ctx->threshold && strcmp(path, "/") != 0) {
        #pragma omp critical
        ctx->store_fn(path, result.size, result.file_count);
    }

    if (skip_file_count) result.file_count = -1;
    return result;
}

#else
/* Linux/other: use readdir + fstatat */
static ScanResult scan_impl(const char *path, dev_t root_dev, const ScanContext *ctx) {
    ScanResult result = {0, 0};
    if (ctx->shutdown && *ctx->shutdown) return result;

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return (ScanResult){-1, -1};

    struct stat dir_st;
    if (fstat(dirfd, &dir_st) != 0) {
        close(dirfd);
        return (ScanResult){-1, -1};
    }

    if (root_dev == 0) {
        root_dev = dir_st.st_dev;
    } else if (dir_st.st_dev != root_dev) {
        close(dirfd);
        return (ScanResult){0, 0};
    }

    if (path_is_network_fs(path) || path_is_virtual_fs(path)) {
        close(dirfd);
        return (ScanResult){0, 0};
    }

    int skip_file_count = path_is_git_dir(path);

    DIR *dir = fdopendir(dirfd);
    if (!dir) {
        close(dirfd);
        return (ScanResult){-1, -1};
    }

    char **subdirs = NULL;
    size_t subdir_count = 0;
    size_t subdir_cap = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ctx->shutdown && *ctx->shutdown) break;
        if (PATH_IS_DOT_OR_DOTDOT(entry->d_name)) continue;

        int is_dir = 0;

#ifdef DT_DIR
        if (entry->d_type == DT_DIR) {
            is_dir = 1;
        } else if (entry->d_type == DT_REG) {
            struct stat st;
            if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                result.size += st.st_size;
                if (!skip_file_count) result.file_count++;
            }
        } else if (entry->d_type == DT_LNK) {
            if (!skip_file_count) result.file_count++;
        } else if (entry->d_type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    is_dir = 1;
                } else if (S_ISREG(st.st_mode)) {
                    result.size += st.st_size;
                    if (!skip_file_count) result.file_count++;
                } else if (S_ISLNK(st.st_mode)) {
                    if (!skip_file_count) result.file_count++;
                }
            }
        }
#else
        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISDIR(st.st_mode)) {
                is_dir = 1;
            } else if (S_ISREG(st.st_mode)) {
                result.size += st.st_size;
                if (!skip_file_count) result.file_count++;
            } else if (S_ISLNK(st.st_mode)) {
                if (!skip_file_count) result.file_count++;
            }
        }
#endif

        if (is_dir) {
            char *full = malloc(PATH_MAX);
            if (full) {
                size_t plen = strlen(path);
                int need_slash = (plen > 0 && path[plen - 1] != '/');
                snprintf(full, PATH_MAX, need_slash ? "%s/%s" : "%s%s",
                         path, entry->d_name);

                if (path_should_skip_firmlink(full)) {
                    free(full);
                } else {
                    if (subdir_count >= subdir_cap) {
                        subdir_cap = subdir_cap ? subdir_cap * 2 : 16;
                        char **new_subdirs = realloc(subdirs, subdir_cap * sizeof(char *));
                        if (!new_subdirs) {
                            free(full);
                        } else {
                            subdirs = new_subdirs;
                            subdirs[subdir_count++] = full;
                        }
                    } else {
                        subdirs[subdir_count++] = full;
                    }
                }
            }
        }
    }
    closedir(dir);

    /* Process subdirectories with OMP tasks */
    ScanResult *sub_stats = NULL;
    if (subdir_count > 0) {
        sub_stats = malloc(subdir_count * sizeof(ScanResult));
        if (sub_stats) {
            for (size_t i = 0; i < subdir_count; i++) {
                /* Check cache first if available */
                off_t cached_size;
                long cached_count;
                if (ctx->cache_fn && ctx->cache_fn(subdirs[i], &cached_size, &cached_count)) {
                    sub_stats[i].size = cached_size;
                    sub_stats[i].file_count = cached_count;
                } else {
                    #pragma omp task shared(sub_stats) firstprivate(i, root_dev, ctx)
                    sub_stats[i] = scan_impl(subdirs[i], root_dev, ctx);
                }
            }
            #pragma omp taskwait

            for (size_t i = 0; i < subdir_count; i++) {
                if (sub_stats[i].size >= 0) result.size += sub_stats[i].size;
                if (!skip_file_count && sub_stats[i].file_count >= 0)
                    result.file_count += sub_stats[i].file_count;
                free(subdirs[i]);
            }
            free(sub_stats);
        } else {
            for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
        }
        free(subdirs);
    }

    if (ctx->store_fn && !skip_file_count &&
        result.file_count >= ctx->threshold && strcmp(path, "/") != 0) {
        #pragma omp critical
        ctx->store_fn(path, result.size, result.file_count);
    }

    if (skip_file_count) result.file_count = -1;
    return result;
}
#endif
