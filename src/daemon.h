/*
 * daemon.h - Daemon management for l-cached
 *
 * Provides interactive menu for managing the size caching daemon,
 * including start/stop/status and cache management.
 */

#ifndef L_DAEMON_H
#define L_DAEMON_H

#include "common.h"

/* Service names */
#ifdef __APPLE__
#define DAEMON_LABEL "com.l.cached"      /* launchd label */
#else
#define DAEMON_LABEL "l-cached"          /* systemd service name */
#endif

/* Run the daemon management interface */
void daemon_run(const char *binary_path);

#endif /* L_DAEMON_H */
