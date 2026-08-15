/*
 * tree.c - Tree data structures and building
 */

#include "tree.h"
#include "cache.h"
#include <dirent.h>
#include <fnmatch.h>
#include <string.h>

/* ============================================================================
 * File List Management
 * ============================================================================ */

void file_list_init(FileList *list) {
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

void file_list_add(FileList *list, FileEntry *entry) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : L_INITIAL_FILE_CAPACITY;
        list->entries = xrealloc(list->entries, list->capacity * sizeof(FileEntry));
    }
    list->entries[list->count++] = *entry;
}

void file_entry_free(FileEntry *entry) {
    free(entry->path);
    free(entry->abs_path);
    free(entry->symlink_target);
    free(entry->branch);
    free(entry->tag);
    free(entry->remote);
}

void file_entry_init(FileEntry *fe, const char *path, int is_virtual_fs) {
    memset(fe, 0, sizeof(*fe));
    fe->path = xstrdup(path);
    fe->name = strrchr(fe->path, '/');
    fe->name = fe->name ? fe->name + 1 : fe->path;
    fe->line_count = -1;
    fe->word_count = -1;
    fe->file_count = -1;

    struct stat st;
    memset(&st, 0, sizeof(st));
    fe->type = detect_file_type(fe->path, &st, &fe->symlink_target, &fe->alloc_size);
    fe->mode = st.st_mode;
    fe->dev = st.st_dev;
    fe->mtime = GET_MTIME(st);
    fe->size = is_virtual_fs ? -1 : st.st_size;
    fe->is_readonly = (access(fe->path, W_OK) != 0);
}

void file_entry_compute(FileEntry *fe, const ComputeOpts *c, int is_virtual_fs) {
    if (is_virtual_fs) return;

    int is_dir = (fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR);
    int is_file = (fe->type == FTYPE_FILE || fe->type == FTYPE_EXEC ||
                   fe->type == FTYPE_SYMLINK || fe->type == FTYPE_SYMLINK_EXEC);

    if (is_dir && (c->sizes || c->file_counts)) {
        DirStats stats = get_dir_stats_cached(fe->path);
        if (c->sizes) fe->size = stats.size;
        if (c->file_counts) fe->file_count = stats.file_count;
    } else if (is_file && (c->line_counts || c->media_info)) {
        fileinfo_compute_content(fe, c);
    }
}

void file_list_free(FileList *list) {
    for (size_t i = 0; i < list->count; i++) {
        file_entry_free(&list->entries[i]);
    }
    free(list->entries);
    list->entries = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* ============================================================================
 * Sorting
 * ============================================================================ */

static int entry_cmp_name(const void *a, const void *b) {
    const FileEntry *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

static int node_cmp_size(const void *a, const void *b) {
    const TreeNode *na = a, *nb = b;
    if (nb->entry.size > na->entry.size) return 1;
    if (nb->entry.size < na->entry.size) return -1;
    return 0;
}

static int node_cmp_time(const void *a, const void *b) {
    const TreeNode *na = a, *nb = b;
    if (nb->entry.mtime > na->entry.mtime) return 1;
    if (nb->entry.mtime < na->entry.mtime) return -1;
    return 0;
}

static void reverse_nodes(TreeNode *nodes, size_t count) {
    for (size_t i = 0; i < count / 2; i++) {
        TreeNode tmp = nodes[i];
        nodes[i] = nodes[count - 1 - i];
        nodes[count - 1 - i] = tmp;
    }
}

/* Order a parent's children for display. Entries arrive name-sorted from
 * read_directory; size/time ordering runs here — after metadata is final,
 * since -S sorts on computed directory totals. */
static void order_children(TreeNode *parent, const TreeBuildOpts *opts) {
    if (parent->child_count == 0) return;

    if (opts->sort_by == SORT_SIZE || opts->sort_by == SORT_TIME) {
        qsort(parent->children, parent->child_count, sizeof(TreeNode),
              opts->sort_by == SORT_SIZE ? node_cmp_size : node_cmp_time);
        if (opts->sort_reverse) reverse_nodes(parent->children, parent->child_count);
    } else if (opts->sort_reverse) {
        reverse_nodes(parent->children, parent->child_count);
    }
}

/* ============================================================================
 * Directory Reading
 * ============================================================================ */

/* Read a directory's entries, name-sorted. Metadata (dir stats, file
 * content) and -S/-T/-r ordering are the caller's concern: sizes are summed
 * bottom-up after recursion, so both must wait until subtrees are final
 * (build_tree_children / tree_expand_node + order_children). */
int read_directory(const char *dir_path, FileList *list,
                   const TreeBuildOpts *opts) {
    (void)opts;
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    int is_virtual_fs = path_is_virtual_fs(dir_path);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (PATH_IS_DOT_OR_DOTDOT(entry->d_name)) continue;
        /* Hidden entries are always included in the tree; -a (show_hidden) only
         * controls whether they are displayed (a visibility concern) and whether
         * we recurse into hidden directories (see build_tree_children). */

        char full_path[PATH_MAX];
        path_join(full_path, sizeof(full_path), dir_path, entry->d_name);

        FileEntry fe;
        file_entry_init(&fe, full_path, is_virtual_fs);
        file_list_add(list, &fe);
    }
    closedir(dir);

    qsort(list->entries, list->count, sizeof(FileEntry), entry_cmp_name);
    return 0;
}

/* ============================================================================
 * Tree Node Management
 * ============================================================================ */

void tree_node_free(TreeNode *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) {
        tree_node_free(&node->children[i]);
    }
    free(node->children);
    file_entry_free(&node->entry);
}

int node_is_directory(const TreeNode *node) {
    return node->entry.type == FTYPE_DIR || node->entry.type == FTYPE_SYMLINK_DIR;
}

/* ============================================================================
 * Tree Building
 * ============================================================================ */

/* Check if directory has a .gitignore containing a line with just "*" */
static int has_ignore_all_gitignore(const char *dir_path) {
    char gitignore_path[PATH_MAX];
    snprintf(gitignore_path, sizeof(gitignore_path), "%s/.gitignore", dir_path);

    FILE *fp = fopen(gitignore_path, "r");
    if (!fp) return 0;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing whitespace/newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                          line[len-1] == ' ' || line[len-1] == '\t')) {
            line[--len] = '\0';
        }
        if (strcmp(line, "*") == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int should_skip_dir(const char *name, int is_ignored, int skip_gitignored) {
    if (is_ignored && skip_gitignored) return 1;
    if (path_name_is_opaque(name)) return 1;
    return 0;
}

static void apply_git_status(FileEntry *fe, GitCache *git, int compute_diff) {
    /* Key by the canonical path when available: the cache is keyed by paths
     * derived from the (resolved) repo root, which the display path can
     * differ from under a symlinked cwd. */
    const char *key = fe->abs_path ? fe->abs_path : fe->path;
    GitStatusNode *git_node = git_cache_get_node(git, key);
    if (git_node) {
        fe->git_flags = git_node->flags;
        if (compute_diff) {
            fe->diff_added = git_node->lines_added;
            fe->diff_removed = git_node->lines_removed;
        }
    }
}

/* Annotate a git repo root entry with everything its row displays: remote
 * URL, latest tag, and branch/upstream state. Runs at build time so the
 * renderer never touches git (git_get_branch_info can spawn git rev-list
 * when the branch is out of sync with its upstream). */
static void annotate_git_root(FileEntry *fe) {
    fe->is_git_root = 1;
    fe->remote = git_get_remote_url(fe->path);
    fe->tag = git_get_latest_tag(fe->path);

    GitBranchInfo gi;
    if (git_get_branch_info(fe->path, &gi)) {
        fe->branch = gi.branch;  /* takes ownership */
        snprintf(fe->short_hash, sizeof(fe->short_hash), "%.7s", gi.commit);
        fe->has_upstream = gi.has_upstream;
        fe->out_of_sync = gi.out_of_sync;
        fe->ahead = gi.ahead;
        fe->behind = gi.behind;
    }
}

/* Find git repo roots in a file list and mark them.
 * Returns array of repo paths (canonical, for cache keying) to populate
 * (caller must free the array). Sets is_git_repo_root[i] for each entry and
 * annotates each root with its repo info. Repos nested inside another repo
 * (submodules, vendored checkouts) are populated like any other, so their
 * rows aggregate the changes inside them. */
static char **find_git_repo_roots(FileList *list, int *is_git_repo_root,
                                   size_t *out_count) {
    char **git_repos = NULL;
    size_t count = 0;

    for (size_t i = 0; i < list->count; i++) {
        is_git_repo_root[i] = 0;
        FileEntry *fe = &list->entries[i];
        if ((fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) &&
            strcmp(fe->name, ".git") != 0 && path_is_git_root(fe->path)) {
            is_git_repo_root[i] = 1;
            annotate_git_root(fe);
            git_repos = xrealloc(git_repos, (count + 1) * sizeof(char *));
            git_repos[count++] = fe->abs_path ? fe->abs_path : fe->path;
        }
    }

    *out_count = count;
    return git_repos;
}

/* ============================================================================
 * Ghost Entries - synthesized rows for git-deleted paths
 * ============================================================================ */

typedef struct {
    const char *parent;      /* Canonical parent directory */
    size_t parent_len;
    char **paths;            /* Deleted descendant paths (owned) */
    size_t count;
    size_t cap;
} GhostCollect;

static void ghost_collect_cb(const char *path, unsigned flags,
                             int lines_added, int lines_removed, void *ud) {
    GhostCollect *gc = ud;
    (void)lines_added; (void)lines_removed;
    if (!(flags & (GITF_WT_DELETED | GITF_STAGED_DELETED))) return;
    if (strncmp(path, gc->parent, gc->parent_len) != 0 ||
        path[gc->parent_len] != '/') return;
    if (gc->count == gc->cap) {
        gc->cap = gc->cap ? gc->cap * 2 : 8;
        gc->paths = xrealloc(gc->paths, gc->cap * sizeof(char *));
    }
    gc->paths[gc->count++] = xstrdup(path);
}

static int file_list_contains_name(const FileList *list, const char *name,
                                   size_t name_len) {
    for (size_t i = 0; i < list->count; i++) {
        if (strncmp(list->entries[i].name, name, name_len) == 0 &&
            list->entries[i].name[name_len] == '\0') return 1;
    }
    return 0;
}

/* Append a ghost entry. Only identity and type are real; every metadata field
 * keeps its "not computed" value so the columns render placeholders. */
static void ghost_entry_add(FileList *list, const char *parent_path,
                            const char *name, size_t name_len, int is_dir) {
    FileEntry fe;
    memset(&fe, 0, sizeof(fe));
    size_t plen = strlen(parent_path);
    fe.path = xmalloc(plen + 1 + name_len + 1);
    memcpy(fe.path, parent_path, plen);
    fe.path[plen] = '/';
    memcpy(fe.path + plen + 1, name, name_len);
    fe.path[plen + 1 + name_len] = '\0';
    fe.name = fe.path + plen + 1;
    fe.type = is_dir ? FTYPE_DIR : FTYPE_FILE;
    fe.is_ghost = 1;
    fe.size = -1;
    fe.line_count = -1;
    fe.word_count = -1;
    fe.file_count = -1;
    file_list_add(list, &fe);
}

/* Inject ghost children for git-deleted paths under parent that the scan
 * could not see: a deleted file becomes a ghost row, and a fully deleted
 * directory becomes a ghost directory whose own materialize (the is_ghost
 * branch there) rebuilds the next level the same way. Runs before canonical
 * paths are derived and repos discovered, so ghosts flow through the same
 * annotation pipeline as scanned entries. Deleted paths whose first component
 * names a scanned entry are skipped — recursion into the real directory
 * injects them at their own level (a deleted path can also collide with a
 * recreated file of the same name; the row then shows the live file). */
static void inject_ghost_children(TreeNode *parent, FileList *list,
                                  GitCache *git) {
    const char *pabs = parent->entry.abs_path ? parent->entry.abs_path
                                              : parent->entry.path;
    GhostCollect gc = { .parent = pabs, .parent_len = strlen(pabs) };
    git_cache_foreach_change(git, ghost_collect_cb, &gc);

    for (size_t i = 0; i < gc.count; i++) {
        const char *rest = gc.paths[i] + gc.parent_len + 1;
        const char *slash = strchr(rest, '/');
        size_t name_len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (!file_list_contains_name(list, rest, name_len)) {
            ghost_entry_add(list, parent->entry.path, rest, name_len,
                            slash != NULL);
        }
        free(gc.paths[i]);
    }
    free(gc.paths);

    /* Keep the read_directory name order with ghosts interleaved. */
    if (gc.count > 0) {
        qsort(list->entries, list->count, sizeof(FileEntry), entry_cmp_name);
    }
}

/* Materialize parent's immediate children from the filesystem: read the
 * directory, populate any nested git repos it directly contains, and create and
 * git-annotate each child node. One level only — build_tree_children drives the
 * recursive descent and tree_expand_node stops here. Both the initial build and
 * interactive lazy expansion go through this, so they can't drift.
 *
 * Sets parent->was_expanded once the directory is readable. Returns a
 * caller-owned array (length parent->child_count) of per-child git-repo-root
 * flags, so a recursive caller can thread in_git_repo downward; returns NULL
 * (leaving parent with no children) when the directory is unreadable or empty. */
static int *materialize_children(TreeNode *parent, const TreeBuildOpts *opts,
                                 GitCache *git, int in_git_repo,
                                 int parent_is_ignored) {
    /* A ghost directory has nothing on disk to read: its children are
     * entirely ghosts, injected from the git cache below. */
    int ghost_parent = parent->entry.is_ghost;
    if (!ghost_parent && access(parent->entry.path, R_OK) != 0) return NULL;
    parent->was_expanded = 1;
    parent->ui_expanded = 1;

    FileList list;
    file_list_init(&list);
    if (!ghost_parent && read_directory(parent->entry.path, &list, opts) != 0) {
        file_list_free(&list);
        return NULL;
    }

    if (opts->compute.git_status) {
        inject_ghost_children(parent, &list, git);
    }
    if (list.count == 0) {
        file_list_free(&list);
        return NULL;
    }

    /* Derive canonical paths from the parent's before any git work, so cache
     * keys (populate) and lookups always agree even when the display path
     * goes through a symlink — and realpath() runs once per tree, not once
     * per entry. */
    if (parent->entry.abs_path) {
        for (size_t i = 0; i < list.count; i++) {
            FileEntry *fe = &list.entries[i];
            size_t n = strlen(parent->entry.abs_path) + 1 + strlen(fe->name) + 1;
            fe->abs_path = xmalloc(n);
            snprintf(fe->abs_path, n, "%s/%s", parent->entry.abs_path, fe->name);
        }
    }

    /* Discover and populate any git repos rooted directly at a child. Zero-init
     * the flags so the git-status-off path (find_git_repo_roots skipped) still
     * threads a defined in_git_repo downward. */
    int *is_git_repo_root = xcalloc(list.count, sizeof(int));
    size_t git_repo_count = 0;
    char **git_repos = opts->compute.git_status
        ? find_git_repo_roots(&list, is_git_repo_root, &git_repo_count)
        : NULL;
    if (git_repos) {
        /* Tasks, not a nested parallel region: the tree build runs inside one
         * region already (immediate execution when called outside one, e.g.
         * interactive expansion). */
        for (size_t i = 0; i < git_repo_count; i++) {
            const char *repo = git_repos[i];
            #pragma omp task firstprivate(repo)
            git_populate_repo(git, repo, opts->compute.git_diff, in_git_repo, opts->git_base);
        }
        #pragma omp taskwait
        free(git_repos);
    }

    parent->children = xmalloc(list.count * sizeof(TreeNode));
    parent->child_count = list.count;

    for (size_t i = 0; i < list.count; i++) {
        TreeNode *child = &parent->children[i];
        memset(child, 0, sizeof(TreeNode));
        child->entry = list.entries[i];
        /* Mark mount boundaries (different filesystem than parent) */
        child->entry.is_mount_point = !child->entry.is_ghost &&
                                      (child->entry.dev != parent->entry.dev);

        if (opts->compute.git_status) {
            apply_git_status(&child->entry, git, opts->compute.git_diff);
        }

        child->entry.is_ignored = parent_is_ignored ||
                                   (child->entry.git_flags & GITF_IGNORED) ||
                                   path_name_is_opaque(child->entry.name);

        /* Check if directory has .gitignore with "*" (ignores all contents) */
        if (!child->entry.is_ignored && node_is_directory(child)) {
            child->entry.is_ignored = has_ignore_all_gitignore(child->entry.path);
        }

        /* Record git-status presence so git-only filtering works during
         * interactive expansion, where the post-build compute_git_status_flags
         * pass doesn't re-run. The static path recomputes this authoritatively,
         * so setting it here is harmless there. Don't bubble across repo
         * boundaries — only propagate when the parent is itself inside a repo. */
        if (GITF_IS_CHANGE(child->entry.git_flags)) {
            child->has_git_status = 1;
            if (in_git_repo) parent->has_git_status = 1;
        }
    }

    free(list.entries);
    return is_git_repo_root;
}

/* Frontier directory: one that will not be materialized (depth limit,
 * hidden without -a, opaque, gitignore-skipped, --min-size pruned, or
 * unreadable). Its stats come from the scanner — the only place the tree
 * build still walks anything twice-removed. Uses the task-based scanner
 * core since we are inside the build's parallel region. */
static void compute_dir_stats_frontier(FileEntry *fe, const ComputeOpts *c) {
    DirStats stats = get_dir_stats_cached_tasks(fe->path);
    if (c->sizes) fe->size = stats.size;
    if (c->file_counts) fe->file_count = stats.file_count;
}

/* One entry's contribution to its parent directory's totals, mirroring the
 * scanner's accounting exactly: allocated blocks, symlinks count as files
 * (macOS also adds the link's own blocks; Linux does not — matching
 * scan_impl's platform split), directories contribute their computed totals
 * (opaque ones have -1 counts and are excluded), and devices/sockets/fifos
 * are not counted. */
static void entry_scan_contribution(const FileEntry *fe, off_t *size, long *count) {
    if (fe->is_ghost) return;  /* nothing on disk to account for */
    switch (fe->type) {
        case FTYPE_SYMLINK:
        case FTYPE_SYMLINK_DIR:
        case FTYPE_SYMLINK_EXEC:
        case FTYPE_SYMLINK_DEVICE:
        case FTYPE_SYMLINK_SOCKET:
        case FTYPE_SYMLINK_FIFO:
        case FTYPE_SYMLINK_BROKEN:
#ifdef __APPLE__
            *size += fe->alloc_size;
#endif
            (*count)++;
            break;
        case FTYPE_DIR:
            if (fe->size >= 0) *size += fe->size;
            if (fe->file_count >= 0) *count += fe->file_count;
            break;
        case FTYPE_FILE:
        case FTYPE_EXEC:
            *size += fe->alloc_size;
            (*count)++;
            break;
        default:
            break;  /* devices, sockets, fifos: not counted by the scanner */
    }
}

/* Firmlink accounting for a freshly summed directory total: an endpoint
 * defines the pair's memoized stats the same way a scanned one does, pairs
 * whose two endpoints meet at this directory are measured if no walk reached
 * them (e.g. both children came out of the size cache), and the double count
 * is subtracted once. in_parallel selects the scanner entry point for the
 * measurement fallback. */
static void apply_firmlink_accounting(const char *abs, off_t *size, long *count,
                                      int in_parallel) {
    int pair = firmlink_lookup_path(abs);
    if (pair >= 0) firmlink_stats_offer(pair, *size, *count);

    for (int i = 0; i < firmlink_count(); i++) {
        off_t ps;
        long pc;
        if (strcmp(firmlink_lca(i), abs) != 0) continue;
        if (!firmlink_stats_get(i, &ps, &pc)) {
            DirStats ds = in_parallel ? get_dir_stats_cached_tasks(firmlink_alias(i))
                                      : get_dir_stats_cached(firmlink_alias(i));
            if (ds.size >= 0) firmlink_stats_offer(i, ds.size, ds.file_count);
        }
    }
    firmlink_adjust_dir(abs, size, count);
}

/* Sum a materialized directory's stats from its (final) children. */
static void dir_stats_from_children(TreeNode *dir, const ComputeOpts *c) {
    off_t size = dir->entry.alloc_size;
    long count = 0;

    for (size_t i = 0; i < dir->child_count; i++) {
        entry_scan_contribution(&dir->children[i].entry, &size, &count);
    }

    const char *abs = dir->entry.abs_path ? dir->entry.abs_path : dir->entry.path;
    apply_firmlink_accounting(abs, &size, &count, 1);

    if (c->sizes) dir->entry.size = size;
    if (c->file_counts) dir->entry.file_count = count;
}

/* A recursed directory's stats once its subtree is complete: bottom-up sum
 * when it materialized, scanner fallback when it turned out unreadable. */
static void finalize_dir_stats(TreeNode *dir, const ComputeOpts *c) {
    if (dir->entry.is_ghost) return;  /* keep the -1 "not computed" defaults */
    if (dir->was_expanded) {
        dir_stats_from_children(dir, c);
    } else {
        compute_dir_stats_frontier(&dir->entry, c);
    }
}

static void build_tree_children(TreeNode *parent, int depth,
                                 const TreeBuildOpts *opts, GitCache *git,
                                 int in_git_repo, int parent_is_ignored) {
    if (depth >= opts->max_depth) return;

    int *is_git_repo_root = materialize_children(parent, opts, git, in_git_repo,
                                                 parent_is_ignored);
    if (!is_git_repo_root) return;  /* unreadable or empty: nothing to recurse */

    const ComputeOpts *c = &opts->compute;
    int is_virtual = path_is_virtual_fs(parent->entry.path);
    int want_dir_stats = (c->sizes || c->file_counts) && !is_virtual;
    int want_content = (c->line_counts || c->media_info) && !is_virtual;

    /* --min-size prunes recursion on a directory's total size, which must
     * then be known before descending: scan every child directory eagerly
     * and keep those totals (the pre-Phase-5 cost model, for this flag
     * only). Otherwise materialized directories are summed bottom-up. */
    int eager_stats = (opts->skip_fn != NULL) && want_dir_stats;
    if (eager_stats) {
        for (size_t i = 0; i < parent->child_count; i++) {
            TreeNode *child = &parent->children[i];
            if (!node_is_directory(child) || child->entry.is_ghost) continue;
            #pragma omp task firstprivate(child)
            compute_dir_stats_frontier(&child->entry, c);
        }
        #pragma omp taskwait
    }

    for (size_t i = 0; i < parent->child_count; i++) {
        TreeNode *child = &parent->children[i];

        if (node_is_directory(child)) {
            /* Don't descend into hidden directories unless -a: their entries
             * are kept in the tree, but scanning their contents (e.g. .git,
             * .cache) is wasteful and their git status is read from the
             * cache by path. */
            int skip_hidden_dir = !opts->show_hidden && child->entry.name[0] == '.';
            /* Repos nested inside this repo (submodules, vendored checkouts)
             * render as normal entries but their contents are foreign — skip
             * them alongside gitignored dirs; -e descends into both. */
            int skip_nested_repo = in_git_repo && is_git_repo_root[i] &&
                                   opts->skip_gitignored;
            int recurse = !skip_hidden_dir && !skip_nested_repo &&
                !should_skip_dir(child->entry.name, child->entry.is_ignored,
                                 opts->skip_gitignored) &&
                !(opts->skip_fn && opts->skip_fn(&child->entry, opts->skip_ctx));

            if (recurse) {
                int child_in_git_repo = in_git_repo || is_git_repo_root[i];
                int child_ignored = child->entry.is_ignored;
                #pragma omp task firstprivate(child, child_in_git_repo, child_ignored)
                {
                    build_tree_children(child, depth + 1, opts, git,
                                        child_in_git_repo, child_ignored);
                    if (!eager_stats && want_dir_stats) {
                        finalize_dir_stats(child, c);
                    }
                }
            } else if (!eager_stats && want_dir_stats && !child->entry.is_ghost) {
                #pragma omp task firstprivate(child)
                compute_dir_stats_frontier(&child->entry, c);
            }
        } else if (want_content) {
            int is_file = (child->entry.type == FTYPE_FILE ||
                           child->entry.type == FTYPE_EXEC ||
                           child->entry.type == FTYPE_SYMLINK ||
                           child->entry.type == FTYPE_SYMLINK_EXEC);
            if (is_file && !child->entry.is_ghost) {
                #pragma omp task firstprivate(child)
                fileinfo_compute_content(&child->entry, c);
            }
        }
    }
    #pragma omp taskwait

    order_children(parent, opts);
    free(is_git_repo_root);
}

TreeNode *build_tree(const char *path, const TreeBuildOpts *opts,
                     GitCache *git, const Icons *icons) {
    (void)icons;  /* Reserved for future use */

    char abs_path[PATH_MAX];
    if (opts->cwd) {
        path_get_abspath(path, abs_path, opts->cwd);
    } else {
        strncpy(abs_path, path, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    }

    char git_root[PATH_MAX];
    int in_git_repo = git_find_root(abs_path, git_root, sizeof(git_root));
    if (in_git_repo && opts->compute.git_status) {
        git_populate_repo(git, git_root, opts->compute.git_diff, 0, opts->git_base);
    }

    TreeNode *root = xmalloc(sizeof(TreeNode));
    memset(root, 0, sizeof(TreeNode));

    int is_virtual_fs = path_is_virtual_fs(abs_path);
    file_entry_init(&root->entry, abs_path, is_virtual_fs);
    /* Resolve the root once; children derive their canonical paths by appending
     * their names, so realpath() runs once per tree instead of once per entry. */
    char root_real[PATH_MAX];
    path_get_realpath(abs_path, root_real, opts->cwd);
    root->entry.abs_path = xstrdup(root_real);

    int is_dir = node_is_directory(root);
    /* Files get their content metadata now; directory stats wait until the
     * subtree is built (bottom-up) or proven frontier (scan) below. */
    if (!is_dir) {
        file_entry_compute(&root->entry, &opts->compute, is_virtual_fs);
    }

    if (opts->compute.git_status) {
        apply_git_status(&root->entry, git, opts->compute.git_diff);
    }

    root->entry.is_ignored = (root->entry.git_flags & GITF_IGNORED) ||
                              path_name_is_opaque(root->entry.name) ||
                              (in_git_repo && git_path_in_ignored(git, abs_path, git_root));

    /* Check if directory has .gitignore with "*" (ignores all contents) */
    if (!root->entry.is_ignored && is_dir) {
        root->entry.is_ignored = has_ignore_all_gitignore(abs_path);
    }

    if (is_dir && in_git_repo && strcmp(abs_path, git_root) == 0) {
        annotate_git_root(&root->entry);
    }

    if (is_dir) {
        /* One parallel region for the whole build: recursion, per-file
         * content, git populates, and frontier scans all run as tasks
         * inside it (nested regions would be serialized by the runtime). */
        #pragma omp parallel
        #pragma omp single
        build_tree_children(root, 0, opts, git, in_git_repo, root->entry.is_ignored);

        if ((opts->compute.sizes || opts->compute.file_counts) && !is_virtual_fs) {
            if (root->was_expanded && !opts->skip_fn) {
                dir_stats_from_children(root, &opts->compute);
            } else {
                /* Depth 0, unreadable, or --min-size (eager) mode: whole-tree
                 * scan, with its own parallel region (we are outside ours). */
                file_entry_compute(&root->entry, &opts->compute, is_virtual_fs);
            }
        }
    }

    return root;
}

void tree_expand_node(TreeNode *node, const TreeBuildOpts *opts,
                      GitCache *git, const Icons *icons) {
    (void)icons;

    if (node->child_count > 0) return;
    if (!node_is_directory(node)) return;

    /* The enclosing repo was already populated when this node was created (in
     * build_tree, or by a parent's materialize_children), so we don't re-scan
     * it here; find_git_repo_roots inside materialize_children still discovers
     * and populates any nested repo first seen at this level. */
    char git_root[PATH_MAX];
    int in_git_repo = opts->compute.git_status &&
                      git_find_root(node->entry.path, git_root, sizeof(git_root));

    int *is_git_repo_root = materialize_children(node, opts, git, in_git_repo,
                                                 node->entry.is_ignored);
    if (!is_git_repo_root) return;
    free(is_git_repo_root);

    /* One level only (interactive expansion doesn't recurse), so every child
     * is a leaf from this call's perspective: compute file content and
     * directory totals directly, in parallel. */
    const ComputeOpts *c = &opts->compute;
    int is_virtual = path_is_virtual_fs(node->entry.path);
    if (!is_virtual && (c->sizes || c->file_counts || c->line_counts || c->media_info)) {
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < node->child_count; i++) {
            if (node->children[i].entry.is_ghost) continue;
            file_entry_compute(&node->children[i].entry, c, is_virtual);
        }
    }

    order_children(node, opts);
}

/* ============================================================================
 * Ancestry Tree Building
 * ============================================================================ */

/* Build a single ancestor node (directory only, no children yet). Stats are
 * not computed here: ancestor totals come from ancestor_compute_stats once
 * the chain child's total is known. */
static TreeNode *build_ancestor_node(const char *path, const TreeBuildOpts *opts) {
    (void)opts;
    TreeNode *node = xmalloc(sizeof(TreeNode));
    memset(node, 0, sizeof(TreeNode));

    int is_virtual_fs = path_is_virtual_fs(path);
    file_entry_init(&node->entry, path, is_virtual_fs);

    /* Special case for root */
    if (node->entry.path[0] == '/' && node->entry.path[1] == '\0') {
        node->entry.name = "/";
    }

    if (node_is_directory(node) && path_is_git_root(path)) {
        annotate_git_root(&node->entry);
    }

    return node;
}

/* An ancestry-chain node's totals. Only the chain child is ever materialized
 * (siblings are not adopted as tree nodes — the -p display shows just the
 * spine), so bottom-up summing over children can't apply. Instead, scan each
 * sibling once and reuse the chain child's already-computed total: across
 * the whole chain every subtree is walked exactly once, where each ancestor
 * previously re-scanned the entire descendant subtree per level. */
static void ancestor_compute_stats(TreeNode *node, const TreeNode *chain_child,
                                   const TreeBuildOpts *opts) {
    const ComputeOpts *c = &opts->compute;
    if (!(c->sizes || c->file_counts)) return;
    if (path_is_virtual_fs(node->entry.path)) return;

    FileList list;
    file_list_init(&list);
    if (read_directory(node->entry.path, &list, opts) != 0) {
        /* Unreadable: same -1 result the whole-subtree scan would produce */
        file_entry_compute(&node->entry, c, 0);
        return;
    }

    off_t size = node->entry.alloc_size;
    long count = 0;
    for (size_t i = 0; i < list.count; i++) {
        FileEntry *fe = &list.entries[i];

        /* The chain child's subtree is already totalled; a symlinked chain
         * component still contributes as a link, matching the scanner. */
        if (fe->type == FTYPE_DIR &&
            strcmp(fe->name, chain_child->entry.name) == 0) {
            if (chain_child->entry.size >= 0) size += chain_child->entry.size;
            if (chain_child->entry.file_count >= 0) count += chain_child->entry.file_count;
            continue;
        }

        if (fe->type == FTYPE_DIR) {
            DirStats ds = get_dir_stats_cached(fe->path);
            fe->size = ds.size;
            fe->file_count = ds.file_count;
        }
        entry_scan_contribution(fe, &size, &count);
    }
    file_list_free(&list);

    apply_firmlink_accounting(node->entry.path, &size, &count, 0);

    if (c->sizes) node->entry.size = size;
    if (c->file_counts) node->entry.file_count = count;
}

/* Move a heap-allocated node's contents into parent's (single) child slot and
 * free the shell. Returns the adopted slot. */
static TreeNode *tree_node_adopt_single(TreeNode *parent, TreeNode *child) {
    parent->children = xmalloc(sizeof(TreeNode));
    parent->children[0] = *child;   /* shallow move: slot now owns contents */
    parent->child_count = 1;
    parent->was_expanded = 1;
    parent->ui_expanded = 1;
    free(child);                    /* shell only; contents live on in the slot */
    return &parent->children[0];
}

TreeNode *build_ancestry_tree(const char *path, const TreeBuildOpts *opts,
                               GitCache *git, const Icons *icons) {
    char abs_path[PATH_MAX];

    /* For ancestry, preserve symlinks in path by using $PWD for relative paths */
    if (path[0] == '/') {
        path_get_abspath(path, abs_path, "/");
    } else {
        const char *pwd = getenv("PWD");
        if (pwd && pwd[0] == '/') {
            path_get_abspath(path, abs_path, pwd);
        } else if (opts->cwd) {
            path_get_abspath(path, abs_path, opts->cwd);
        } else {
            strncpy(abs_path, path, sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        }
    }

    /* Determine base path: home if the path is under it, else /. */
    const char *home = getenv("HOME");
    const char *base = "/";
    size_t base_len = 1;

    if (home && home[0] && strncmp(abs_path, home, strlen(home)) == 0) {
        char after = abs_path[strlen(home)];
        if (after == '\0' || after == '/') {
            base = home;
            base_len = strlen(home);
        }
    }

    /* If path equals the base, just build a normal tree */
    if (strcmp(abs_path, base) == 0) {
        return build_tree(path, opts, git, icons);
    }

    /* Build list of path components from base to target */
    char *components[PATH_MAX / 2];
    int comp_count = 0;

    const char *p = abs_path + base_len;
    if (*p == '/') p++;

    char path_so_far[PATH_MAX];
    strncpy(path_so_far, base, sizeof(path_so_far) - 1);
    path_so_far[sizeof(path_so_far) - 1] = '\0';

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);

        if (comp_len > 0) {
            size_t path_len = strlen(path_so_far);
            size_t remaining = sizeof(path_so_far) - path_len - 1;

            if (remaining > 0 && path_so_far[path_len - 1] != '/') {
                path_so_far[path_len++] = '/';
                path_so_far[path_len] = '\0';
                remaining--;
            }

            if (remaining > 0) {
                size_t to_copy = comp_len < remaining ? comp_len : remaining;
                memcpy(path_so_far + path_len, p, to_copy);
                path_so_far[path_len + to_copy] = '\0';
            }

            components[comp_count++] = xstrdup(path_so_far);
        }

        if (!slash) break;
        p = slash + 1;
    }

    /* Build the root node (repo root, home, or /) */
    TreeNode *root = build_ancestor_node(base, opts);

    /* Build the chain of ancestors, keeping the spine for the stats pass */
    TreeNode **chain = xmalloc((comp_count + 1) * sizeof(TreeNode *));
    chain[0] = root;
    TreeNode *current = root;
    for (int i = 0; i < comp_count; i++) {
        TreeNode *child;

        if (i == comp_count - 1) {
            /* This is the target - build it with full tree */
            child = build_tree(components[i], opts, git, icons);
        } else {
            /* This is an ancestor - just a skeleton node */
            child = build_ancestor_node(components[i], opts);
        }

        current = tree_node_adopt_single(current, child);
        current->is_ancestor = 1;
        chain[i + 1] = current;
        free(components[i]);
    }

    /* Ancestor totals, bottom-up from the target (whose stats build_tree
     * already computed): each level scans only the chain child's siblings. */
    for (int i = comp_count - 1; i >= 0; i--) {
        ancestor_compute_stats(chain[i], chain[i + 1], opts);
    }
    free(chain);

    return root;
}

/* ============================================================================
 * Tree Traversal Helpers
 * ============================================================================ */

int compute_git_status_flags(TreeNode *node, GitCache *git) {
    /* A node counts as "changed" for -m using the same signal as its rendered
     * git icon, so visibility never disagrees with what's drawn and stays
     * correct at any depth: a file uses its own status; a directory uses the
     * cache roll-up over its whole subtree (git_get_dir_summary — exactly what
     * fileinfo_compute_git_dir_status draws), not a walk of the built children,
     * which would miss modifications below the built depth. */
    int result = GITF_IS_CHANGE(node->entry.git_flags);

    if (!result && node_is_directory(node)) {
        /* Keyed by the canonical path, as the cache is (see apply_git_status). */
        const char *abs = node->entry.abs_path ? node->entry.abs_path
                                               : node->entry.path;
        GitSummary s = git_get_dir_summary(git, abs);
        result = s.modified || s.untracked || s.staged ||
                 s.deleted || s.staged_deleted;
    }
    node->has_git_status = result;

    for (size_t i = 0; i < node->child_count; i++) {
        compute_git_status_flags(&node->children[i], git);
    }
    return result;
}

static int is_glob_pattern(const char *s) {
    return (strchr(s, '*') || strchr(s, '?') || strchr(s, '['));
}

/* Smart-case (like vim): a pattern containing an uppercase letter matches
 * case-sensitively, otherwise case-insensitively. */
static int pattern_has_uppercase(const char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') return 1;
    }
    return 0;
}

int compute_grep_flags(TreeNode *node, const char *pattern) {
    int case_sensitive = pattern_has_uppercase(pattern);
    const char *name = node->entry.name;
    int result = is_glob_pattern(pattern)
        ? (fnmatch(pattern, name, case_sensitive ? 0 : FNM_CASEFOLD) == 0)
        : ((case_sensitive ? strstr(name, pattern)
                           : strcasestr(name, pattern)) != NULL);

    for (size_t i = 0; i < node->child_count; i++) {
        if (compute_grep_flags(&node->children[i], pattern)) {
            result = 1;
        }
    }

    node->matches_grep = result;
    return result;
}

