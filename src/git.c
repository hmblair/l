/*
 * git.c - Git status cache and retrieval implementation
 */

#include "git.h"
#include "fileinfo.h"
#include <ctype.h>

#ifdef HAVE_LIBGIT2
#include <git2.h>
#endif

/* Scanf format for paths: width is PATH_MAX-1 to leave room for null terminator */
#define PATH_SCANF_WIDTH 4095
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define SCANF_PATH "%" TOSTRING(PATH_SCANF_WIDTH) "[^\n]"

/* ============================================================================
 * Status Classification
 * ============================================================================ */

unsigned git_flags_from_porcelain(const char *status) {
    if (!status || !status[0]) return 0;
    if (strcmp(status, "!!") == 0) return GITF_IGNORED;
    if (strcmp(status, "??") == 0) return GITF_UNTRACKED;

    unsigned flags = 0;
    if (status[0] == 'D') flags |= GITF_STAGED_DELETED;
    else if (status[0] != ' ' && status[0] != '?' && status[0] != '!') flags |= GITF_STAGED;

    if (status[1] == 'M') flags |= GITF_WT_MODIFIED;
    else if (status[1] == 'D') flags |= GITF_WT_DELETED;
    else if (status[1] == 'R') flags |= GITF_WT_RENAMED;
    else if (status[1] == 'T') flags |= GITF_WT_TYPECHANGE;

    return flags;
}

/* ============================================================================
 * GitCache Functions
 * ============================================================================ */

void git_cache_init(GitCache *cache) {
    memset(cache->buckets, 0, sizeof(cache->buckets));
    memset(cache->agg_buckets, 0, sizeof(cache->agg_buckets));
    cache->repo_root_count = 0;
#ifdef _OPENMP
    omp_init_lock(&cache->lock);
#endif
}

static void git_cache_register_root(GitCache *cache, const char *repo_path) {
    /* Guard the shared repo_roots array: git_populate_repo (and hence this
     * function) runs concurrently across OpenMP threads, so the duplicate
     * scan, the bounds check, and the count increment must be atomic. */
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    /* Don't add duplicates */
    for (int i = 0; i < cache->repo_root_count; i++) {
        if (strcmp(cache->repo_roots[i], repo_path) == 0) {
#ifdef _OPENMP
            omp_unset_lock(&cache->lock);
#endif
            return;
        }
    }
    if (cache->repo_root_count < L_MAX_GIT_ROOTS) {
        cache->repo_roots[cache->repo_root_count++] = strdup(repo_path);
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

/* Check if dir_path is inside (or equal to) any known git repo root */
static int git_cache_path_in_repo(GitCache *cache, const char *dir_path) {
    for (int i = 0; i < cache->repo_root_count; i++) {
        size_t root_len = strlen(cache->repo_roots[i]);
        if (strncmp(dir_path, cache->repo_roots[i], root_len) == 0 &&
            (dir_path[root_len] == '/' || dir_path[root_len] == '\0')) {
            return 1;
        }
    }
    return 0;
}

int git_cache_path_in_any_repo(GitCache *cache, const char *path) {
    return git_cache_path_in_repo(cache, path);
}

void git_cache_foreach_change(GitCache *cache, git_change_cb cb, void *ud) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    for (int i = 0; i < L_HASH_SIZE; i++) {
        for (GitStatusNode *node = cache->buckets[i]; node; node = node->next) {
            cb(node->path, node->flags, node->lines_added, node->lines_removed, ud);
        }
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

static void git_cache_free_aggregates(GitCache *cache) {
    for (int i = 0; i < L_HASH_SIZE; i++) {
        struct GitDirAggregate *agg = cache->agg_buckets[i];
        while (agg) {
            struct GitDirAggregate *next = agg->next;
            free(agg->path);
            free(agg);
            agg = next;
        }
        cache->agg_buckets[i] = NULL;
    }
}

void git_cache_free(GitCache *cache) {
    for (int i = 0; i < L_HASH_SIZE; i++) {
        GitStatusNode *node = cache->buckets[i];
        while (node) {
            GitStatusNode *next = node->next;
            free(node->path);
            free(node);
            node = next;
        }
        cache->buckets[i] = NULL;
    }
    git_cache_free_aggregates(cache);
    for (int i = 0; i < cache->repo_root_count; i++) {
        free(cache->repo_roots[i]);
    }
    cache->repo_root_count = 0;
#ifdef _OPENMP
    omp_destroy_lock(&cache->lock);
#endif
}

void git_cache_add(GitCache *cache, const char *path, const char *status) {
    unsigned int h = hash_string(path);

#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    /* Check if already exists - skip duplicates */
    GitStatusNode *existing = cache->buckets[h];
    while (existing) {
        if (strcmp(existing->path, path) == 0) {
#ifdef _OPENMP
            omp_unset_lock(&cache->lock);
#endif
            return;  /* Already in cache */
        }
        existing = existing->next;
    }

    GitStatusNode *node = xmalloc(sizeof(GitStatusNode));
    node->path = xstrdup(path);
    node->flags = git_flags_from_porcelain(status);
    node->lines_added = 0;
    node->lines_removed = 0;
    node->next = cache->buckets[h];
    cache->buckets[h] = node;

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

unsigned git_cache_get_flags(GitCache *cache, const char *path) {
    unsigned int h = hash_string(path);

#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    unsigned result = 0;
    GitStatusNode *node = cache->buckets[h];
    while (node) {
        if (strcmp(node->path, path) == 0) {
            result = node->flags;
            break;
        }
        node = node->next;
    }

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif

    return result;
}

/* Unlocked lookup; caller holds the lock (or knows no writer can run) */
static GitStatusNode *git_cache_get_node_locked(GitCache *cache, const char *path) {
    unsigned int h = hash_string(path);
    GitStatusNode *node = cache->buckets[h];
    while (node) {
        if (strcmp(node->path, path) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

GitStatusNode *git_cache_get_node(GitCache *cache, const char *path) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    GitStatusNode *node = git_cache_get_node_locked(cache, path);
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
    /* Node fields are stable once inserted (only diff stats mutate, under the
     * lock, before the populate that triggered them returns). */
    return node;
}

void git_cache_set_diff(GitCache *cache, const char *path, int added, int removed) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    GitStatusNode *node = git_cache_get_node_locked(cache, path);
    if (node) {
        node->lines_added = added;
        node->lines_removed = removed;
    }

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

void git_cache_add_diff(GitCache *cache, const char *path, int added, int removed) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    GitStatusNode *node = git_cache_get_node_locked(cache, path);
    if (node) {
        node->lines_added += added;
        node->lines_removed += removed;
    }

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

/* Zero the diff stats of every cached path under repo_path. A single populate
 * accumulates the unstaged and staged numstat passes (git_cache_add_diff), so
 * it must start from zero to stay idempotent: git_populate_repo is invoked more
 * than once against the same repo (lazy expansion in interactive mode, and the
 * periodic git refresh), and without this reset each re-populate would double
 * the line counts. Status needs no such reset — git_cache_add overwrites it. */
static void git_reset_diff_stats(GitCache *cache, const char *repo_path) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    size_t root_len = strlen(repo_path);
    for (int i = 0; i < L_HASH_SIZE; i++) {
        for (GitStatusNode *node = cache->buckets[i]; node; node = node->next) {
            if (strncmp(node->path, repo_path, root_len) == 0 &&
                (node->path[root_len] == '/' || node->path[root_len] == '\0')) {
                node->lines_added = 0;
                node->lines_removed = 0;
            }
        }
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

/* Classify normalized status flags into a directory summary, applying them
 * with the given sign (+1 to add, -1 to remove). Single source of truth for
 * the flags -> bucket mapping: a staged deletion counts as a deletion rather
 * than a generic staged change, matching the row indicator. */
void git_summary_apply_flags(GitSummary *s, unsigned flags, int sign) {
    if (!flags || (flags & GITF_IGNORED)) return;
    if (flags & GITF_UNTRACKED) { s->untracked += sign; return; }
    if (flags & GITF_STAGED_DELETED) s->staged_deleted += sign;
    else if (flags & GITF_STAGED) s->staged += sign;
    if (flags & GITF_WT_MODIFIED) s->modified += sign;
    else if (flags & GITF_WT_DELETED) s->deleted += sign;
}

/* ---- Per-directory aggregates ------------------------------------------- */

/* Find or create the aggregate for a directory. Caller holds the lock. */
static GitSummary *git_agg_get_or_create(GitCache *cache, const char *dir) {
    unsigned int h = hash_string(dir);
    for (struct GitDirAggregate *a = cache->agg_buckets[h]; a; a = a->next) {
        if (strcmp(a->path, dir) == 0) return &a->sum;
    }
    struct GitDirAggregate *a = xmalloc(sizeof(*a));
    a->path = xstrdup(dir);
    memset(&a->sum, 0, sizeof(a->sum));
    a->next = cache->agg_buckets[h];
    cache->agg_buckets[h] = a;
    return &a->sum;
}

/* Rebuild every directory aggregate from the current status nodes. Each
 * change contributes to all ancestors that lie inside a known repo root
 * (containment is downward-closed, so the walk stops at the first ancestor
 * outside every root — the same scope the old full-cache prefix scan had via
 * its in-repo gate). Called at the end of every git_populate_repo; a global
 * wipe-and-rebuild keeps re-populates and nested repos consistent without
 * ordering concerns. */
static void git_cache_rebuild_aggregates(GitCache *cache) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    git_cache_free_aggregates(cache);

    for (int i = 0; i < L_HASH_SIZE; i++) {
        for (GitStatusNode *node = cache->buckets[i]; node; node = node->next) {
            int has_counts = GITF_IS_CHANGE(node->flags);
            int has_lines = node->lines_added || node->lines_removed;
            if (!has_counts && !has_lines) continue;

            char dir[PATH_MAX];
            strncpy(dir, node->path, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';

            for (;;) {
                char *slash = strrchr(dir, '/');
                if (!slash || slash == dir) break;  /* reached filesystem root */
                *slash = '\0';
                if (!git_cache_path_in_repo(cache, dir)) break;

                GitSummary *s = git_agg_get_or_create(cache, dir);
                git_summary_apply_flags(s, node->flags, +1);
                s->diff_added += node->lines_added;
                s->diff_removed += node->lines_removed;
            }
        }
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

GitSummary git_get_dir_summary(GitCache *cache, const char *dir_path) {
    GitSummary summary = {0};
    unsigned int h = hash_string(dir_path);

#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    for (struct GitDirAggregate *a = cache->agg_buckets[h]; a; a = a->next) {
        if (strcmp(a->path, dir_path) == 0) {
            summary = a->sum;
            break;
        }
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
    return summary;
}

int git_path_in_ignored(GitCache *cache, const char *path, const char *git_root) {
    if (!cache || !path || !git_root) return 0;

    size_t root_len = strlen(git_root);
    if (strncmp(path, git_root, root_len) != 0) return 0;

    /* Walk up from path to git_root, checking each ancestor */
    char check_path[PATH_MAX];
    strncpy(check_path, path, sizeof(check_path) - 1);
    check_path[sizeof(check_path) - 1] = '\0';

    while (strlen(check_path) > root_len) {
        if (git_cache_get_flags(cache, check_path) & GITF_IGNORED) {
            return 1;
        }
        /* Move up to parent directory */
        char *last_slash = strrchr(check_path, '/');
        if (!last_slash || last_slash <= check_path + root_len) break;
        *last_slash = '\0';
    }
    return 0;
}

/* ============================================================================
 * Git Branch Functions
 * ============================================================================ */

/* Resolve a repo's git directory into out: .git itself when it is a
 * directory, or the target of a .git gitlink file otherwise (submodules
 * store a relative "gitdir:" path, resolved here against the repo;
 * worktrees an absolute one). Returns 1 on success. */
static int git_resolve_gitdir(const char *repo_path, char *out, size_t out_size) {
    char git_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", repo_path);

    struct stat st;
    if (stat(git_path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        snprintf(out, out_size, "%s", git_path);
        return 1;
    }

    FILE *f = fopen(git_path, "r");
    if (!f) return 0;
    char line[PATH_MAX];
    int ok = 0;
    if (fgets(line, sizeof(line), f)) {
        const char *prefix = "gitdir: ";
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            char *gitdir = line + strlen(prefix);
            gitdir[strcspn(gitdir, "\r\n")] = '\0';
            if (gitdir[0] == '/') {
                snprintf(out, out_size, "%s", gitdir);
            } else {
                snprintf(out, out_size, "%s/%s", repo_path, gitdir);
            }
            ok = gitdir[0] != '\0';
        }
    }
    fclose(f);
    return ok;
}

/* Superproject reporting policy for a submodule (submodule.<name>.ignore). */
typedef enum {
    SUBMODULE_IGNORE_NONE = 0,
    SUBMODULE_IGNORE_UNTRACKED,
    SUBMODULE_IGNORE_DIRTY,
    SUBMODULE_IGNORE_ALL,
} SubmoduleIgnore;

/* Scan a git-config-style file for [submodule "name"]'s ignore key. Returns 1
 * and sets *out when present with a recognized value. As in git, a later
 * occurrence wins within one file. */
static int config_submodule_ignore(const char *path, const char *name,
                                   SubmoduleIgnore *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    const char *pre = "[submodule \"";
    size_t pre_len = strlen(pre);
    size_t name_len = strlen(name);

    char line[1024];
    int in_section = 0, found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            in_section = strncmp(p, pre, pre_len) == 0 &&
                         strncmp(p + pre_len, name, name_len) == 0 &&
                         p[pre_len + name_len] == '"' &&
                         p[pre_len + name_len + 1] == ']';
            continue;
        }
        if (!in_section || strncmp(p, "ignore", 6) != 0) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (strcmp(p, "untracked") == 0)  { *out = SUBMODULE_IGNORE_UNTRACKED; found = 1; }
        else if (strcmp(p, "dirty") == 0) { *out = SUBMODULE_IGNORE_DIRTY;     found = 1; }
        else if (strcmp(p, "all") == 0)   { *out = SUBMODULE_IGNORE_ALL;       found = 1; }
        else if (strcmp(p, "none") == 0)  { *out = SUBMODULE_IGNORE_NONE;      found = 1; }
    }
    fclose(f);
    return found;
}

/* The superproject's declared policy for a submodule checkout. The gitlink
 * names both parties — it points at <superproject git dir>/modules/<name> —
 * so read submodule.<name>.ignore from the superproject's config, falling
 * back to its .gitmodules, matching git's precedence. Plain nested checkouts
 * (.git is a directory) have no superproject and report NONE. */
static SubmoduleIgnore submodule_ignore_policy(const char *repo_path) {
    char git_path[PATH_MAX];
    path_join(git_path, sizeof(git_path), repo_path, ".git");
    struct stat st;
    if (stat(git_path, &st) != 0 || !S_ISREG(st.st_mode))
        return SUBMODULE_IGNORE_NONE;

    char gitdir[PATH_MAX];
    if (!git_resolve_gitdir(repo_path, gitdir, sizeof(gitdir)))
        return SUBMODULE_IGNORE_NONE;

    /* Split at the last "/modules/": the prefix is the superproject's git
     * dir (nested submodules chain their modules dirs, so the last split is
     * the immediate superproject), the suffix the submodule name. */
    char *m = NULL;
    for (char *q = strstr(gitdir, "/modules/"); q; q = strstr(q + 1, "/modules/"))
        m = q;
    if (!m) return SUBMODULE_IGNORE_NONE;
    *m = '\0';
    const char *name = m + strlen("/modules/");

    SubmoduleIgnore ign;
    char file[PATH_MAX];
    path_join(file, sizeof(file), gitdir, "config");
    if (config_submodule_ignore(file, name, &ign)) return ign;

    /* .gitmodules lives in the superproject worktree, derivable in the
     * common case where the git dir is <worktree>/.git. */
    size_t glen = strlen(gitdir);
    if (glen > 5 && strcmp(gitdir + glen - 5, "/.git") == 0) {
        gitdir[glen - 5] = '\0';
        path_join(file, sizeof(file), gitdir, ".gitmodules");
        if (config_submodule_ignore(file, name, &ign)) return ign;
    }
    return SUBMODULE_IGNORE_NONE;
}

int git_read_ref(const char *repo_path, const char *ref_name, char *hash, size_t hash_len) {
    char gitdir[PATH_MAX];
    char ref_path[PATH_MAX];
    char line[256];
    hash[0] = '\0';

    if (!git_resolve_gitdir(repo_path, gitdir, sizeof(gitdir))) return 0;

    /* Try loose ref file first */
    path_join(ref_path, sizeof(ref_path), gitdir, ref_name);
    FILE *f = fopen(ref_path, "r");
    if (f) {
        if (fgets(hash, hash_len, f)) {
            size_t len = strlen(hash);
            if (len > 0 && hash[len - 1] == '\n') hash[len - 1] = '\0';
        }
        fclose(f);
        return hash[0] != '\0';
    }

    /* Fall back to packed-refs */
    path_join(ref_path, sizeof(ref_path), gitdir, "packed-refs");
    f = fopen(ref_path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '^') continue;  /* Skip comments and peeled refs */
        /* Format: "<hash> <ref_name>\n" */
        char *space = strchr(line, ' ');
        if (!space) continue;
        *space = '\0';
        char *ref = space + 1;
        size_t ref_len = strlen(ref);
        if (ref_len > 0 && ref[ref_len - 1] == '\n') ref[ref_len - 1] = '\0';
        if (strcmp(ref, ref_name) == 0) {
            strncpy(hash, line, hash_len - 1);
            hash[hash_len - 1] = '\0';
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

char *git_get_branch(const char *repo_path) {
    char gitdir[PATH_MAX];
    char head_path[PATH_MAX];
    if (!git_resolve_gitdir(repo_path, gitdir, sizeof(gitdir))) return NULL;
    path_join(head_path, sizeof(head_path), gitdir, "HEAD");

    FILE *f = fopen(head_path, "r");
    if (!f) return NULL;

    char buf[L_GIT_HEAD_BUF_SIZE];
    char *branch = NULL;
    if (fgets(buf, sizeof(buf), f)) {
        /* Format: "ref: refs/heads/branch-name\n" */
        const char *prefix = "ref: refs/heads/";
        if (strncmp(buf, prefix, strlen(prefix)) == 0) {
            char *start = buf + strlen(prefix);
            size_t len = strlen(start);
            if (len > 0 && start[len - 1] == '\n') start[len - 1] = '\0';
            branch = xstrdup(start);
        } else {
            /* Detached HEAD - show short hash */
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
            if (len > 7) buf[7] = '\0';  /* Truncate to short hash */
            branch = xstrdup(buf);
        }
    }
    fclose(f);
    return branch;
}

int git_get_branch_info(const char *repo_path, GitBranchInfo *info) {
    info->branch = NULL;
    info->commit[0] = '\0';
    info->has_upstream = 0;
    info->out_of_sync = 0;
    info->ahead = 0;
    info->behind = 0;

    char *branch = git_get_branch(repo_path);
    if (!branch) return 0;

    info->branch = branch;

    char local_hash[64], remote_hash[64];
    char local_ref[128], remote_ref[128];
    snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
    snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);

    if (git_read_ref(repo_path, local_ref, local_hash, sizeof(local_hash))) {
        snprintf(info->commit, sizeof(info->commit), "%.7s", local_hash);
    }
    info->has_upstream = git_read_ref(repo_path, remote_ref, remote_hash, sizeof(remote_hash));

    if (info->has_upstream) {
        info->out_of_sync = (strcmp(local_hash, remote_hash) != 0);
        if (info->out_of_sync) {
#ifdef HAVE_LIBGIT2
            git_repository *repo = NULL;
            git_oid local_oid, remote_oid;
            if (git_repository_open(&repo, repo_path) == 0) {
                if (git_oid_fromstr(&local_oid, local_hash) == 0 &&
                    git_oid_fromstr(&remote_oid, remote_hash) == 0) {
                    size_t ahead = 0, behind = 0;
                    if (git_graph_ahead_behind(&ahead, &behind, repo,
                                               &local_oid, &remote_oid) == 0) {
                        info->ahead = (int)ahead;
                        info->behind = (int)behind;
                    }
                }
                git_repository_free(repo);
            }
#else
            char *esc_path = shell_escape(repo_path);
            char *esc_branch = shell_escape(branch);
            if (esc_path && esc_branch) {
                char cmd[L_SHELL_CMD_BUF_SIZE];
                char buf[64];
                snprintf(cmd, sizeof(cmd),
                         "git -C '%s' rev-list --count 'origin/%s'..'%s' 2>/dev/null",
                         esc_path, esc_branch, esc_branch);
                FILE *fp = popen(cmd, "r");
                if (fp) {
                    if (fgets(buf, sizeof(buf), fp))
                        info->ahead = atoi(buf);
                    pclose(fp);
                }
                snprintf(cmd, sizeof(cmd),
                         "git -C '%s' rev-list --count '%s'..'origin/%s' 2>/dev/null",
                         esc_path, esc_branch, esc_branch);
                fp = popen(cmd, "r");
                if (fp) {
                    if (fgets(buf, sizeof(buf), fp))
                        info->behind = atoi(buf);
                    pclose(fp);
                }
            }
            free(esc_path);
            free(esc_branch);
#endif
        }
    }

    return 1;
}

#ifndef HAVE_LIBGIT2
char *git_get_latest_tag(const char *repo_path) {
    char *escaped = shell_escape(repo_path);
    if (!escaped) return NULL;

    char cmd[L_SHELL_CMD_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "git -C '%s' describe --tags 2>/dev/null", escaped);
    free(escaped);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char buf[256];
    char *result = NULL;
    if (fgets(buf, sizeof(buf), fp)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0]) result = xstrdup(buf);
    }
    pclose(fp);
    return result;
}
#endif /* !HAVE_LIBGIT2 */

char *git_get_remote_url(const char *repo_path) {
    char gitdir[PATH_MAX];
    char config_path[PATH_MAX];
    if (!git_resolve_gitdir(repo_path, gitdir, sizeof(gitdir))) return NULL;
    path_join(config_path, sizeof(config_path), gitdir, "config");

    FILE *f = fopen(config_path, "r");
    if (!f) return NULL;

    char line[1024];
    int in_origin = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (p[0] == '[') {
            in_origin = (strncmp(p, "[remote \"origin\"]", 17) == 0);
            continue;
        }

        if (in_origin && strncmp(p, "url", 3) == 0) {
            char *eq = strchr(p, '=');
            if (!eq) continue;
            eq++;
            while (*eq == ' ' || *eq == '\t') eq++;
            eq[strcspn(eq, "\r\n")] = '\0';
            if (*eq) {
                fclose(f);
                return xstrdup(eq);
            }
        }
    }

    fclose(f);
    return NULL;
}

char *git_remote_to_web_url(const char *remote) {
    if (!remote) return NULL;

    char buf[1024];

    if (strncmp(remote, "git@", 4) == 0) {
        /* git@host:user/repo.git -> https://host/user/repo */
        const char *host = remote + 4;
        const char *colon = strchr(host, ':');
        if (!colon) return NULL;
        snprintf(buf, sizeof(buf), "https://%.*s/%s",
                 (int)(colon - host), host, colon + 1);
    } else if (strncmp(remote, "ssh://", 6) == 0) {
        /* ssh://git@host/user/repo.git -> https://host/user/repo */
        const char *p = remote + 6;
        const char *at = strchr(p, '@');
        if (at) p = at + 1;
        snprintf(buf, sizeof(buf), "https://%s", p);
    } else if (strncmp(remote, "https://", 8) == 0 ||
               strncmp(remote, "http://", 7) == 0) {
        snprintf(buf, sizeof(buf), "%s", remote);
    } else {
        return NULL;
    }

    /* Strip trailing .git */
    size_t len = strlen(buf);
    if (len > 4 && strcmp(buf + len - 4, ".git") == 0)
        buf[len - 4] = '\0';

    return xstrdup(buf);
}

/* ============================================================================
 * Shell Escape
 * ============================================================================ */

char *shell_escape(const char *path) {
    /* Count single quotes to determine buffer size */
    size_t quotes = 0;
    size_t path_len = strlen(path);
    for (const char *p = path; *p; p++) {
        if (*p == '\'') quotes++;
    }

    /* Check for integer overflow: need path_len + quotes*3 + 1 bytes */
    if (quotes > (SIZE_MAX - path_len - 1) / 3) {
        return NULL;  /* Would overflow */
    }

    /* Allocate: original length + 3 extra chars per quote + null */
    size_t len = path_len + quotes * 3 + 1;
    char *escaped = xmalloc(len);
    char *out = escaped;

    for (const char *p = path; *p; p++) {
        if (*p == '\'') {
            /* End quote, escaped quote, start quote: '\'' */
            *out++ = '\'';
            *out++ = '\\';
            *out++ = '\'';
            *out++ = '\'';
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
    return escaped;
}

/* ============================================================================
 * Git Repository Functions
 * ============================================================================ */

/* Parse numstat output lines, accumulating diff stats into cache */
static void git_parse_numstat(FILE *fp, GitCache *cache, const char *repo_path) {
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), fp)) {
        int added, removed;
        char path[PATH_SCANF_WIDTH + 1];

        /* Stop at separator (used when combining multiple outputs) */
        if (strcmp(line, "---\n") == 0 || strcmp(line, "---") == 0) break;

        if (sscanf(line, "%d\t%d\t" SCANF_PATH, &added, &removed, path) == 3) {
            char full_path[PATH_MAX];
            path_join(full_path, sizeof(full_path), repo_path, path);
            git_cache_add_diff(cache, full_path, added, removed);
        }
    }
}

/* Untracked files never appear in numstat output, so a freshly created file —
 * often the bulk of what changed — would report no line delta. Count each
 * untracked text file's lines as all-added; binary and unreadable files keep
 * zero stats, matching numstat's "-" for binaries. Paths are collected under
 * the cache lock but counted outside it (node paths are stable once
 * inserted); the stat writes re-take it via git_cache_set_diff. */
static void git_populate_untracked_diff_stats(GitCache *cache, const char *repo_path) {
    size_t root_len = strlen(repo_path);
    int capacity = 64, count = 0;
    char **paths = xmalloc(capacity * sizeof(*paths));

#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif
    for (int i = 0; i < L_HASH_SIZE; i++) {
        for (GitStatusNode *node = cache->buckets[i]; node; node = node->next) {
            if (!(node->flags & GITF_UNTRACKED)) continue;
            if (strncmp(node->path, repo_path, root_len) != 0 ||
                node->path[root_len] != '/') continue;
            if (count == capacity) {
                capacity *= 2;
                paths = xrealloc(paths, capacity * sizeof(*paths));
            }
            paths[count++] = node->path;
        }
    }
#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif

    for (int i = 0; i < count; i++) {
        /* Regular files only: opening a FIFO would block, and an untracked
         * symlink's content is its target path, not the mapped file. */
        struct stat st;
        if (lstat(paths[i], &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int lines = fileinfo_count_text_lines(paths[i]);
        if (lines >= 0) git_cache_set_diff(cache, paths[i], lines, 0);
    }
    free(paths);
}

int git_base_resolves(const char *repo_path, const char *base) {
    char *esc_repo = shell_escape(repo_path);
    char *esc_base = shell_escape(base);
    int ok = 0;
    if (esc_repo && esc_base) {
        char cmd[L_SHELL_CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' rev-parse --verify --quiet '%s^{commit}' 2>/dev/null",
                 esc_repo, esc_base);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[64];
            ok = fgets(buf, sizeof(buf), fp) != NULL;
            pclose(fp);
        }
    }
    free(esc_repo);
    free(esc_base);
    return ok;
}

/* Overlay changes relative to a base commit. git status only sees
 * HEAD-relative state, so anything committed since base would be invisible;
 * diff --name-status vs base fills those in. Entries the status pass already
 * added keep their richer two-char code (git_cache_add skips duplicates), so
 * the overlay only contributes paths that are clean relative to HEAD. */
static void git_populate_base_changes(GitCache *cache, const char *repo_path, const char *base) {
    char *esc_repo = shell_escape(repo_path);
    char *esc_base = shell_escape(base);
    if (esc_repo && esc_base) {
        char cmd[L_SHELL_CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' diff --name-status --ignore-submodules=all '%s' 2>/dev/null",
                 esc_repo, esc_base);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char line[PATH_MAX + 16];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                /* Fields are tab-separated; renames and copies list two
                 * paths — report under the current (last) one. */
                char *path = strrchr(line, '\t');
                if (!path || !path[1]) continue;
                path++;
                const char *status;
                switch (line[0]) {
                    case 'M': status = " M"; break;
                    case 'A': case 'C': status = "A "; break;
                    case 'D': status = " D"; break;
                    case 'R': status = "R "; break;
                    case 'T': status = " T"; break;
                    default: continue;
                }
                char full_path[PATH_MAX];
                path_join(full_path, sizeof(full_path), repo_path, path);
                git_cache_add(cache, full_path, status);
            }
            pclose(fp);
        }
    }
    free(esc_repo);
    free(esc_base);
}

/* Diff stats against a base commit: one tree-to-worktree numstat covers
 * committed, staged, and unstaged deltas in a single pass, replacing the
 * two HEAD-relative passes. */
static void git_populate_base_diff_stats(GitCache *cache, const char *repo_path, const char *base) {
    git_reset_diff_stats(cache, repo_path);

    char *esc_repo = shell_escape(repo_path);
    char *esc_base = shell_escape(base);
    if (esc_repo && esc_base) {
        char cmd[L_SHELL_CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' diff --numstat --ignore-submodules=all '%s' 2>/dev/null",
                 esc_repo, esc_base);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            git_parse_numstat(fp, cache, repo_path);
            pclose(fp);
        }
    }
    free(esc_repo);
    free(esc_base);
}

#ifdef HAVE_LIBGIT2

/* libgit2 implementation - faster, no fork/exec overhead */

int git_find_root(const char *path, char *root, size_t root_len) {
    git_buf buf = {0};
    if (git_repository_discover(&buf, path, 0, NULL) != 0) {
        return 0;
    }
    /* buf contains path to .git, get parent directory */
    git_repository *repo = NULL;
    if (git_repository_open(&repo, buf.ptr) != 0) {
        git_buf_dispose(&buf);
        return 0;
    }
    const char *workdir = git_repository_workdir(repo);
    if (workdir) {
        strncpy(root, workdir, root_len - 1);
        root[root_len - 1] = '\0';
        /* Remove trailing slash */
        size_t len = strlen(root);
        if (len > 0 && root[len - 1] == '/') root[len - 1] = '\0';
    }
    git_repository_free(repo);
    git_buf_dispose(&buf);
    return workdir != NULL;
}

/* Shell-based diff stats (faster than libgit2's patch-based approach) */
static void git_populate_diff_stats_shell(GitCache *cache, const char *repo_path) {
    char cmd[L_SHELL_CMD_BUF_SIZE];

    char *escaped = shell_escape(repo_path);
    if (!escaped) return;

    /* Start from zero so re-populating the same repo doesn't double the counts */
    git_reset_diff_stats(cache, repo_path);

    /* Get both unstaged and staged diff stats */
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff --numstat 2>/dev/null && "
             "echo '---' && "
             "git -C '%s' diff --cached --numstat 2>/dev/null",
             escaped, escaped);
    free(escaped);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    git_parse_numstat(fp, cache, repo_path);
    git_parse_numstat(fp, cache, repo_path);
    pclose(fp);
}

void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats, int nested, const char *base) {
    if (nested || (base && !git_base_resolves(repo_path, base))) base = NULL;

    /* A nested populate reports on the superproject's behalf, so honor its
     * declared submodule.<name>.ignore policy the way git status does. */
    int skip_untracked = 0;
    if (nested) {
        switch (submodule_ignore_policy(repo_path)) {
            case SUBMODULE_IGNORE_ALL:
            case SUBMODULE_IGNORE_DIRTY:     return;
            case SUBMODULE_IGNORE_UNTRACKED: skip_untracked = 1; break;
            case SUBMODULE_IGNORE_NONE:      break;
        }
    }
    git_cache_register_root(cache, repo_path);

    git_repository *repo = NULL;
    git_status_list *status_list = NULL;
    git_status_options opts = {0};
    opts.version = GIT_STATUS_OPTIONS_VERSION;

    if (git_repository_open(&repo, repo_path) != 0) return;

    /* Match behavior of:
     *   git status --porcelain -uall --ignored=matching --ignore-submodules=all
     * Submodule entries are excluded in both builds: a nested repo's changes
     * are aggregated from its own populate, so the superproject's single
     * "submodule modified" entry would only double-report them. */
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_IGNORED |
                 GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
    if (!skip_untracked) {
        opts.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                      GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    }

    if (git_status_list_new(&status_list, repo, &opts) != 0) {
        git_repository_free(repo);
        return;
    }

    size_t count = git_status_list_entrycount(status_list);
    for (size_t i = 0; i < count; i++) {
        const git_status_entry *entry = git_status_byindex(status_list, i);
        if (!entry) continue;

        /* Determine path (prefer new path for renames) */
        const char *path = entry->head_to_index ? entry->head_to_index->new_file.path : NULL;
        if (!path && entry->index_to_workdir)
            path = entry->index_to_workdir->new_file.path;
        if (!path) continue;

        /* Convert libgit2 status to porcelain format (XY) */
        char status[3] = {' ', ' ', '\0'};
        unsigned int s = entry->status;

        /* Index status (X) */
        if (s & GIT_STATUS_INDEX_NEW)        status[0] = 'A';
        else if (s & GIT_STATUS_INDEX_MODIFIED) status[0] = 'M';
        else if (s & GIT_STATUS_INDEX_DELETED)  status[0] = 'D';
        else if (s & GIT_STATUS_INDEX_RENAMED)  status[0] = 'R';
        else if (s & GIT_STATUS_INDEX_TYPECHANGE) status[0] = 'T';

        /* Workdir status (Y) */
        if (s & GIT_STATUS_WT_NEW)           status[1] = '?';
        else if (s & GIT_STATUS_WT_MODIFIED) status[1] = 'M';
        else if (s & GIT_STATUS_WT_DELETED)  status[1] = 'D';
        else if (s & GIT_STATUS_WT_RENAMED)  status[1] = 'R';
        else if (s & GIT_STATUS_WT_TYPECHANGE) status[1] = 'T';

        /* Ignored */
        if (s & GIT_STATUS_IGNORED) {
            status[0] = '!';
            status[1] = '!';
        }

        /* Untracked (workdir new without index) */
        if ((s & GIT_STATUS_WT_NEW) && !(s & GIT_STATUS_INDEX_NEW)) {
            status[0] = '?';
            status[1] = '?';
        }

        /* Build absolute path and remove trailing slash (directories end with /) */
        char full_path[PATH_MAX];
        path_join(full_path, sizeof(full_path), repo_path, path);
        size_t len = strlen(full_path);
        if (len > 0 && full_path[len - 1] == '/') full_path[len - 1] = '\0';

        git_cache_add(cache, full_path, status);
    }

    git_status_list_free(status_list);
    git_repository_free(repo);

    if (base) git_populate_base_changes(cache, repo_path, base);

    /* Use shell for diff stats (faster than libgit2's patch iteration) */
    if (include_diff_stats) {
        if (base) git_populate_base_diff_stats(cache, repo_path, base);
        else git_populate_diff_stats_shell(cache, repo_path);
        git_populate_untracked_diff_stats(cache, repo_path);
    }

    git_cache_rebuild_aggregates(cache);
}

char *git_get_latest_tag(const char *repo_path) {
    git_repository *repo = NULL;
    git_describe_result *result = NULL;
    git_buf buf = {0};
    char *tag_str = NULL;

    if (git_repository_open(&repo, repo_path) != 0)
        return NULL;

    git_describe_options desc_opts = {0};
    git_describe_options_init(&desc_opts, GIT_DESCRIBE_OPTIONS_VERSION);
    desc_opts.describe_strategy = GIT_DESCRIBE_TAGS;

    if (git_describe_workdir(&result, repo, &desc_opts) != 0)
        goto cleanup;

    git_describe_format_options fmt_opts = {0};
    git_describe_format_options_init(&fmt_opts, GIT_DESCRIBE_FORMAT_OPTIONS_VERSION);
    if (git_describe_format(&buf, result, &fmt_opts) != 0)
        goto cleanup;

    if (buf.ptr && buf.ptr[0])
        tag_str = xstrdup(buf.ptr);

cleanup:
    git_buf_dispose(&buf);
    git_describe_result_free(result);
    git_repository_free(repo);
    return tag_str;
}

#else

/* Fallback: shell out to git command */

static void git_parse_status_output(FILE *fp, GitCache *cache, const char *repo_path) {
    char line[PATH_MAX + 8];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        /* Stop at separator (used when combining with diff --numstat) */
        if (strcmp(line, "---") == 0) break;

        if (len < 4) continue;  /* Need at least "XY path" */

        char status[3] = {line[0], line[1], '\0'};
        const char *path = line + 3;

        /* Handle renamed files: "R  old -> new" */
        const char *arrow = strstr(path, " -> ");
        if (arrow) path = arrow + 4;

        /* Build absolute path and remove trailing slash (directories from git end with /) */
        char full_path[PATH_MAX];
        path_join(full_path, sizeof(full_path), repo_path, path);
        len = strlen(full_path);
        if (len > 0 && full_path[len - 1] == '/') full_path[len - 1] = '\0';

        git_cache_add(cache, full_path, status);
    }
}

int git_find_root(const char *path, char *root, size_t root_len) {
    char *escaped = shell_escape(path);
    if (!escaped) return 0;  /* Path too long or malformed */

    char cmd[L_SHELL_CMD_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse --show-toplevel 2>/dev/null", escaped);
    free(escaped);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    int found = 0;
    if (fgets(root, root_len, fp)) {
        size_t len = strlen(root);
        if (len > 0 && root[len - 1] == '\n') root[len - 1] = '\0';
        found = 1;
    }
    pclose(fp);
    return found;
}

void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats, int nested, const char *base) {
    if (nested || (base && !git_base_resolves(repo_path, base))) base = NULL;

    /* A nested populate reports on the superproject's behalf, so honor its
     * declared submodule.<name>.ignore policy the way git status does. */
    const char *untracked_opt = "-uall";
    if (nested) {
        switch (submodule_ignore_policy(repo_path)) {
            case SUBMODULE_IGNORE_ALL:
            case SUBMODULE_IGNORE_DIRTY:     return;
            case SUBMODULE_IGNORE_UNTRACKED: untracked_opt = "-uno"; break;
            case SUBMODULE_IGNORE_NONE:      break;
        }
    }
    git_cache_register_root(cache, repo_path);

    char *escaped = shell_escape(repo_path);
    if (!escaped) return;  /* Path too long or malformed */

    char cmd[L_SHELL_CMD_BUF_SIZE];

    /* Combine status and diff in one subprocess when diff stats needed:
     * Use --- separator to distinguish the two outputs. Base-relative stats
     * come from their own pass instead (git_populate_base_diff_stats). */
    if (include_diff_stats && !base) {
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' status --porcelain %s --ignored=matching --ignore-submodules=all 2>/dev/null && "
                 "echo '---' && "
                 "git -C '%s' diff --numstat 2>/dev/null && "
                 "echo '---' && "
                 "git -C '%s' diff --cached --numstat 2>/dev/null",
                 escaped, untracked_opt, escaped, escaped);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' status --porcelain %s --ignored=matching --ignore-submodules=all 2>/dev/null",
                 escaped, untracked_opt);
    }

    FILE *fp = popen(cmd, "r");
    if (fp) {
        /* Parse status output until separator */
        git_parse_status_output(fp, cache, repo_path);

        /* If we included diff stats, parse unstaged and staged numstat. Reset
         * first so re-populating the same repo doesn't double the counts. */
        if (include_diff_stats && !base) {
            git_reset_diff_stats(cache, repo_path);
            git_parse_numstat(fp, cache, repo_path);
            git_parse_numstat(fp, cache, repo_path);
        }
        pclose(fp);
    }
    free(escaped);

    if (base) git_populate_base_changes(cache, repo_path, base);
    if (include_diff_stats) {
        if (base) git_populate_base_diff_stats(cache, repo_path, base);
        git_populate_untracked_diff_stats(cache, repo_path);
    }
    git_cache_rebuild_aggregates(cache);
}

#endif /* HAVE_LIBGIT2 */
