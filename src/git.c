/*
 * git.c - Git status cache and retrieval implementation
 */

#include "git.h"
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
 * GitCache Functions
 * ============================================================================ */

void git_cache_init(GitCache *cache) {
    memset(cache->buckets, 0, sizeof(cache->buckets));
#ifdef _OPENMP
    omp_init_lock(&cache->lock);
#endif
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
    strncpy(node->status, status, 2);
    node->status[2] = '\0';
    node->lines_added = 0;
    node->lines_removed = 0;
    node->next = cache->buckets[h];
    cache->buckets[h] = node;

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

const char *git_cache_get(GitCache *cache, const char *path) {
    unsigned int h = hash_string(path);

#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    const char *result = NULL;
    GitStatusNode *node = cache->buckets[h];
    while (node) {
        if (strcmp(node->path, path) == 0) {
            result = node->status;
            break;
        }
        node = node->next;
    }

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif

    return result;
}

GitStatusNode *git_cache_get_node(GitCache *cache, const char *path) {
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

void git_cache_set_diff(GitCache *cache, const char *path, int added, int removed) {
#ifdef _OPENMP
    omp_set_lock(&cache->lock);
#endif

    GitStatusNode *node = git_cache_get_node(cache, path);
    if (node) {
        node->lines_added = added;
        node->lines_removed = removed;
    }

#ifdef _OPENMP
    omp_unset_lock(&cache->lock);
#endif
}

GitSummary git_get_dir_summary(GitCache *cache, const char *dir_path) {
    GitSummary summary = {0, 0, 0, 0};
    size_t dir_len = strlen(dir_path);

    /* Iterate through all buckets */
    for (int i = 0; i < L_HASH_SIZE; i++) {
        GitStatusNode *node = cache->buckets[i];
        while (node) {
            /* Check if this path is under dir_path */
            if (strncmp(node->path, dir_path, dir_len) == 0 &&
                node->path[dir_len] == '/') {
                const char *status = node->status;
                if (strcmp(status, "!!") == 0) {
                    /* Ignored - skip */
                } else if (strcmp(status, "??") == 0) {
                    summary.untracked++;
                } else {
                    /* Check staged (index) and working tree separately */
                    if (status[0] != ' ' && status[0] != '?' && status[0] != '!') {
                        summary.staged++;
                    }
                    if (status[1] == 'M') {
                        summary.modified++;
                    } else if (status[1] == 'D') {
                        summary.deleted++;
                    }
                }
            }
            node = node->next;
        }
    }
    return summary;
}

/* Internal: walk deleted entries under dir_path, accumulating count or lines */
static int git_walk_deleted(GitCache *cache, const char *dir_path,
                            int count_files, int direct_only) {
    if (!cache) return 0;

    int result = 0;
    size_t dir_len = strlen(dir_path);

    for (int i = 0; i < L_HASH_SIZE; i++) {
        GitStatusNode *node = cache->buckets[i];
        while (node) {
            if (node->status[1] == 'D' &&
                strncmp(node->path, dir_path, dir_len) == 0 &&
                node->path[dir_len] == '/') {
                /* If direct_only, skip nested paths */
                if (!direct_only || strchr(node->path + dir_len + 1, '/') == NULL) {
                    result += count_files ? 1 : node->lines_removed;
                }
            }
            node = node->next;
        }
    }
    return result;
}

int git_count_deleted_direct(GitCache *cache, const char *dir_path) {
    return git_walk_deleted(cache, dir_path, 1, 1);
}

int git_deleted_lines_direct(GitCache *cache, const char *dir_path) {
    return git_walk_deleted(cache, dir_path, 0, 1);
}

int git_deleted_lines_recursive(GitCache *cache, const char *dir_path) {
    return git_walk_deleted(cache, dir_path, 0, 0);
}

int git_dir_has_hidden_status(GitCache *cache, const char *dir_path) {
    if (!cache || !dir_path) return 0;

    size_t dir_len = strlen(dir_path);

    for (int i = 0; i < L_HASH_SIZE; i++) {
        GitStatusNode *node = cache->buckets[i];
        while (node) {
            /* Check if this is a direct child of dir_path */
            if (strncmp(node->path, dir_path, dir_len) == 0 &&
                node->path[dir_len] == '/' &&
                strchr(node->path + dir_len + 1, '/') == NULL) {
                /* Get the filename (part after dir_path/) */
                const char *filename = node->path + dir_len + 1;
                /* Check if it's hidden and has git status */
                if (filename[0] == '.' &&
                    node->status[0] != '\0' &&
                    strcmp(node->status, "!!") != 0) {
                    return 1;
                }
            }
            node = node->next;
        }
    }
    return 0;
}

GitSummary git_get_hidden_dir_summary(GitCache *cache, const char *dir_path) {
    GitSummary summary = {0, 0, 0, 0};
    if (!cache || !dir_path) return summary;

    size_t dir_len = strlen(dir_path);

    for (int i = 0; i < L_HASH_SIZE; i++) {
        GitStatusNode *node = cache->buckets[i];
        while (node) {
            /* Check if this is a direct child of dir_path */
            if (strncmp(node->path, dir_path, dir_len) == 0 &&
                node->path[dir_len] == '/' &&
                strchr(node->path + dir_len + 1, '/') == NULL) {
                /* Get the filename (part after dir_path/) */
                const char *filename = node->path + dir_len + 1;
                /* Only count hidden files */
                if (filename[0] == '.') {
                    const char *status = node->status;
                    if (strcmp(status, "!!") == 0) {
                        /* Ignored - skip */
                    } else if (strcmp(status, "??") == 0) {
                        summary.untracked++;
                    } else {
                        if (status[0] != ' ' && status[0] != '?' && status[0] != '!') {
                            summary.staged++;
                        }
                        if (status[1] == 'M') {
                            summary.modified++;
                        } else if (status[1] == 'D') {
                            summary.deleted++;
                        }
                    }
                }
            }
            node = node->next;
        }
    }
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
        const char *status = git_cache_get(cache, check_path);
        if (status && strcmp(status, "!!") == 0) {
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

int git_read_ref(const char *repo_path, const char *ref_name, char *hash, size_t hash_len) {
    char ref_path[PATH_MAX];
    char line[256];
    hash[0] = '\0';

    /* Try loose ref file first */
    snprintf(ref_path, sizeof(ref_path), "%s/.git/%s", repo_path, ref_name);
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
    snprintf(ref_path, sizeof(ref_path), "%s/.git/packed-refs", repo_path);
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
    char git_path[PATH_MAX];
    char head_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", repo_path);

    /* Check if .git is a file (worktree) or directory (normal repo) */
    struct stat st;
    if (stat(git_path, &st) != 0) return NULL;

    if (S_ISREG(st.st_mode)) {
        /* Worktree: .git is a file containing "gitdir: /path/to/git/dir" */
        FILE *gf = fopen(git_path, "r");
        if (!gf) return NULL;

        char gitdir_buf[PATH_MAX];
        char *gitdir = NULL;
        if (fgets(gitdir_buf, sizeof(gitdir_buf), gf)) {
            const char *prefix = "gitdir: ";
            if (strncmp(gitdir_buf, prefix, strlen(prefix)) == 0) {
                gitdir = gitdir_buf + strlen(prefix);
                size_t len = strlen(gitdir);
                if (len > 0 && gitdir[len - 1] == '\n') gitdir[len - 1] = '\0';
            }
        }
        fclose(gf);
        if (!gitdir) return NULL;

        snprintf(head_path, sizeof(head_path), "%s/HEAD", gitdir);
    } else {
        /* Normal repo: .git is a directory */
        snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", repo_path);
    }

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
    info->has_upstream = 0;
    info->out_of_sync = 0;

    char *branch = git_get_branch(repo_path);
    if (!branch) return 0;

    info->branch = branch;

    char local_hash[64], remote_hash[64];
    char local_ref[128], remote_ref[128];
    snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
    snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/origin/%s", branch);

    git_read_ref(repo_path, local_ref, local_hash, sizeof(local_hash));
    info->has_upstream = git_read_ref(repo_path, remote_ref, remote_hash, sizeof(remote_hash));

    if (info->has_upstream) {
        info->out_of_sync = (strcmp(local_hash, remote_hash) != 0);
    }

    return 1;
}

char *git_get_latest_tag(const char *repo_path) {
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "git -C '%s' describe --tags --abbrev=0 2>/dev/null", repo_path);
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

char *git_get_remote_url(const char *repo_path) {
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/.git/config", repo_path);

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
 * Shell Escape (for non-libgit2 fallback)
 * ============================================================================ */

#ifndef HAVE_LIBGIT2

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

#endif /* !HAVE_LIBGIT2 */

/* ============================================================================
 * Git Repository Functions
 * ============================================================================ */

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
    char line[PATH_MAX + 64];

    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff --numstat 2>/dev/null", repo_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        int added, removed;
        char path[PATH_SCANF_WIDTH + 1];

        if (sscanf(line, "%d\t%d\t" SCANF_PATH, &added, &removed, path) == 3) {
            char full_path[PATH_MAX];
            path_join(full_path, sizeof(full_path), repo_path, path);
            git_cache_set_diff(cache, full_path, added, removed);
        }
    }
    pclose(fp);
}

void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats) {
    git_repository *repo = NULL;
    git_status_list *status_list = NULL;
    git_status_options opts = {0};
    opts.version = GIT_STATUS_OPTIONS_VERSION;

    if (git_repository_open(&repo, repo_path) != 0) return;

    /* Match behavior of: git status --porcelain -uall --ignored=matching */
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                 GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                 GIT_STATUS_OPT_INCLUDE_IGNORED |
                 GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

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

    /* Use shell for diff stats (faster than libgit2's patch iteration) */
    if (include_diff_stats) {
        git_populate_diff_stats_shell(cache, repo_path);
    }
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

void git_populate_repo(GitCache *cache, const char *repo_path, int include_diff_stats) {
    char *escaped = shell_escape(repo_path);
    if (!escaped) return;  /* Path too long or malformed */

    char cmd[L_SHELL_CMD_BUF_SIZE];

    /* Combine status and diff in one subprocess when diff stats needed:
     * Use --- separator to distinguish the two outputs */
    if (include_diff_stats) {
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' status --porcelain -uall --ignored=matching 2>/dev/null && "
                 "echo '---' && "
                 "git -C '%s' diff --numstat 2>/dev/null", escaped, escaped);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' status --porcelain -uall --ignored=matching 2>/dev/null", escaped);
    }

    FILE *fp = popen(cmd, "r");
    if (fp) {
        /* Parse status output until separator */
        git_parse_status_output(fp, cache, repo_path);

        /* If we included diff stats, parse them after the separator */
        if (include_diff_stats) {
            char line[PATH_MAX + 64];
            while (fgets(line, sizeof(line), fp)) {
                int added, removed;
                char path[PATH_SCANF_WIDTH + 1];

                if (sscanf(line, "%d\t%d\t" SCANF_PATH, &added, &removed, path) == 3) {
                    char full_path[PATH_MAX];
                    path_join(full_path, sizeof(full_path), repo_path, path);
                    git_cache_set_diff(cache, full_path, added, removed);
                }
            }
        }
        pclose(fp);
    }

    free(escaped);
}

#endif /* HAVE_LIBGIT2 */
