/*
 * daemon.h - Daemon management for l-cached
 *
 * Provides interactive menu for managing the size caching daemon,
 * including start/stop/status and cache management.
 */

#ifndef L_DAEMON_H
#define L_DAEMON_H

#include "common.h"

/* Launchd service label */
#define DAEMON_LABEL "com.zu.l-cached"

/* Run the daemon management interface */
void daemon_run(const char *binary_path);

#endif /* L_DAEMON_H */
