/*
 * Tardygrada — MCP Bridge Implementation
 * Single-threaded proxy: MCP JSON-RPC (stdin/stdout) <-> daemon JSON-line (Unix socket).
 * No malloc. Reuses tardy_daemon_send() for all daemon communication.
 */

#include "mcp_bridge.h"
#include "mcp/json.h"
#include "daemon_client.h"
#include "vm/util.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define BRIDGE_BUF_SIZE 8192

/* ============================================
 * MCP stdio write (Content-Length framing)
 * ============================================ */

static void bridge_write(const char *data, int len)
{
    char header[64];
    int hlen = snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n", len);
    tardy_write(STDOUT_FILENO, header, hlen);
    tardy_write(STDOUT_FILENO, data, len);
}

/* ============================================
 * JSON escape helper
 * ============================================ */

static int json_escape(const char *src, char *dst, int dst_size)
{
    int w = 0;
    for (int i = 0; src[i] && w < dst_size - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[w++] = '\\';
        } else if (src[i] == '\n') {
            dst[w++] = '\\';
            dst[w++] = 'n';
            continue;
        } else if (src[i] == '\r') {
            dst[w++] = '\\';
            dst[w++] = 'r';
            continue;
        } else if (src[i] == '\t') {
            dst[w++] = '\\';
            dst[w++] = 't';
            continue;
        }
        dst[w++] = src[i];
    }
    dst[w] = '\0';
    return w;
}

/* ============================================
 * Response builders
 * ============================================ */

static int build_response(char *buf, int buf_size, const char *id,
                           const char *result)
{
    return snprintf(buf, buf_size,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
}

static int build_error(char *buf, int buf_size, const char *id,
                        int code, const char *message)
{
    return snprintf(buf, buf_size,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        id, code, message);
}

static int build_tool_result(char *buf, int buf_size, const char *id,
                              const char *text)
{
    char escaped[4096];
    json_escape(text, escaped, sizeof(escaped));
    return snprintf(buf, buf_size,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}",
        id, escaped);
}

/* ============================================
 * Tool definitions (static JSON)
 * ============================================ */

static const char *TOOLS_LIST =
    "{\"tools\":["
    "{\"name\":\"verify_claim\","
     "\"description\":\"Verify a claim using Tardygrada's 8-layer pipeline\","
     "\"inputSchema\":{\"type\":\"object\","
      "\"properties\":{\"claim\":{\"type\":\"string\",\"description\":\"The claim to verify\"}},"
      "\"required\":[\"claim\"]}},"
    "{\"name\":\"verify_document\","
     "\"description\":\"Scan a document for internal contradictions\","
     "\"inputSchema\":{\"type\":\"object\","
      "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the document\"}},"
      "\"required\":[\"path\"]}},"
    "{\"name\":\"spawn_agent\","
     "\"description\":\"Spawn a persistent agent in the daemon\","
     "\"inputSchema\":{\"type\":\"object\","
      "\"properties\":{\"name\":{\"type\":\"string\"},\"trust\":{\"type\":\"string\","
       "\"enum\":[\"mutable\",\"default\",\"verified\",\"hardened\",\"sovereign\"]}},"
      "\"required\":[\"name\"]}},"
    "{\"name\":\"read_agent\","
     "\"description\":\"Read an agent's value from the daemon\","
     "\"inputSchema\":{\"type\":\"object\","
      "\"properties\":{\"agent\":{\"type\":\"string\"},\"field\":{\"type\":\"string\"}},"
      "\"required\":[\"agent\"]}},"
    "{\"name\":\"daemon_status\","
     "\"description\":\"Get Tardygrada daemon status\","
     "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
    "]}";

/* ============================================
 * Tool dispatch — translate MCP tool call to daemon JSON
 * ============================================ */

static int handle_tool_call(const char *tool_name, const char *args_json,
                             const char *id, char *out, int out_size)
{
    char daemon_req[4096];
    char daemon_resp[4096];
    int len;

    /* Parse arguments */
    tardy_json_parser_t ap;

    if (strcmp(tool_name, "verify_claim") == 0) {
        if (tardy_json_parse(&ap, args_json, (int)strlen(args_json)) < 0)
            return build_error(out, out_size, id, -32602, "Invalid arguments");
        int ci = tardy_json_find(&ap, 0, "claim");
        if (ci < 0)
            return build_error(out, out_size, id, -32602, "Missing claim argument");
        char claim[2048];
        tardy_json_str(&ap, ci, claim, sizeof(claim));
        char escaped[2048];
        json_escape(claim, escaped, sizeof(escaped));
        snprintf(daemon_req, sizeof(daemon_req),
                 "{\"cmd\":\"run\",\"claim\":\"%s\"}", escaped);

    } else if (strcmp(tool_name, "verify_document") == 0) {
        if (tardy_json_parse(&ap, args_json, (int)strlen(args_json)) < 0)
            return build_error(out, out_size, id, -32602, "Invalid arguments");
        int pi = tardy_json_find(&ap, 0, "path");
        if (pi < 0)
            return build_error(out, out_size, id, -32602, "Missing path argument");
        char path[1024];
        tardy_json_str(&ap, pi, path, sizeof(path));
        char escaped[1024];
        json_escape(path, escaped, sizeof(escaped));
        snprintf(daemon_req, sizeof(daemon_req),
                 "{\"cmd\":\"verify-doc\",\"path\":\"%s\"}", escaped);

    } else if (strcmp(tool_name, "spawn_agent") == 0) {
        if (tardy_json_parse(&ap, args_json, (int)strlen(args_json)) < 0)
            return build_error(out, out_size, id, -32602, "Invalid arguments");
        int ni = tardy_json_find(&ap, 0, "name");
        if (ni < 0)
            return build_error(out, out_size, id, -32602, "Missing name argument");
        char name[256];
        tardy_json_str(&ap, ni, name, sizeof(name));
        char trust[64] = "default";
        int ti = tardy_json_find(&ap, 0, "trust");
        if (ti >= 0)
            tardy_json_str(&ap, ti, trust, sizeof(trust));
        snprintf(daemon_req, sizeof(daemon_req),
                 "{\"cmd\":\"spawn\",\"name\":\"%s\",\"trust\":\"%s\"}", name, trust);

    } else if (strcmp(tool_name, "read_agent") == 0) {
        if (tardy_json_parse(&ap, args_json, (int)strlen(args_json)) < 0)
            return build_error(out, out_size, id, -32602, "Invalid arguments");
        int ai = tardy_json_find(&ap, 0, "agent");
        if (ai < 0)
            return build_error(out, out_size, id, -32602, "Missing agent argument");
        char agent[256];
        tardy_json_str(&ap, ai, agent, sizeof(agent));
        int fi = tardy_json_find(&ap, 0, "field");
        if (fi >= 0) {
            char field[256];
            tardy_json_str(&ap, fi, field, sizeof(field));
            snprintf(daemon_req, sizeof(daemon_req),
                     "{\"cmd\":\"read\",\"agent\":\"%s\",\"field\":\"%s\"}", agent, field);
        } else {
            snprintf(daemon_req, sizeof(daemon_req),
                     "{\"cmd\":\"read\",\"agent\":\"%s\"}", agent);
        }

    } else if (strcmp(tool_name, "daemon_status") == 0) {
        snprintf(daemon_req, sizeof(daemon_req), "{\"cmd\":\"status\"}");

    } else {
        return build_error(out, out_size, id, -32601, "Unknown tool");
    }

    /* Send to daemon */
    len = tardy_daemon_send(daemon_req, daemon_resp, sizeof(daemon_resp));
    if (len <= 0)
        return build_error(out, out_size, id, -32000, "Daemon communication failed");

    return build_tool_result(out, out_size, id, daemon_resp);
}

/* ============================================
 * MCP request handler
 * ============================================ */

static int handle_request(const char *request, int req_len,
                           char *out, int out_size)
{
    tardy_json_parser_t p;
    if (tardy_json_parse(&p, request, req_len) < 0)
        return build_error(out, out_size, "null", -32700, "Parse error");

    /* Extract id (may not exist for notifications) */
    char id[64] = "null";
    int id_tok = tardy_json_find(&p, 0, "id");
    if (id_tok >= 0) {
        /* Copy raw token value (could be number or string) */
        int tlen = p.tokens[id_tok].len;
        if (tlen > 0 && tlen < (int)sizeof(id) - 2) {
            if (p.tokens[id_tok].type == TARDY_JSON_STRING) {
                id[0] = '"';
                memcpy(id + 1, p.tokens[id_tok].start, tlen);
                id[tlen + 1] = '"';
                id[tlen + 2] = '\0';
            } else {
                memcpy(id, p.tokens[id_tok].start, tlen);
                id[tlen] = '\0';
            }
        }
    }

    /* Extract method */
    int method_tok = tardy_json_find(&p, 0, "method");
    if (method_tok < 0)
        return build_error(out, out_size, id, -32600, "Invalid request");

    /* initialize */
    if (tardy_json_eq(&p, method_tok, "initialize")) {
        return build_response(out, out_size, id,
            "{\"protocolVersion\":\"2024-11-05\","
             "\"capabilities\":{\"tools\":{}},"
             "\"serverInfo\":{\"name\":\"tardygrada\",\"version\":\"0.1.0\"}}");
    }

    /* notifications/initialized — no response */
    if (tardy_json_eq(&p, method_tok, "notifications/initialized"))
        return 0;

    /* tools/list */
    if (tardy_json_eq(&p, method_tok, "tools/list"))
        return build_response(out, out_size, id, TOOLS_LIST);

    /* tools/call */
    if (tardy_json_eq(&p, method_tok, "tools/call")) {
        int params_tok = tardy_json_find(&p, 0, "params");
        if (params_tok < 0)
            return build_error(out, out_size, id, -32602, "Missing params");

        /* Find tool name */
        int name_tok = tardy_json_find(&p, params_tok, "name");
        if (name_tok < 0)
            return build_error(out, out_size, id, -32602, "Missing tool name");
        char tool_name[128];
        tardy_json_str(&p, name_tok, tool_name, sizeof(tool_name));

        /* Find arguments — extract raw JSON substring */
        int args_tok = tardy_json_find(&p, params_tok, "arguments");
        char args_json[4096] = "{}";
        if (args_tok >= 0) {
            /* Extract the raw JSON for the arguments object */
            const tardy_json_token_t *at = &p.tokens[args_tok];
            if (at->type == TARDY_JSON_OBJECT) {
                /* The token start points to the opening {, we need to find matching } */
                /* Use the raw source: find balanced braces */
                const char *start = at->start;
                int depth = 0;
                int alen = 0;
                for (int i = 0; start + i <= request + req_len; i++) {
                    if (start[i] == '{') depth++;
                    else if (start[i] == '}') { depth--; if (depth == 0) { alen = i + 1; break; } }
                }
                if (alen > 0 && alen < (int)sizeof(args_json)) {
                    memcpy(args_json, start, alen);
                    args_json[alen] = '\0';
                }
            }
        }

        return handle_tool_call(tool_name, args_json, id, out, out_size);
    }

    /* Unknown method */
    return build_error(out, out_size, id, -32601, "Method not found");
}

/* ============================================
 * Main loop — read MCP stdin, dispatch, write MCP stdout
 * ============================================ */

int tardy_mcp_bridge_run(void)
{
    char read_buf[BRIDGE_BUF_SIZE];
    char write_buf[BRIDGE_BUF_SIZE];

    for (;;) {
        /* Read Content-Length header */
        char hdr[128];
        int hpos = 0;
        int content_length = -1;

        while (hpos < (int)sizeof(hdr) - 1) {
            ssize_t n = read(STDIN_FILENO, hdr + hpos, 1);
            if (n <= 0)
                return 0; /* EOF — clean shutdown */
            hpos++;

            /* Check for \r\n\r\n (end of headers) */
            if (hpos >= 4 &&
                hdr[hpos - 4] == '\r' && hdr[hpos - 3] == '\n' &&
                hdr[hpos - 2] == '\r' && hdr[hpos - 1] == '\n') {

                hdr[hpos] = '\0';
                const char *cl = "Content-Length: ";
                char *found = NULL;
                for (int i = 0; i < hpos - 15; i++) {
                    if (memcmp(hdr + i, cl, 16) == 0) {
                        found = hdr + i + 16;
                        break;
                    }
                }
                if (found) {
                    content_length = 0;
                    while (*found >= '0' && *found <= '9') {
                        content_length = content_length * 10 + (*found - '0');
                        found++;
                    }
                }
                break;
            }
        }

        if (content_length <= 0 || content_length >= BRIDGE_BUF_SIZE)
            continue;

        /* Read body */
        int total = 0;
        while (total < content_length) {
            ssize_t n = read(STDIN_FILENO, read_buf + total,
                            content_length - total);
            if (n <= 0)
                return 0;
            total += (int)n;
        }
        read_buf[content_length] = '\0';

        /* Handle request */
        int rlen = handle_request(read_buf, content_length,
                                   write_buf, sizeof(write_buf));
        if (rlen > 0)
            bridge_write(write_buf, rlen);
    }
}
