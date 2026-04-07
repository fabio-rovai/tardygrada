/*
 * Tardygrada — Daemon Client
 * Same Unix socket pattern as coordinate/bridge.c.
 * No malloc. Direct POSIX.
 */

#include "daemon_client.h"
#include "daemon.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>

int tardy_daemon_is_running(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TARDY_DAEMON_SOCKET, sizeof(addr.sun_path) - 1);

    int ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

int tardy_daemon_send(const char *json_request, char *response, size_t response_len)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TARDY_DAEMON_SOCKET, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Send request + newline */
    size_t req_len = strlen(json_request);
    ssize_t sent = write(fd, json_request, req_len);
    if (sent != (ssize_t)req_len) { close(fd); return -1; }
    sent = write(fd, "\n", 1);
    (void)sent;

    /* Read response until newline */
    size_t total = 0;
    while (total < response_len - 1) {
        ssize_t n = read(fd, response + total, 1);
        if (n <= 0) break;
        if (response[total] == '\n') break;
        total++;
    }
    response[total] = '\0';
    close(fd);
    return (int)total;
}
