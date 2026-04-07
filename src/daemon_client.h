/*
 * Tardygrada — Daemon Client
 * Connect to running daemon, send JSON, receive response.
 */

#ifndef TARDY_DAEMON_CLIENT_H
#define TARDY_DAEMON_CLIENT_H

#include <stddef.h>

/* Check if daemon is running (try connect to socket) */
int tardy_daemon_is_running(void);

/* Connect to daemon, send JSON line, receive JSON line response.
 * Returns length of response on success, -1 on failure. */
int tardy_daemon_send(const char *json_request, char *response, size_t response_len);

#endif /* TARDY_DAEMON_CLIENT_H */
