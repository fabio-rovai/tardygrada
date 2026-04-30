/*
 * Tardygrada — Daemon Client
 * Same Unix socket pattern as coordinate/bridge.c.
 * No malloc. Direct POSIX.
 */

#include "daemon_client.h"
#include "daemon.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
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

    /* Mirror the server-side hardening: a wedged or slow daemon must not
     * be able to hang the client indefinitely. Bound every recv/send to
     * 10s — generous enough for verify-doc on large files, tight enough
     * to fail rather than freeze. */
    struct timeval rcv_to = { .tv_sec = 10, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    struct timeval snd_to = { .tv_sec = 10, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

    /* Send request + newline */
    size_t req_len = strlen(json_request);
    ssize_t sent = write(fd, json_request, req_len);
    if (sent != (ssize_t)req_len) { close(fd); return -1; }
    sent = write(fd, "\n", 1);
    (void)sent;

    /* Buffered read scanning for '\n'. Replaces a byte-by-byte read loop
     * that issued one syscall per character. */
    size_t total = 0;
    while (total + 1 < response_len) {
        ssize_t n = read(fd, response + total, response_len - 1 - total);
        if (n <= 0) break;
        int found_nl = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (response[total + i] == '\n') {
                total += (size_t)i;
                found_nl = 1;
                break;
            }
        }
        if (found_nl) break;
        total += (size_t)n;
    }
    response[total] = '\0';
    close(fd);
    return (int)total;
}
