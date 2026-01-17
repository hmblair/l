/*
 * fileinfo.h - File type detection, line counting, and image parsing
 */

#ifndef L_FILEINFO_H
#define L_FILEINFO_H

#include "common.h"
#include "icons.h"  /* For FileType enum */

/* ============================================================================
 * Content Type (for polymorphic line_count field)
 * ============================================================================ */

typedef enum {
    CONTENT_UNKNOWN = 0,
    CONTENT_TEXT,        /* line_count = lines */
    CONTENT_IMAGE,       /* line_count = megapixels * 10 */
    CONTENT_AUDIO,       /* line_count = duration seconds */
    CONTENT_PDF,         /* line_count = page count */
    CONTENT_BINARY,      /* not countable */
} ContentType;

/* ============================================================================
 * File Type Detection
 * ============================================================================ */

FileType detect_file_type(const char *path, struct stat *st, char **symlink_target);
const char *get_file_color(FileType type, int is_cwd, int is_ignored, int is_tty, int color_all);

/* ============================================================================
 * Line Counting and Media Parsing
 * ============================================================================ */

int count_file_lines(const char *path);
int count_file_words(const char *path);
int get_image_megapixels(const char *path);
int get_audio_duration(const char *path);
int get_pdf_page_count(const char *path);

/* Forward declare FileEntry to avoid circular dependency */
struct FileEntry;

/* Compute content metadata for a FileEntry.
 * @param fe         The file entry to update
 * @param line_count If true, compute line counts for text files
 * @param media_info If true, parse images/audio/PDF for metadata
 * Sets content_type and line_count fields appropriately. */
void fileinfo_compute_content(struct FileEntry *fe, int line_count, int media_info);

/* Get human-readable file type name from path (e.g., "C source", "Python")
 * Returns NULL if type cannot be determined */
const char *get_file_type_name(const char *path, const FileTypes *ft,
                                const Shebangs *sb);

/* ============================================================================
 * Type Statistics (for summary mode)
 * ============================================================================ */

#define MAX_TYPE_STATS 128  /* Maximum distinct file types tracked */

/* File type statistics for summary (supports both text and non-text) */
typedef struct {
    const char *name;       /* Type name (e.g., "C source", "JPEG image") */
    int file_count;         /* Number of files of this type */
    long line_count;        /* Total lines (0 if not applicable, e.g., images) */
    int has_lines;          /* 1 if line_count is meaningful */
} TypeStat;

typedef struct {
    TypeStat entries[MAX_TYPE_STATS];
    int count;
    int total_files;        /* Total file count across all types */
    long total_lines;       /* Total lines (text files only) */
} TypeStats;

/* Forward declare TreeNode (defined in ui.h) */
struct TreeNode;

/* Initialize empty TypeStats */
void type_stats_init(TypeStats *stats);

/* Add a file to type statistics */
void type_stats_add(TypeStats *stats, const char *type_name,
                    int lines, ContentType content_type);

/* Compute type stats from a tree node recursively.
 * NOTE: Tree must be fully expanded (use max_depth = L_MAX_DEPTH). */
void type_stats_from_tree(TypeStats *stats, const struct TreeNode *node,
                          const FileTypes *ft, const Shebangs *sb,
                          int show_hidden);

/* Sort type stats by line count descending (text files first) */
void type_stats_sort(TypeStats *stats);

/* ============================================================================
 * FileEntry Compute Functions
 * ============================================================================ */

#include "git.h"

/* Compute type statistics for a directory.
 * Populates fe->type_stats and sets fe->has_type_stats = 1.
 * Requires: node is a fully-expanded directory tree. */
void fileinfo_compute_type_stats(struct FileEntry *fe, const struct TreeNode *node,
                                  const FileTypes *ft, const Shebangs *sb,
                                  int show_hidden);

/* Compute git repository info for a git root.
 * Populates fe->branch, fe->tag, fe->remote, fe->short_hash, fe->commit_count,
 * fe->has_upstream, fe->out_of_sync, fe->repo_status, and sets fe->has_git_repo_info = 1.
 * Requires: fe->is_git_root is true. */
void fileinfo_compute_git_repo_info(struct FileEntry *fe, GitCache *git);

/* Compute git directory status (aggregated from children).
 * Populates fe->git_dir_status and sets fe->has_git_dir_status = 1. */
void fileinfo_compute_git_dir_status(struct FileEntry *fe, GitCache *git);

#endif /* L_FILEINFO_H */
