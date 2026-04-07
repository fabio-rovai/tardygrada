/*
 * Tardygrada — Daemon Mode
 * Persistent VM with Unix socket. CLI talks to it transparently.
 * Single-connection, no threads, no epoll. Local dev only.
 */

#ifndef TARDY_DAEMON_H
#define TARDY_DAEMON_H

#define TARDY_DAEMON_SOCKET "/tmp/tardygrada.sock"
#define TARDY_DAEMON_PID    "/tmp/tardygrada.pid"
#define TARDY_DAEMON_BUF    8192

/* Start daemon: init VM, load .tardy files from config, listen on socket.
 * config_path may be NULL (no agents loaded).
 * If foreground is true, stays in foreground (no fork). */
int tardy_daemon_start(const char *config_path, int foreground);

/* Stop daemon: send stop command to socket */
int tardy_daemon_stop(void);

/* Query daemon status */
int tardy_daemon_status(void);

#endif /* TARDY_DAEMON_H */
