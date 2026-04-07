/*
 * Tardygrada — LLM Grounding Client
 *
 * Connects to an external LLM service via Unix socket.
 * Sends JSON requests, parses JSON responses.
 * Zero library dependencies — just POSIX sockets and string ops.
 */

#include "llm_ground.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/* ============================================
 * Environment check
 * ============================================ */

bool tardy_llm_ground_enabled(void)
{
    const char *val = getenv("TARDY_LLM_GROUND");
    return (val && (val[0] == '1' || val[0] == 'y' || val[0] == 'Y'));
}

/* ============================================
 * Connection management
 * ============================================ */

int tardy_llm_connect(tardy_llm_conn_t *conn)
{
    if (!conn) return -1;

    memset(conn, 0, sizeof(tardy_llm_conn_t));
    conn->fd = -1;
    conn->connected = false;

    conn->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (conn->fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TARDY_LLM_SOCKET, sizeof(addr.sun_path) - 1);

    if (connect(conn->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(conn->fd);
        conn->fd = -1;
        return -1;
    }

    conn->connected = true;
    return 0;
}

void tardy_llm_disconnect(tardy_llm_conn_t *conn)
{
    if (!conn) return;
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->connected = false;
}

/* ============================================
 * Minimal JSON helpers — no library needed
 * ============================================ */

/* Extract a string value for a given key from JSON.
 * Writes into out (max out_size). Returns 0 on success. */
static int json_extract_str(const char *json, const char *key,
                             char *out, int out_size)
{
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return -1;

    const char *p = strstr(json, pattern);
    if (!p) return -1;

    /* Skip past key and colon */
    p += plen;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    if (*p != '"') return -1;
    p++; /* skip opening quote */

    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++; /* skip escape */
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/* Extract a boolean value for a given key. Returns -1 on failure. */
static int json_extract_bool(const char *json, const char *key)
{
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return -1;

    const char *p = strstr(json, pattern);
    if (!p) return -1;

    p += plen;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

/* Extract a float value for a given key. Returns -1.0 on failure. */
static float json_extract_float(const char *json, const char *key)
{
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen < 0 || plen >= (int)sizeof(pattern)) return -1.0f;

    const char *p = strstr(json, pattern);
    if (!p) return -1.0f;

    p += plen;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    char numbuf[32];
    int i = 0;
    while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-')
           && i < 31) {
        numbuf[i++] = *p++;
    }
    numbuf[i] = '\0';

    if (i == 0) return -1.0f;

    char *end = NULL;
    float val = (float)strtod(numbuf, &end);
    if (end == numbuf) return -1.0f;
    return val;
}

/* Escape a string for JSON embedding. Writes to out. Returns length. */
static int json_escape(const char *src, char *out, int out_size)
{
    int i = 0;
    while (*src && i < out_size - 2) {
        if (*src == '"' || *src == '\\') {
            out[i++] = '\\';
            if (i >= out_size - 1) break;
        }
        if (*src == '\n') {
            out[i++] = '\\';
            if (i >= out_size - 1) break;
            out[i++] = 'n';
        } else if (*src == '\r') {
            out[i++] = '\\';
            if (i >= out_size - 1) break;
            out[i++] = 'r';
        } else if (*src == '\t') {
            out[i++] = '\\';
            if (i >= out_size - 1) break;
            out[i++] = 't';
        } else {
            out[i++] = *src;
        }
        src++;
    }
    out[i] = '\0';
    return i;
}

/* ============================================
 * Send request, receive response
 * ============================================ */

static tardy_llm_ground_result_t send_and_recv(tardy_llm_conn_t *conn,
                                                 const char *request)
{
    tardy_llm_ground_result_t result = {false, 0.0f, ""};

    if (!conn || !conn->connected || conn->fd < 0)
        return result;

    /* Send request (one line of JSON + newline) */
    int req_len = (int)strlen(request);
    ssize_t sent = write(conn->fd, request, (size_t)req_len);
    if (sent != req_len) {
        snprintf(result.explanation, sizeof(result.explanation),
                 "send failed: %s", strerror(errno));
        return result;
    }

    /* Receive response — read until newline or buffer full */
    int total = 0;
    while (total < (int)sizeof(conn->buf) - 1) {
        ssize_t n = read(conn->fd, conn->buf + total,
                         (size_t)(sizeof(conn->buf) - 1 - (size_t)total));
        if (n <= 0) break;
        total += (int)n;
        /* Check for newline — end of response */
        int found_nl = 0;
        for (int i = 0; i < total; i++) {
            if (conn->buf[i] == '\n') { found_nl = 1; break; }
        }
        if (found_nl) break;
    }
    conn->buf[total] = '\0';

    if (total == 0) {
        snprintf(result.explanation, sizeof(result.explanation),
                 "empty response from LLM socket");
        return result;
    }

    /* Check for error */
    char err[256];
    if (json_extract_str(conn->buf, "error", err, sizeof(err)) == 0) {
        snprintf(result.explanation, sizeof(result.explanation),
                 "LLM error: %.200s", err);
        return result;
    }

    /* Parse response */
    int grounded = json_extract_bool(conn->buf, "grounded");
    float confidence = json_extract_float(conn->buf, "confidence");
    char explanation[256];

    result.grounded = (grounded == 1);
    result.confidence = (confidence >= 0.0f) ? confidence : 0.0f;

    if (json_extract_str(conn->buf, "explanation", explanation,
                          sizeof(explanation)) == 0) {
        strncpy(result.explanation, explanation, sizeof(result.explanation) - 1);
        result.explanation[sizeof(result.explanation) - 1] = '\0';
    } else {
        snprintf(result.explanation, sizeof(result.explanation),
                 "LLM returned grounded=%s, confidence=%.2f",
                 result.grounded ? "true" : "false",
                 (double)result.confidence);
    }

    return result;
}

/* ============================================
 * Public API
 * ============================================ */

tardy_llm_ground_result_t tardy_llm_ground_triple(
    tardy_llm_conn_t *conn,
    const char *subject, const char *predicate, const char *object)
{
    tardy_llm_ground_result_t fail = {false, 0.0f, "not connected"};
    if (!conn || !conn->connected) return fail;

    /* Build JSON request */
    char esc_s[512], esc_p[256], esc_o[512];
    json_escape(subject, esc_s, sizeof(esc_s));
    json_escape(predicate, esc_p, sizeof(esc_p));
    json_escape(object, esc_o, sizeof(esc_o));

    char request[4096];
    int rlen = snprintf(request, sizeof(request),
        "{\"action\":\"ground_triple\","
        "\"subject\":\"%s\","
        "\"predicate\":\"%s\","
        "\"object\":\"%s\"}\n",
        esc_s, esc_p, esc_o);

    if (rlen < 0 || rlen >= (int)sizeof(request)) {
        tardy_llm_ground_result_t err = {false, 0.0f, "request too large"};
        return err;
    }

    return send_and_recv(conn, request);
}

tardy_llm_ground_result_t tardy_llm_ground_claim(
    tardy_llm_conn_t *conn,
    const char *claim)
{
    tardy_llm_ground_result_t fail = {false, 0.0f, "not connected"};
    if (!conn || !conn->connected) return fail;

    char esc_claim[4096];
    json_escape(claim, esc_claim, sizeof(esc_claim));

    char request[8192];
    int rlen = snprintf(request, sizeof(request),
        "{\"action\":\"ground_claim\","
        "\"claim\":\"%s\"}\n",
        esc_claim);

    if (rlen < 0 || rlen >= (int)sizeof(request)) {
        tardy_llm_ground_result_t err = {false, 0.0f, "request too large"};
        return err;
    }

    return send_and_recv(conn, request);
}
