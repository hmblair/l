/*
 * daemon.c - Daemon management for l-cached
 */

#include "daemon.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <sqlite3.h>

/* ============================================================================
 * Terminal Handling
 * ============================================================================ */

static struct termios orig_termios;
static int raw_mode_enabled = 0;

static void term_disable_raw(void) {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        printf("\033[?25h");  /* Show cursor */
        fflush(stdout);
        raw_mode_enabled = 0;
    }
}

static void sigint_handler(int sig) {
    term_disable_raw();
    printf("\n");
    _exit(128 + sig);
}

static void term_enable_raw(void) {
    if (raw_mode_enabled) return;

    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_disable_raw);

    /* Handle SIGINT to restore terminal */
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    printf("\033[?25l");  /* Hide cursor */
    raw_mode_enabled = 1;
}

typedef enum {
    KEY_NONE,
    KEY_UP,
    KEY_DOWN,
    KEY_ENTER,
    KEY_QUIT
} KeyPress;

static KeyPress term_read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return KEY_NONE;

    if (c == '\n' || c == '\r') return KEY_ENTER;
    if (c == 'q' || c == 'Q') return KEY_QUIT;
    if (c == 'k' || c == 'K') return KEY_UP;
    if (c == 'j' || c == 'J') return KEY_DOWN;

    if (c == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_NONE;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_NONE;
        if (seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;
            if (seq[1] == 'B') return KEY_DOWN;
        }
    }
    return KEY_NONE;
}

/* ============================================================================
 * Menu System
 * ============================================================================ */

typedef struct {
    const char *label;
    void (*action)(const char *binary_path);
} MenuItem;

static void menu_render(MenuItem *items, int count, int selected) {
    /* Move cursor up to redraw */
    printf("\033[%dA", count);

    for (int i = 0; i < count; i++) {
        printf("\r\033[K");  /* Clear line */
        if (i == selected) {
            printf("  %s❯%s %s%s\n", COLOR_CYAN, COLOR_RESET, items[i].label, COLOR_RESET);
        } else {
            printf("    %s%s%s\n", COLOR_GREY, items[i].label, COLOR_RESET);
        }
    }
}

static int menu_run(MenuItem *items, int count, const char *binary_path) {
    int selected = 0;

    /* Print initial empty lines */
    for (int i = 0; i < count; i++) printf("\n");

    term_enable_raw();
    menu_render(items, count, selected);

    while (1) {
        KeyPress key = term_read_key();
        switch (key) {
            case KEY_UP:
                selected = (selected - 1 + count) % count;
                menu_render(items, count, selected);
                break;
            case KEY_DOWN:
                selected = (selected + 1) % count;
                menu_render(items, count, selected);
                break;
            case KEY_ENTER:
                term_disable_raw();
                printf("\n");
                if (items[selected].action) {
                    items[selected].action(binary_path);
                }
                return selected;
            case KEY_QUIT:
                term_disable_raw();
                printf("\n");
                return -1;
            default:
                break;
        }
    }
}

/* ============================================================================
 * Path Helpers
 * ============================================================================ */

static void get_service_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(buf, len, "%s/Library/LaunchAgents/%s.plist",
             home ? home : "/tmp", DAEMON_LABEL);
#else
    snprintf(buf, len, "%s/.config/systemd/user/%s.service",
             home ? home : "/tmp", DAEMON_LABEL);
#endif
}

static void get_cache_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/sizes.db", home ? home : "/tmp");
}

static void get_log_path(char *buf, size_t len) {
    snprintf(buf, len, "/tmp/l-cached.log");
}

static void get_status_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/status", home ? home : "/tmp");
}

/* ============================================================================
 * Service Management (launchd on macOS, systemd on Linux)
 * ============================================================================ */

static int daemon_is_installed(void) {
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));
    return access(service_path, F_OK) == 0;
}

static int daemon_is_running(void) {
#ifdef __APPLE__
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "launchctl list %s >/dev/null 2>&1", DAEMON_LABEL);
    return system(cmd) == 0;
#else
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl --user is-active --quiet %s 2>/dev/null", DAEMON_LABEL);
    return system(cmd) == 0;
#endif
}

static void daemon_create_service(const char *binary_path) {
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));

    /* Ensure directory exists */
    char dir[PATH_MAX];
    const char *home = getenv("HOME");
#ifdef __APPLE__
    snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home ? home : "/tmp");
#else
    snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home ? home : "/tmp");
    /* Create parent directories */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/.config", home ? home : "/tmp");
    mkdir(parent, 0755);
    snprintf(parent, sizeof(parent), "%s/.config/systemd", home ? home : "/tmp");
    mkdir(parent, 0755);
#endif
    mkdir(dir, 0755);

    char log_path[PATH_MAX];
    get_log_path(log_path, sizeof(log_path));

    FILE *f = fopen(service_path, "w");
    if (!f) {
        fprintf(stderr, "%sError:%s Cannot create service file: %s\n",
                COLOR_RED, COLOR_RESET, service_path);
        return;
    }

#ifdef __APPLE__
    const char *user = getenv("USER");

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
    fprintf(f, "<plist version=\"1.0\">\n");
    fprintf(f, "<dict>\n");
    fprintf(f, "  <key>Label</key>\n");
    fprintf(f, "  <string>%s</string>\n", DAEMON_LABEL);
    fprintf(f, "  <key>ProgramArguments</key>\n");
    fprintf(f, "  <array>\n");
    fprintf(f, "    <string>%s</string>\n", binary_path);
    fprintf(f, "  </array>\n");
    fprintf(f, "  <key>KeepAlive</key>\n");
    fprintf(f, "  <true/>\n");
    fprintf(f, "  <key>StandardOutPath</key>\n");
    fprintf(f, "  <string>%s</string>\n", log_path);
    fprintf(f, "  <key>StandardErrorPath</key>\n");
    fprintf(f, "  <string>%s</string>\n", log_path);
    fprintf(f, "  <key>EnvironmentVariables</key>\n");
    fprintf(f, "  <dict>\n");
    fprintf(f, "    <key>HOME</key>\n");
    fprintf(f, "    <string>%s</string>\n", home ? home : "");
    fprintf(f, "    <key>USER</key>\n");
    fprintf(f, "    <string>%s</string>\n", user ? user : "");
    fprintf(f, "  </dict>\n");
    fprintf(f, "</dict>\n");
    fprintf(f, "</plist>\n");
#else
    /* systemd user service unit file */
    fprintf(f, "[Unit]\n");
    fprintf(f, "Description=l directory size cache daemon\n");
    fprintf(f, "\n");
    fprintf(f, "[Service]\n");
    fprintf(f, "Type=simple\n");
    fprintf(f, "ExecStart=%s\n", binary_path);
    fprintf(f, "Restart=always\n");
    fprintf(f, "RestartSec=5\n");
    fprintf(f, "StandardOutput=append:%s\n", log_path);
    fprintf(f, "StandardError=append:%s\n", log_path);
    fprintf(f, "Environment=HOME=%s\n", home ? home : "");
    fprintf(f, "\n");
    fprintf(f, "[Install]\n");
    fprintf(f, "WantedBy=default.target\n");
#endif
    fclose(f);
}

static void daemon_load(void) {
#ifdef __APPLE__
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "launchctl load -w '%s' 2>/dev/null", service_path);
    (void)system(cmd);
#else
    /* Reload systemd user daemon and enable+start the service */
    (void)system("systemctl --user daemon-reload 2>/dev/null");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl --user enable --now %s 2>/dev/null", DAEMON_LABEL);
    (void)system(cmd);
#endif
}

static void daemon_unload(void) {
#ifdef __APPLE__
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "launchctl unload '%s' 2>/dev/null", service_path);
    (void)system(cmd);
#else
    /* Stop and disable the systemd user service */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl --user disable --now %s 2>/dev/null", DAEMON_LABEL);
    (void)system(cmd);
#endif
}

static void daemon_remove_service(void) {
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));
    unlink(service_path);
}

/* ============================================================================
 * Cache Management
 * ============================================================================ */

static int cache_get_count(void) {
    char cache_path[PATH_MAX];
    get_cache_path(cache_path, sizeof(cache_path));

    sqlite3 *db;
    if (sqlite3_open_v2(cache_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        return 0;
    }

    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sizes", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return count;
}

static time_t cache_get_mtime(void) {
    char cache_path[PATH_MAX];
    get_cache_path(cache_path, sizeof(cache_path));

    /* Check WAL file first (SQLite WAL mode) */
    char wal_path[PATH_MAX + 8];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", cache_path);

    struct stat st;
    if (stat(wal_path, &st) == 0) {
        struct stat db_st;
        if (stat(cache_path, &db_st) == 0 && st.st_mtime > db_st.st_mtime) {
            return st.st_mtime;
        }
    }

    if (stat(cache_path, &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

static off_t cache_get_size(void) {
    char cache_path[PATH_MAX];
    get_cache_path(cache_path, sizeof(cache_path));

    struct stat st;
    if (stat(cache_path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

static void cache_clear(void) {
    char cache_path[PATH_MAX];
    get_cache_path(cache_path, sizeof(cache_path));

    char wal_path[PATH_MAX + 8], shm_path[PATH_MAX + 8];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", cache_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", cache_path);

    unlink(cache_path);
    unlink(wal_path);
    unlink(shm_path);
}

/* ============================================================================
 * Status Display
 * ============================================================================ */

static void print_status(void) {
    printf("\n");

    /* Daemon status */
    if (daemon_is_installed()) {
        if (daemon_is_running()) {
            /* Get PID */
            pid_t pid = 0;
            FILE *fp = popen("pgrep -x l-cached", "r");
            if (fp) {
                char buf[32];
                if (fgets(buf, sizeof(buf), fp))
                    pid = (pid_t)atoi(buf);
                pclose(fp);
            }
            /* Get status (scanning/idle) */
            char status_path[PATH_MAX];
            get_status_path(status_path, sizeof(status_path));
            char status[16] = "idle";
            FILE *sf = fopen(status_path, "r");
            if (sf) {
                if (fgets(status, sizeof(status), sf)) {
                    status[strcspn(status, "\n")] = '\0';
                }
                fclose(sf);
            }
            int is_scanning = (strcmp(status, "scanning") == 0);
            if (pid > 0) {
                printf("  %s●%s Daemon    %srunning%s %s(%s, PID %d)%s\n",
                       COLOR_GREEN, COLOR_RESET, COLOR_GREEN, COLOR_RESET,
                       COLOR_GREY, status, pid, COLOR_RESET);
            } else {
                printf("  %s●%s Daemon    %srunning%s %s(%s)%s\n",
                       COLOR_GREEN, COLOR_RESET, COLOR_GREEN, COLOR_RESET,
                       COLOR_GREY, status, COLOR_RESET);
            }
            (void)is_scanning;  /* Suppress unused warning */
        } else {
            printf("  %s●%s Daemon    %sstopped%s\n", COLOR_YELLOW, COLOR_RESET, COLOR_YELLOW, COLOR_RESET);
        }
    } else {
        printf("  %s○%s Daemon    %snot installed%s\n", COLOR_GREY, COLOR_RESET, COLOR_GREY, COLOR_RESET);
    }

    /* Cache status */
    int count = cache_get_count();
    time_t mtime = cache_get_mtime();
    off_t size = cache_get_size();
    char cache_path[PATH_MAX];
    get_cache_path(cache_path, sizeof(cache_path));

    if (count > 0) {
        char time_buf[64];
        struct tm *tm = localtime(&mtime);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M", tm);

        char size_buf[32];
        if (size >= 1024 * 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1fM", (double)size / (1024 * 1024));
        } else if (size >= 1024) {
            snprintf(size_buf, sizeof(size_buf), "%lldK", (long long)size / 1024);
        } else {
            snprintf(size_buf, sizeof(size_buf), "%lldB", (long long)size);
        }

        printf("  %s●%s Cache     %s%d%s entries %s(%s)%s\n",
               COLOR_GREEN, COLOR_RESET, COLOR_WHITE, count, COLOR_RESET,
               COLOR_GREY, size_buf, COLOR_RESET);
        printf("              %supdated %s%s\n", COLOR_GREY, time_buf, COLOR_RESET);
    } else {
        printf("  %s○%s Cache     %sempty%s\n", COLOR_GREY, COLOR_RESET, COLOR_GREY, COLOR_RESET);
    }
    printf("              %s%s%s\n", COLOR_GREY, cache_path, COLOR_RESET);

    /* Log file */
    char log_path[PATH_MAX];
    get_log_path(log_path, sizeof(log_path));
    struct stat st;
    if (stat(log_path, &st) == 0) {
        printf("  %s○%s Log       %s%s%s\n", COLOR_GREY, COLOR_RESET, COLOR_GREY, log_path, COLOR_RESET);
    }

    printf("\n");
}

/* ============================================================================
 * Menu Actions
 * ============================================================================ */

static void action_start(const char *binary_path) {
    if (daemon_is_running()) {
        printf("%sAlready running%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    if (!daemon_is_installed()) {
        daemon_create_service(binary_path);
    }
    daemon_load();

    /* Wait briefly and check */
    usleep(500000);
    if (daemon_is_running()) {
        printf("%sStarted%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%sFailed to start%s - check log\n", COLOR_RED, COLOR_RESET);
    }
}

static void action_stop(const char *binary_path) {
    (void)binary_path;

    if (!daemon_is_running()) {
        printf("%sNot running%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    daemon_unload();
    daemon_remove_service();
    printf("%sStopped and uninstalled%s\n", COLOR_GREEN, COLOR_RESET);
}

static void action_clear(const char *binary_path) {
    (void)binary_path;

    int count = cache_get_count();
    if (count == 0) {
        printf("%sCache already empty%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    cache_clear();
    printf("%sCleared %d entries%s\n", COLOR_GREEN, count, COLOR_RESET);
}

static void action_refresh(const char *binary_path) {
    (void)binary_path;

    if (!daemon_is_running()) {
        printf("%sDaemon not running%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    /* Send SIGUSR1 to trigger immediate refresh */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pkill -USR1 -f l-cached 2>/dev/null");
    if (system(cmd) == 0) {
        printf("%sRefresh triggered%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%sFailed to signal daemon%s\n", COLOR_RED, COLOR_RESET);
    }
}

static void action_exit(const char *binary_path) {
    (void)binary_path;
    /* No-op, handled by menu return */
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

static int find_in_path(const char *name, char *result) {
    const char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char *path_copy = strdup(path_env);
    if (!path_copy) return 0;

    char *dir = strtok(path_copy, ":");
    while (dir) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            if (realpath(candidate, result)) {
                free(path_copy);
                return 1;
            }
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);
    return 0;
}

void daemon_run(const char *binary_path) {
    /* Resolve to find actual binary location */
    char resolved_path[PATH_MAX];
    int found = 0;

    /* If path contains /, try realpath directly */
    if (strchr(binary_path, '/')) {
        if (realpath(binary_path, resolved_path)) {
            found = 1;
        }
    }

    /* Otherwise search PATH */
    if (!found) {
        found = find_in_path(binary_path, resolved_path);
    }

    /* Find l-cached next to resolved binary */
    char daemon_path[PATH_MAX];
    if (found) {
        char *slash = strrchr(resolved_path, '/');
        if (slash) {
            size_t dir_len = slash - resolved_path;
            snprintf(daemon_path, sizeof(daemon_path), "%.*s/l-cached",
                     (int)dir_len, resolved_path);
        } else {
            found = 0;
        }
    }

    if (!found) {
        snprintf(daemon_path, sizeof(daemon_path), "./l-cached");
    }

    /* Check daemon binary exists */
    if (access(daemon_path, X_OK) != 0) {
        fprintf(stderr, "%sError:%s l-cached not found at %s\n",
                COLOR_RED, COLOR_RESET, daemon_path);
        exit(1);
    }

    print_status();

    /* Build menu based on current state */
    MenuItem items[5];
    int count = 0;

    if (daemon_is_running()) {
        items[count++] = (MenuItem){"Refresh now", action_refresh};
        items[count++] = (MenuItem){"Stop daemon", action_stop};
    } else {
        items[count++] = (MenuItem){"Start daemon", action_start};
    }

    if (cache_get_count() > 0) {
        items[count++] = (MenuItem){"Clear cache", action_clear};
    }

    items[count++] = (MenuItem){"Exit", action_exit};

    int choice = menu_run(items, count, daemon_path);

    /* If an action was taken (not exit), show updated status */
    if (choice >= 0 && items[choice].action != action_exit) {
        printf("\n");
        print_status();
    }
}
