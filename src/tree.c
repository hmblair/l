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
    fe->type = detect_file_type(fe->path, &st, &fe->symlink_target);
    fe->mode = st.st_mode;
    fe->dev = st.st_dev;
    fe->mtime = GET_MTIME(st);
    fe->size = is_virtual_fs ? -1 : st.st_size;
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
        fileinfo_compute_content(fe, c->line_counts, c->media_info);
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

static int entry_cmp_size(const void *a, const void *b) {
    const FileEntry *ea = a, *eb = b;
    if (eb->size > ea->size) return 1;
    if (eb->size < ea->size) return -1;
    return 0;
}

static int entry_cmp_time(const void *a, const void *b) {
    const FileEntry *ea = a, *eb = b;
    if (eb->mtime > ea->mtime) return 1;
    if (eb->mtime < ea->mtime) return -1;
    return 0;
}

static void reverse_file_list(FileList *list) {
    for (size_t i = 0; i < list->count / 2; i++) {
        FileEntry tmp = list->entries[i];
        list->entries[i] = list->entries[list->count - 1 - i];
        list->entries[list->count - 1 - i] = tmp;
    }
}

static void sort_file_list(FileList *list, SortMode mode, int reverse) {
    if (list->count == 0) return;

    int (*cmp)(const void *, const void *) = NULL;
    switch (mode) {
        case SORT_NAME: cmp = entry_cmp_name; break;
        case SORT_SIZE: cmp = entry_cmp_size; break;
        case SORT_TIME: cmp = entry_cmp_time; break;
        default: return;
    }

    qsort(list->entries, list->count, sizeof(FileEntry), cmp);
    if (reverse) reverse_file_list(list);
}

/* ============================================================================
 * Directory Reading
 * ============================================================================ */

int read_directory(const char *dir_path, FileList *list,
                   const TreeBuildOpts *opts) {
    DIR *dir = opendir(dir_path);
    if (!dir) return -1;

    int is_virtual_fs = path_is_virtual_fs(dir_path);
    const ComputeOpts *c = &opts->compute;

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

    /* Compute metadata in parallel if requested */
    int need_parallel = !is_virtual_fs &&
        (c->sizes || c->file_counts || c->line_counts || c->media_info);
    if (list->count > 0 && need_parallel) {
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < list->count; i++) {
            file_entry_compute(&list->entries[i], c, is_virtual_fs);
        }
    }

    /* Sort */
    qsort(list->entries, list->count, sizeof(FileEntry), entry_cmp_name);
    if (opts->sort_by != SORT_NONE && opts->sort_by != SORT_NAME) {
        sort_file_list(list, opts->sort_by, opts->sort_reverse);
    } else if (opts->sort_reverse) {
        reverse_file_list(list);
    }

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
    GitStatusNode *git_node = git_cache_get_node(git, fe->path);
    if (git_node) {
        strncpy(fe->git_status, git_node->status, sizeof(fe->git_status) - 1);
        fe->git_status[sizeof(fe->git_status) - 1] = '\0';
        if (compute_diff) {
            fe->diff_added = git_node->lines_added;
            fe->diff_removed = git_node->lines_removed;
        }
    }
}

/* Find git repo roots in a file list and mark them.
 * Returns array of repo paths to populate (caller must free).
 * Sets is_git_repo_root[i] and is_submodule[i] for each entry. */
static char **find_git_repo_roots(FileList *list, int in_git_repo,
                                   int *is_git_repo_root, int *is_submodule,
                                   size_t *out_count) {
    char **git_repos = NULL;
    size_t count = 0;

    for (size_t i = 0; i < list->count; i++) {
        is_git_repo_root[i] = 0;
        is_submodule[i] = 0;
        FileEntry *fe = &list->entries[i];
        if ((fe->type == FTYPE_DIR || fe->type == FTYPE_SYMLINK_DIR) &&
            strcmp(fe->name, ".git") != 0 && path_is_git_root(fe->path)) {
            is_git_repo_root[i] = 1;
            fe->is_git_root = 1;
            fe->remote = git_get_remote_url(fe->path);
            fe->tag = git_get_latest_tag(fe->path);
            if (in_git_repo) {
                is_submodule[i] = 1;
            } else {
                git_repos = xrealloc(git_repos, (count + 1) * sizeof(char *));
                git_repos[count++] = fe->path;
            }
        }
    }

    *out_count = count;
    return git_repos;
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
    if (access(parent->entry.path, R_OK) != 0) return NULL;
    parent->was_expanded = 1;

    FileList list;
    file_list_init(&list);
    if (read_directory(parent->entry.path, &list, opts) != 0 || list.count == 0) {
        file_list_free(&list);
        return NULL;
    }

    /* Discover and populate any git repos rooted directly at a child. Zero-init
     * the flags so the git-status-off path (find_git_repo_roots skipped) still
     * threads a defined in_git_repo downward. */
    int *is_git_repo_root = xcalloc(list.count, sizeof(int));
    int *is_submodule = xcalloc(list.count, sizeof(int));
    size_t git_repo_count = 0;
    char **git_repos = opts->compute.git_status
        ? find_git_repo_roots(&list, in_git_repo, is_git_repo_root, is_submodule, &git_repo_count)
        : NULL;
    if (git_repos) {
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < git_repo_count; i++) {
            git_populate_repo(git, git_repos[i], opts->compute.git_diff);
        }
        free(git_repos);
    }

    parent->children = xmalloc(list.count * sizeof(TreeNode));
    parent->child_count = list.count;

    for (size_t i = 0; i < list.count; i++) {
        TreeNode *child = &parent->children[i];
        memset(child, 0, sizeof(TreeNode));
        child->entry = list.entries[i];
        /* Mark mount boundaries (different filesystem than parent) */
        child->entry.is_mount_point = (child->entry.dev != parent->entry.dev);
        /* Derive canonical path from the parent's (no realpath syscall). */
        if (parent->entry.abs_path) {
            size_t n = strlen(parent->entry.abs_path) + 1 + strlen(child->entry.name) + 1;
            child->entry.abs_path = xmalloc(n);
            snprintf(child->entry.abs_path, n, "%s/%s", parent->entry.abs_path, child->entry.name);
        }

        if (opts->compute.git_status) {
            apply_git_status(&child->entry, git, opts->compute.git_diff);
        }

        const char *git_status = child->entry.git_status[0] ? child->entry.git_status : NULL;
        child->entry.is_ignored = parent_is_ignored ||
                                   (git_status && strcmp(git_status, "!!") == 0) ||
                                   path_name_is_opaque(child->entry.name) ||
                                   is_submodule[i];

        /* Check if directory has .gitignore with "*" (ignores all contents) */
        if (!child->entry.is_ignored && node_is_directory(child)) {
            child->entry.is_ignored = has_ignore_all_gitignore(child->entry.path);
        }

        /* Record git-status presence so git-only filtering works during
         * interactive expansion, where the post-build compute_git_status_flags
         * pass doesn't re-run. The static path recomputes this authoritatively,
         * so setting it here is harmless there. Don't bubble across repo
         * boundaries — only propagate when the parent is itself inside a repo. */
        if (git_status && strcmp(git_status, "!!") != 0) {
            child->has_git_status = 1;
            if (in_git_repo) parent->has_git_status = 1;
        }
    }

    free(is_submodule);
    free(list.entries);
    return is_git_repo_root;
}

static void build_tree_children(TreeNode *parent, int depth,
                                 const TreeBuildOpts *opts, GitCache *git,
                                 int in_git_repo, int parent_is_ignored) {
    if (depth >= opts->max_depth) return;

    int *is_git_repo_root = materialize_children(parent, opts, git, in_git_repo,
                                                 parent_is_ignored);
    if (!is_git_repo_root) return;  /* unreadable or empty: nothing to recurse */

    for (size_t i = 0; i < parent->child_count; i++) {
        TreeNode *child = &parent->children[i];

        /* Don't descend into hidden directories unless -a: their entries are
         * kept in the tree, but scanning their contents (e.g. .git, .cache) is
         * wasteful and their git status is read from the cache by path. */
        int skip_hidden_dir = !opts->show_hidden && child->entry.name[0] == '.';
        if (node_is_directory(child) && !skip_hidden_dir &&
            !should_skip_dir(child->entry.name, child->entry.is_ignored, opts->skip_gitignored) &&
            !(opts->skip_fn && opts->skip_fn(&child->entry, opts->skip_ctx))) {
            int child_in_git_repo = in_git_repo || is_git_repo_root[i];
            build_tree_children(child, depth + 1, opts, git, child_in_git_repo, child->entry.is_ignored);
        }
    }

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
        git_populate_repo(git, git_root, opts->compute.git_diff);
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
    file_entry_compute(&root->entry, &opts->compute, is_virtual_fs);

    if (opts->compute.git_status) {
        apply_git_status(&root->entry, git, opts->compute.git_diff);
    }

    root->entry.is_ignored = (root->entry.git_status[0] && strcmp(root->entry.git_status, "!!") == 0) ||
                              path_name_is_opaque(root->entry.name) ||
                              (in_git_repo && git_path_in_ignored(git, abs_path, git_root));

    /* Check if directory has .gitignore with "*" (ignores all contents) */
    if (!root->entry.is_ignored && is_dir) {
        root->entry.is_ignored = has_ignore_all_gitignore(abs_path);
    }

    if (is_dir && in_git_repo && strcmp(abs_path, git_root) == 0) {
        root->entry.is_git_root = 1;
        root->entry.remote = git_get_remote_url(abs_path);
        root->entry.tag = git_get_latest_tag(abs_path);
    }

    if (is_dir) {
        build_tree_children(root, 0, opts, git, in_git_repo, root->entry.is_ignored);
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
    free(is_git_repo_root);
}

void tree_rescan_node(TreeNode *node, const TreeBuildOpts *opts,
                      GitCache *git, const Icons *icons) {
    if (!node_is_directory(node)) return;

    /* Free existing children */
    for (size_t i = 0; i < node->child_count; i++) {
        tree_node_free(&node->children[i]);
    }
    free(node->children);
    node->children = NULL;
    node->child_count = 0;
    node->was_expanded = 0;

    /* Re-expand */
    tree_expand_node(node, opts, git, icons);
}

/* ============================================================================
 * Ancestry Tree Building
 * ============================================================================ */

/* Build a single ancestor node (directory only, no children yet) */
static TreeNode *build_ancestor_node(const char *path, const TreeBuildOpts *opts) {
    TreeNode *node = xmalloc(sizeof(TreeNode));
    memset(node, 0, sizeof(TreeNode));

    int is_virtual_fs = path_is_virtual_fs(path);
    file_entry_init(&node->entry, path, is_virtual_fs);

    /* Special case for root */
    if (node->entry.path[0] == '/' && node->entry.path[1] == '\0') {
        node->entry.name = "/";
    }

    file_entry_compute(&node->entry, &opts->compute, is_virtual_fs);

    if (node_is_directory(node) && path_is_git_root(path)) {
        node->entry.is_git_root = 1;
        node->entry.remote = git_get_remote_url(path);
        node->entry.tag = git_get_latest_tag(path);
    }

    return node;
}

/* Move a heap-allocated node's contents into parent's (single) child slot and
 * free the shell. Returns the adopted slot. */
static TreeNode *tree_node_adopt_single(TreeNode *parent, TreeNode *child) {
    parent->children = xmalloc(sizeof(TreeNode));
    parent->children[0] = *child;   /* shallow move: slot now owns contents */
    parent->child_count = 1;
    parent->was_expanded = 1;
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

    /* Determine base path. With -g, anchor at the enclosing git repo root
     * (found by walking up from the target); otherwise home if the path is under
     * it, else /. */
    const char *home = getenv("HOME");
    const char *base = "/";
    size_t base_len = 1;
    char repo_base[PATH_MAX];

    if (opts->ancestry_to_repo) {
        strncpy(repo_base, abs_path, sizeof(repo_base) - 1);
        repo_base[sizeof(repo_base) - 1] = '\0';
        while (!path_is_git_root(repo_base)) {
            char *slash = strrchr(repo_base, '/');
            if (!slash || slash == repo_base) { repo_base[0] = '\0'; break; }
            *slash = '\0';
        }
        if (repo_base[0]) {
            base = repo_base;
            base_len = strlen(repo_base);
        }
    } else if (home && home[0] && strncmp(abs_path, home, strlen(home)) == 0) {
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

    /* Build the chain of ancestors */
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
        free(components[i]);
    }

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
    int result = (node->entry.git_status[0] != '\0' &&
                  strcmp(node->entry.git_status, "!!") != 0);

    if (!result && node_is_directory(node)) {
        GitSummary s = git_get_dir_summary(git, node->entry.path);
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

