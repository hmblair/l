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

/* ============================================================================
 * Visited inode set - prevents double-counting firmlinks and bind mounts
 * ============================================================================ */

#define VISITED_BUCKETS 4096

typedef struct VisitedNode {
    dev_t dev;
    ino_t ino;
    struct VisitedNode *next;
} VisitedNode;

typedef struct {
    VisitedNode *buckets[VISITED_BUCKETS];
} VisitedSet;

static void visited_init(VisitedSet *set) {
    memset(set->buckets, 0, sizeof(set->buckets));
}

static unsigned visited_hash(dev_t dev, ino_t ino) {
    return (unsigned)((dev * 2654435761u) ^ (ino * 2246822519u)) % VISITED_BUCKETS;
}

/* Returns 1 if already visited, 0 if new (and inserts it) */
static int visited_check_and_insert(VisitedSet *set, dev_t dev, ino_t ino) {
    unsigned h = visited_hash(dev, ino);
    int found = 0;

    #pragma omp critical(visited_set)
    {
        VisitedNode *n = set->buckets[h];
        while (n) {
            if (n->dev == dev && n->ino == ino) {
                found = 1;
                break;
            }
            n = n->next;
        }
        if (!found) {
            VisitedNode *node = malloc(sizeof(VisitedNode));
            if (node) {
                node->dev = dev;
                node->ino = ino;
                node->next = set->buckets[h];
                set->buckets[h] = node;
            }
        }
    }
    return found;
}

static void visited_free(VisitedSet *set) {
    for (int i = 0; i < VISITED_BUCKETS; i++) {
        VisitedNode *n = set->buckets[i];
        while (n) {
            VisitedNode *next = n->next;
            free(n);
            n = next;
        }
    }
}

/* ============================================================================
 * Scan context and helpers
 * ============================================================================ */

/* Scan context passed to recursive calls */
typedef struct {
    scan_store_fn store_fn;
    scan_cache_fn cache_fn;
    volatile int *shutdown;
    long threshold;
    VisitedSet *visited;
} ScanContext;

#define MAX_SCAN_DEPTH 128

static ScanResult scan_impl(const char *path, int depth,
                            const ScanContext *ctx);

/* Common setup for scan_impl - returns dirfd on success, -1 to skip, -2 on error */
static int scan_setup(const char *path, struct stat *dir_st, int depth,
                      const ScanContext *ctx) {
    if (ctx->shutdown && *ctx->shutdown) return -1;
    if (depth >= MAX_SCAN_DEPTH) return -1;

    int dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return -2;

    if (path_is_virtual_fs(path)) {
        close(dirfd);
        return -1;
    }

    if (fstat(dirfd, dir_st) != 0) {
        close(dirfd);
        return -2;
    }

    if (visited_check_and_insert(ctx->visited, dir_st->st_dev, dir_st->st_ino)) {
        close(dirfd);
        /* Cache duplicates at 0 so live-scan fallbacks find them */
        if (ctx->store_fn) {
            #pragma omp critical
            ctx->store_fn(path, 0, 0);
        }
        return -1;
    }

    return dirfd;
}

/* Process subdirectories with OMP tasks, accumulating into result */
static void scan_process_subdirs(char **subdirs, size_t subdir_count,
                                 int depth, const ScanContext *ctx,
                                 int skip_file_count, ScanResult *result) {
    if (subdir_count == 0) return;

    ScanResult *sub_stats = malloc(subdir_count * sizeof(ScanResult));
    if (sub_stats) {
        for (size_t i = 0; i < subdir_count; i++) {
            /* Check cache first if available */
            off_t cached_size;
            long cached_count;
            if (ctx->cache_fn && ctx->cache_fn(subdirs[i], &cached_size, &cached_count)) {
                sub_stats[i].size = cached_size;
                sub_stats[i].file_count = cached_count;
            } else {
                #pragma omp task shared(sub_stats) firstprivate(i, depth, ctx)
                sub_stats[i] = scan_impl(subdirs[i], depth, ctx);
            }
        }
        #pragma omp taskwait

        for (size_t i = 0; i < subdir_count; i++) {
            if (sub_stats[i].size >= 0) result->size += sub_stats[i].size;
            if (!skip_file_count && sub_stats[i].file_count >= 0)
                result->file_count += sub_stats[i].file_count;
            free(subdirs[i]);
        }
        free(sub_stats);
    } else {
        for (size_t i = 0; i < subdir_count; i++) free(subdirs[i]);
    }
    free(subdirs);
}

/* Finalize scan result: optionally store and handle skip_file_count */
static void scan_finalize(const char *path, const ScanContext *ctx,
                          int skip_file_count, ScanResult *result) {
    if (ctx->store_fn && !skip_file_count &&
        result->file_count >= ctx->threshold && strcmp(path, "/") != 0) {
        #pragma omp critical
        ctx->store_fn(path, result->size, result->file_count);
    }
    if (skip_file_count) result->file_count = -1;
}

/* Common teardown for scan_impl */
static void scan_teardown(const char *path, char **subdirs, size_t subdir_count,
                          int depth, const ScanContext *ctx,
                          int skip_file_count, ScanResult *result) {
    scan_process_subdirs(subdirs, subdir_count, depth + 1,
                         ctx, skip_file_count, result);
    scan_finalize(path, ctx, skip_file_count, result);
}

ScanResult scan_directory(const char *path,
                          scan_store_fn store_fn,
                          scan_cache_fn cache_fn,
                          volatile int *shutdown,
                          long threshold) {
    ScanResult result;
    VisitedSet visited;
    visited_init(&visited);
    ScanContext ctx = {store_fn, cache_fn, shutdown, threshold, &visited};

    #pragma omp parallel
    #pragma omp single
    {
        result = scan_impl(path, 0, &ctx);
    }

    visited_free(&visited);
    return result;
}

#ifdef __APPLE__
/* macOS: use getattrlistbulk for faster metadata fetching */
static ScanResult scan_impl(const char *path, int depth,
                            const ScanContext *ctx) {
    struct stat dir_st;

    int dirfd = scan_setup(path, &dir_st, depth, ctx);
    if (dirfd == -1) return (ScanResult){0, 0};
    if (dirfd == -2) return (ScanResult){-1, -1};

    ScanResult result = {dir_st.st_blocks * 512, 0};
    int skip_file_count = path_is_git_dir(path);

    struct attrlist attrList = {0};
    attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrList.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME |
                          ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE;
    attrList.fileattr = ATTR_FILE_ALLOCSIZE;
    attrList.dirattr = ATTR_DIR_ALLOCSIZE;

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

            off_t alloc_size = 0;
            if (obj_type == VREG && (returned.fileattr & ATTR_FILE_ALLOCSIZE)) {
                alloc_size = *(off_t *)p;
            } else if (obj_type == VDIR && (returned.dirattr & ATTR_DIR_ALLOCSIZE)) {
                alloc_size = *(off_t *)p;
            }

            if (!PATH_IS_DOT_OR_DOTDOT(name)) {
                if (obj_type == VREG || obj_type == VLNK) {
                    result.size += alloc_size;
                    if (!skip_file_count) result.file_count++;
                } else if (obj_type == VDIR) {
                    result.size += alloc_size;
                    char *full = malloc(PATH_MAX);
                    if (full) {
                        size_t plen = strlen(path);
                        int need_slash = (plen > 0 && path[plen - 1] != '/');
                        snprintf(full, PATH_MAX, need_slash ? "%s/%s" : "%s%s",
                                 path, name);

                        if (subdir_count >= subdir_cap) {
                            size_t new_cap = subdir_cap ? subdir_cap * 2 : 16;
                            char **new_subdirs = realloc(subdirs, new_cap * sizeof(char *));
                            if (!new_subdirs) {
                                free(full);
                            } else {
                                subdir_cap = new_cap;
                                subdirs = new_subdirs;
                                subdirs[subdir_count++] = full;
                            }
                        } else {
                            subdirs[subdir_count++] = full;
                        }
                    }
                }
            }
            ptr += length;
        }
    }

    free(attrBuf);
    close(dirfd);

    scan_teardown(path, subdirs, subdir_count, depth,
                  ctx, skip_file_count, &result);
    return result;
}

#else
/* Linux/other: use readdir + fstatat */
static ScanResult scan_impl(const char *path, int depth,
                            const ScanContext *ctx) {
    struct stat dir_st;

    int dirfd = scan_setup(path, &dir_st, depth, ctx);
    if (dirfd == -1) return (ScanResult){0, 0};
    if (dirfd == -2) return (ScanResult){-1, -1};

    ScanResult result = {dir_st.st_blocks * 512, 0};
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

        struct stat st;
        if (fstatat(dirfd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISDIR(st.st_mode)) {
                is_dir = 1;
                result.size += st.st_blocks * 512;
            } else if (S_ISREG(st.st_mode)) {
                result.size += st.st_blocks * 512;
                if (!skip_file_count) result.file_count++;
            } else if (S_ISLNK(st.st_mode)) {
                if (!skip_file_count) result.file_count++;
            }
        }

        if (is_dir) {
            char *full = malloc(PATH_MAX);
            if (full) {
                size_t plen = strlen(path);
                int need_slash = (plen > 0 && path[plen - 1] != '/');
                snprintf(full, PATH_MAX, need_slash ? "%s/%s" : "%s%s",
                         path, entry->d_name);

                if (subdir_count >= subdir_cap) {
                    size_t new_cap = subdir_cap ? subdir_cap * 2 : 16;
                    char **new_subdirs = realloc(subdirs, new_cap * sizeof(char *));
                    if (!new_subdirs) {
                        free(full);
                    } else {
                        subdir_cap = new_cap;
                        subdirs = new_subdirs;
                        subdirs[subdir_count++] = full;
                    }
                } else {
                    subdirs[subdir_count++] = full;
                }
            }
        }
    }
    closedir(dir);

    scan_teardown(path, subdirs, subdir_count, depth,
                  ctx, skip_file_count, &result);
    return result;
}
#endif
