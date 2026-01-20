/*
 * daemon.c - Daemon management for l-cached
 */

#include "daemon.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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

/* Read key with optional timeout (ms). Returns KEY_NONE on timeout. */
static KeyPress term_read_key(int timeout_ms) {
    if (timeout_ms > 0) {
        fd_set fds;
        struct timeval tv;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
            return KEY_NONE;
    }

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

/* Forward declarations for status refresh */
static int is_daemon_scanning(void);
static int cache_get_count(void);
static off_t cache_get_size(void);

/* Line offset from bottom of status to cache count line (set by print_status) */
static int g_cache_count_line = 0;

static void refresh_cache_line(int menu_count, int show_scanning) {
    int count = cache_get_count();

    /* Move up from menu to the cache line, update count, move back */
    int lines_up = menu_count + g_cache_count_line + 1;
    printf("\033[s");  /* Save cursor */
    printf("\033[%dA", lines_up);  /* Move up */
    printf("\r\033[K");  /* Clear line */

    if (show_scanning) {
        printf("  %s●%s Cache     %s%d%s entries %s(scanning...)%s",
               COLOR_GREEN, COLOR_RESET, COLOR_WHITE, count, COLOR_RESET,
               COLOR_GREY, COLOR_RESET);
    } else {
        off_t size = cache_get_size();
        char size_buf[32];
        if (size >= 1024 * 1024) {
            snprintf(size_buf, sizeof(size_buf), "%.1fM", (double)size / (1024 * 1024));
        } else if (size >= 1024) {
            snprintf(size_buf, sizeof(size_buf), "%lldK", (long long)size / 1024);
        } else {
            snprintf(size_buf, sizeof(size_buf), "%lldB", (long long)size);
        }
        printf("  %s●%s Cache     %s%d%s entries %s(%s)%s",
               COLOR_GREEN, COLOR_RESET, COLOR_WHITE, count, COLOR_RESET,
               COLOR_GREY, size_buf, COLOR_RESET);
    }

    printf("\033[u");  /* Restore cursor */
    fflush(stdout);
}

static int menu_run(MenuItem *items, int count, const char *binary_path) {
    int selected = 0;
    int was_scanning = is_daemon_scanning();

    /* Print initial empty lines */
    for (int i = 0; i < count; i++) printf("\n");

    term_enable_raw();
    menu_render(items, count, selected);

    while (1) {
        /* Use 200ms timeout when scanning to allow status refresh */
        int scanning = is_daemon_scanning();
        int timeout = scanning ? 200 : (was_scanning ? 1 : 0);
        KeyPress key = term_read_key(timeout);

        if (key == KEY_NONE && (scanning || was_scanning)) {
            /* Refresh cache line - show size when scan completes */
            refresh_cache_line(count, scanning);
            was_scanning = scanning;
            continue;
        }
        was_scanning = scanning;

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
    snprintf(buf, len, "%s/.cache/l/sizes-v2.db", home ? home : "/tmp");
}

static void get_log_path(char *buf, size_t len) {
    snprintf(buf, len, "/tmp/l-cached.log");
}

static void get_status_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/status", home ? home : "/tmp");
}

static int is_daemon_scanning(void) {
    char path[PATH_MAX];
    get_status_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char status[16] = "";
    if (fgets(status, sizeof(status), f)) {
        status[strcspn(status, "\n")] = '\0';
    }
    fclose(f);
    return strcmp(status, "scanning") == 0;
}

static void get_config_path(char *buf, size_t len) {
    const char *home = getenv("HOME");
    snprintf(buf, len, "%s/.cache/l/config", home ? home : "/tmp");
}

/* ============================================================================
 * Configuration (local for interactive editing, uses common.c for reading)
 * ============================================================================ */

static void config_save(int interval, int threshold) {
    char path[PATH_MAX];
    get_config_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "scan_interval=%d\n", interval);
    fprintf(f, "file_threshold=%d\n", threshold);
    fclose(f);
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
    if (system(cmd)) { /* ignore */ }
#else
    /* Reload systemd user daemon and enable+start the service */
    if (system("systemctl --user daemon-reload 2>/dev/null")) { /* ignore */ }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl --user enable --now %s 2>/dev/null", DAEMON_LABEL);
    if (system(cmd)) { /* ignore */ }
#endif
}

static void daemon_unload(void) {
#ifdef __APPLE__
    char service_path[PATH_MAX];
    get_service_path(service_path, sizeof(service_path));

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "launchctl unload '%s' 2>/dev/null", service_path);
    if (system(cmd)) { /* ignore */ }
#else
    /* Stop and disable the systemd user service */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl --user disable --now %s 2>/dev/null", DAEMON_LABEL);
    if (system(cmd)) { /* ignore */ }
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

    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path);

    /* Try temp database first (scan in progress), then fall back to final */
    const char *paths[] = {temp_path, cache_path};

    for (int i = 0; i < 2; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) continue;

        sqlite3 *db;
        if (sqlite3_open_v2(paths[i], &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            continue;
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

        if (count > 0) return count;  /* Return first non-zero count */
    }
    return 0;
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

    off_t total = 0;
    struct stat st;

    /* Main database file */
    if (stat(cache_path, &st) == 0) {
        total += st.st_size;
    }

    /* WAL file (SQLite writes here first) */
    char wal_path[PATH_MAX + 8];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", cache_path);
    if (stat(wal_path, &st) == 0) {
        total += st.st_size;
    }

    return total;
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
    int lines_after_cache = 0;  /* Count lines printed after the cache count line */

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
        /* Lines after the cache count line start here */
        printf("              %supdated %s%s\n", COLOR_GREY, time_buf, COLOR_RESET);
        lines_after_cache++;
    } else {
        printf("  %s○%s Cache     %sempty%s\n", COLOR_GREY, COLOR_RESET, COLOR_GREY, COLOR_RESET);
    }
    printf("              %s%s%s\n", COLOR_GREY, cache_path, COLOR_RESET);
    lines_after_cache++;

    /* Log file */
    char log_path[PATH_MAX];
    get_log_path(log_path, sizeof(log_path));
    struct stat st;
    if (stat(log_path, &st) == 0) {
        printf("  %s○%s Log       %s%s%s\n", COLOR_GREY, COLOR_RESET, COLOR_GREY, log_path, COLOR_RESET);
        lines_after_cache++;
    }

    /* Config */
    printf("  %s○%s Config    %sscan every %dm, cache dirs with ≥%d files%s\n",
           COLOR_GREY, COLOR_RESET, COLOR_GREY,
           config_get_interval() / 60, config_get_threshold(), COLOR_RESET);
    lines_after_cache++;

    printf("\n");
    lines_after_cache++;

    /* Set global for dynamic refresh */
    g_cache_count_line = lines_after_cache;
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

    /* Stop daemon first so it releases the database file */
    int was_running = daemon_is_running();
    if (was_running) {
        daemon_unload();
        usleep(500000);  /* Wait for daemon to stop */
    }

    cache_clear();
    printf("%sCleared %d entries%s\n", COLOR_GREEN, count, COLOR_RESET);

    /* Restart daemon if it was running */
    if (was_running) {
        daemon_load();
        printf("%sDaemon restarted%s\n", COLOR_GREEN, COLOR_RESET);
    }
}

static void action_refresh(const char *binary_path) {
    (void)binary_path;

    if (!daemon_is_running()) {
        printf("%sDaemon not running%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    /* Get daemon PID and send SIGUSR1 to trigger immediate refresh */
    pid_t pid = 0;
    FILE *fp = popen("pgrep -x l-cached", "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            pid = (pid_t)atoi(buf);
        pclose(fp);
    }

    if (pid > 0 && kill(pid, SIGUSR1) == 0) {
        printf("%sRefresh triggered%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%sFailed to signal daemon%s\n", COLOR_RED, COLOR_RESET);
    }
}

static void action_exit(const char *binary_path) {
    (void)binary_path;
    /* No-op, handled by menu return */
}

static int read_number(const char *prompt, int current) {
    term_disable_raw();
    printf("%s%s [%d]: %s", COLOR_WHITE, prompt, current, COLOR_RESET);
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) {
        term_enable_raw();
        return current;
    }

    int val = atoi(buf);
    term_enable_raw();
    return (val > 0) ? val : current;
}

static void action_configure(const char *binary_path) {
    (void)binary_path;

    int cur_interval = config_get_interval();
    int cur_threshold = config_get_threshold();

    printf("\n%sCurrent Configuration%s\n", COLOR_WHITE, COLOR_RESET);
    printf("  Scan interval: %s%d minutes%s\n", COLOR_CYAN, cur_interval / 60, COLOR_RESET);
    printf("  Min files:     %s%d%s\n\n", COLOR_CYAN, cur_threshold, COLOR_RESET);

    MenuItem items[] = {
        {"Change scan interval", NULL},
        {"Change min files", NULL},
        {"Back", NULL}
    };
    int count = 3;

    /* Print lines for menu */
    for (int i = 0; i < count; i++) printf("\n");

    term_enable_raw();
    menu_render(items, count, 0);

    int selected = 0;
    while (1) {
        KeyPress key = term_read_key(0);
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
                if (selected == 0) {
                    int mins = read_number("Scan interval (minutes)", cur_interval / 60);
                    cur_interval = mins * 60;
                    config_save(cur_interval, cur_threshold);
                    printf("%sSaved: scan every %d minutes%s\n", COLOR_GREEN, mins, COLOR_RESET);
                    printf("%sRestart daemon to apply%s\n", COLOR_GREY, COLOR_RESET);
                } else if (selected == 1) {
                    cur_threshold = read_number("Min files to cache", cur_threshold);
                    config_save(cur_interval, cur_threshold);
                    printf("%sSaved: cache dirs with >= %d files%s\n", COLOR_GREEN, cur_threshold, COLOR_RESET);
                    printf("%sRestart daemon to apply%s\n", COLOR_GREY, COLOR_RESET);
                }
                return;
            case KEY_QUIT:
                term_disable_raw();
                printf("\n");
                return;
            default:
                break;
        }
    }
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
    /* Interactive menu requires a TTY */
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "%sError:%s --daemon requires an interactive terminal\n",
                COLOR_RED, COLOR_RESET);
        fprintf(stderr, "Use: --daemon start | stop | status | refresh | clear\n");
        exit(1);
    }

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
    MenuItem items[6];
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

    items[count++] = (MenuItem){"Configure", action_configure};
    items[count++] = (MenuItem){"Exit", action_exit};

    int choice = menu_run(items, count, daemon_path);

    /* If an action was taken (not exit), show updated status */
    if (choice >= 0 && items[choice].action != action_exit) {
        printf("\n");
        print_status();
    }
}
