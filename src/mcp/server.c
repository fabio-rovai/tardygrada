/*
 * Tardygrada — MCP Server Implementation
 * JSON-RPC 2.0 over stdin/stdout.
 *
 * A Tardygrada program compiles to this.
 * The world connects. Asks questions. Gets verified responses.
 */

#include "server.h"
#include <unistd.h>
#include <string.h>

/* Direct write — no stdio */
static void mcp_write(const char *data, int len)
{
    /* MCP stdio transport: Content-Length header + \r\n\r\n + body */
    char header[64];
    int hlen = 0;
    const char *prefix = "Content-Length: ";
    int plen = (int)strlen(prefix);
    memcpy(header, prefix, plen);
    hlen = plen;

    /* itoa for content length */
    char num[16];
    int nlen = 0;
    int tmp = len;
    if (tmp == 0) {
        num[nlen++] = '0';
    } else {
        while (tmp > 0) {
            num[nlen++] = '0' + (tmp % 10);
            tmp /= 10;
        }
    }
    for (int i = nlen - 1; i >= 0; i--)
        header[hlen++] = num[i];
    header[hlen++] = '\r';
    header[hlen++] = '\n';
    header[hlen++] = '\r';
    header[hlen++] = '\n';

    write(STDOUT_FILENO, header, hlen);
    write(STDOUT_FILENO, data, len);
}

/* Build a JSON-RPC response */
static int build_response(char *buf, int buf_size, const char *id,
                           const char *result)
{
    int written = 0;
    const char *pre = "{\"jsonrpc\":\"2.0\",\"id\":";
    int prelen = (int)strlen(pre);
    if (written + prelen >= buf_size) return -1;
    memcpy(buf + written, pre, prelen);
    written += prelen;

    int idlen = (int)strlen(id);
    if (written + idlen >= buf_size) return -1;
    memcpy(buf + written, id, idlen);
    written += idlen;

    const char *mid = ",\"result\":";
    int midlen = (int)strlen(mid);
    if (written + midlen >= buf_size) return -1;
    memcpy(buf + written, mid, midlen);
    written += midlen;

    int rlen = (int)strlen(result);
    if (written + rlen >= buf_size) return -1;
    memcpy(buf + written, result, rlen);
    written += rlen;

    if (written + 1 >= buf_size) return -1;
    buf[written++] = '}';
    buf[written] = '\0';
    return written;
}

/* Build an error response */
static int build_error(char *buf, int buf_size, const char *id,
                        int code, const char *message)
{
    int written = 0;
    const char *pre = "{\"jsonrpc\":\"2.0\",\"id\":";
    int prelen = (int)strlen(pre);
    if (written + prelen >= buf_size) return -1;
    memcpy(buf + written, pre, prelen);
    written += prelen;

    int idlen = (int)strlen(id);
    if (written + idlen >= buf_size) return -1;
    memcpy(buf + written, id, idlen);
    written += idlen;

    const char *mid = ",\"error\":{\"code\":";
    int midlen = (int)strlen(mid);
    if (written + midlen >= buf_size) return -1;
    memcpy(buf + written, mid, midlen);
    written += midlen;

    /* code as string */
    char codestr[16];
    int clen = 0;
    int neg = 0;
    int c = code;
    if (c < 0) { neg = 1; c = -c; }
    if (c == 0) { codestr[clen++] = '0'; }
    while (c > 0) { codestr[clen++] = '0' + (c % 10); c /= 10; }
    if (neg) codestr[clen++] = '-';
    for (int i = 0; i < clen / 2; i++) {
        char t = codestr[i];
        codestr[i] = codestr[clen - 1 - i];
        codestr[clen - 1 - i] = t;
    }
    if (written + clen >= buf_size) return -1;
    memcpy(buf + written, codestr, clen);
    written += clen;

    const char *msgpre = ",\"message\":\"";
    int mplen = (int)strlen(msgpre);
    if (written + mplen >= buf_size) return -1;
    memcpy(buf + written, msgpre, mplen);
    written += mplen;

    int mlen = (int)strlen(message);
    if (written + mlen >= buf_size) return -1;
    memcpy(buf + written, message, mlen);
    written += mlen;

    const char *suf = "\"}}";
    int suflen = (int)strlen(suf);
    if (written + suflen >= buf_size) return -1;
    memcpy(buf + written, suf, suflen);
    written += suflen;

    buf[written] = '\0';
    return written;
}

/* ============================================
 * MCP Method Handlers
 * ============================================ */

/* initialize — MCP handshake */
static int handle_initialize(tardy_mcp_server_t *srv, const char *id)
{
    const char *result =
        "{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"tardygrada\",\"version\":\"0.1.0\"}}";
    int len = build_response(srv->write_buf, TARDY_MCP_BUF_SIZE, id, result);
    if (len > 0)
        mcp_write(srv->write_buf, len);
    return 0;
}

/* Emit one tool entry into the buffer. Returns bytes written. */
static int emit_tool(char *buf, int buf_size, int pos, const char *name,
                      tardy_agent_t *agent, int *first)
{
    int tlen = pos;
    if (!*first && tlen < buf_size) buf[tlen++] = ',';
    *first = 0;

    const char *pre = "{\"name\":\"";
    int prelen = (int)strlen(pre);
    if (tlen + prelen >= buf_size) return tlen;
    memcpy(buf + tlen, pre, prelen);
    tlen += prelen;

    int nlen = (int)strlen(name);
    if (tlen + nlen >= buf_size) return tlen;
    memcpy(buf + tlen, name, nlen);
    tlen += nlen;

    const char *mid_desc;
    if (agent && agent->trust >= TARDY_TRUST_SOVEREIGN)
        mid_desc = "\",\"description\":\"sovereign agent\",";
    else if (agent && agent->trust >= TARDY_TRUST_VERIFIED)
        mid_desc = "\",\"description\":\"verified agent\",";
    else if (agent && agent->trust >= TARDY_TRUST_DEFAULT)
        mid_desc = "\",\"description\":\"immutable agent\",";
    else
        mid_desc = "\",\"description\":\"mutable agent\",";

    int mdlen = (int)strlen(mid_desc);
    if (tlen + mdlen >= buf_size) return tlen;
    memcpy(buf + tlen, mid_desc, mdlen);
    tlen += mdlen;

    const char *schema = "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}";
    int slen = (int)strlen(schema);
    if (tlen + slen >= buf_size) return tlen;
    memcpy(buf + tlen, schema, slen);
    tlen += slen;
    return tlen;
}

/* tools/list — expose ALL agents in the tree as tools */
static int handle_tools_list(tardy_mcp_server_t *srv, const char *id)
{
    char tools[4096];
    int tlen = 0;
    tools[tlen++] = '{';
    const char *tp = "\"tools\":[";
    int tplen = (int)strlen(tp);
    memcpy(tools + tlen, tp, tplen);
    tlen += tplen;

    int first = 1;

    /* Walk all agents (skip root at index 0), expose value agents as tools */
    for (int i = 1; i < srv->vm->agent_count; i++) {
        tardy_agent_t *a = &srv->vm->agents[i];
        if (a->state == TARDY_STATE_DEAD)
            continue;
        if (a->type_tag == TARDY_TYPE_AGENT)
            continue; /* skip container agents, expose values only */

        /* Find this agent's name by scanning all parents' contexts */
        const char *name = NULL;
        for (int p = 0; p < srv->vm->agent_count; p++) {
            tardy_agent_t *parent = &srv->vm->agents[p];
            for (int c = 0; c < parent->context.child_count; c++) {
                if (parent->context.children[c].agent_id.hi == a->id.hi &&
                    parent->context.children[c].agent_id.lo == a->id.lo) {
                    name = parent->context.children[c].name;
                    break;
                }
            }
            if (name) break;
        }

        if (name)
            tlen = emit_tool(tools, sizeof(tools), tlen, name, a, &first);
    }

    tools[tlen++] = ']';
    tools[tlen++] = '}';
    tools[tlen] = '\0';

    int len = build_response(srv->write_buf, TARDY_MCP_BUF_SIZE, id, tools);
    if (len > 0)
        mcp_write(srv->write_buf, len);
    return 0;
}

/* tools/call — read an agent's value */
static int handle_tools_call(tardy_mcp_server_t *srv,
                              tardy_json_parser_t *parser,
                              const char *id)
{
    /* Get params.name */
    int params_tok = tardy_json_find(parser, 0, "params");
    if (params_tok < 0) {
        int len = build_error(srv->write_buf, TARDY_MCP_BUF_SIZE,
                               id, -32602, "missing params");
        if (len > 0) mcp_write(srv->write_buf, len);
        return -1;
    }

    int name_tok = tardy_json_find(parser, params_tok, "name");
    if (name_tok < 0) {
        int len = build_error(srv->write_buf, TARDY_MCP_BUF_SIZE,
                               id, -32602, "missing params.name");
        if (len > 0) mcp_write(srv->write_buf, len);
        return -1;
    }

    char tool_name[64];
    tardy_json_str(parser, name_tok, tool_name, sizeof(tool_name));

    /* Find agent by name anywhere in the tree */
    int64_t val = 0;
    tardy_read_status_t status = TARDY_READ_HASH_MISMATCH;

    /* Search all parents for a child with this name */
    for (int i = 0; i < srv->vm->agent_count && status != TARDY_READ_OK; i++) {
        status = tardy_vm_read(srv->vm, srv->vm->agents[i].id,
                               tool_name, &val, sizeof(int64_t));
    }

    if (status == TARDY_READ_OK) {
        /* Build result with value and provenance */
        char result[512];
        int rlen = 0;
        const char *pre = "{\"content\":[{\"type\":\"text\",\"text\":\"";
        int prelen = (int)strlen(pre);
        memcpy(result + rlen, pre, prelen);
        rlen += prelen;

        /* Value as string */
        char numstr[32];
        int nlen = 0;
        int neg = 0;
        int64_t v = val;
        if (v < 0) { neg = 1; v = -v; }
        if (v == 0) numstr[nlen++] = '0';
        while (v > 0) { numstr[nlen++] = '0' + (char)(v % 10); v /= 10; }
        if (neg) numstr[nlen++] = '-';
        /* reverse */
        for (int i = 0; i < nlen / 2; i++) {
            char t = numstr[i];
            numstr[i] = numstr[nlen - 1 - i];
            numstr[nlen - 1 - i] = t;
        }
        memcpy(result + rlen, numstr, nlen);
        rlen += nlen;

        const char *suf = "\"}]}";
        int suflen = (int)strlen(suf);
        memcpy(result + rlen, suf, suflen);
        rlen += suflen;
        result[rlen] = '\0';

        int len = build_response(srv->write_buf, TARDY_MCP_BUF_SIZE,
                                  id, result);
        if (len > 0)
            mcp_write(srv->write_buf, len);
    } else {
        const char *err;
        switch (status) {
        case TARDY_READ_HASH_MISMATCH:  err = "hash verification failed"; break;
        case TARDY_READ_NO_CONSENSUS:   err = "no Byzantine consensus"; break;
        case TARDY_READ_SIG_INVALID:    err = "signature invalid"; break;
        default:                        err = "read failed"; break;
        }
        int len = build_error(srv->write_buf, TARDY_MCP_BUF_SIZE,
                               id, -32000, err);
        if (len > 0) mcp_write(srv->write_buf, len);
    }

    return 0;
}

/* ============================================
 * MCP Server Core
 * ============================================ */

int tardy_mcp_init(tardy_mcp_server_t *srv, tardy_vm_t *vm)
{
    if (!srv || !vm)
        return -1;
    memset(srv, 0, sizeof(tardy_mcp_server_t));
    srv->vm = vm;
    srv->running = 1;
    return 0;
}

int tardy_mcp_handle(tardy_mcp_server_t *srv, const char *request, int len)
{
    tardy_json_parser_t parser;
    if (tardy_json_parse(&parser, request, len) < 0)
        return -1;

    /* Extract method and id */
    int method_tok = tardy_json_find(&parser, 0, "method");
    int id_tok = tardy_json_find(&parser, 0, "id");

    char id_str[32] = "null";
    if (id_tok >= 0) {
        if (parser.tokens[id_tok].type == TARDY_JSON_STRING) {
            id_str[0] = '"';
            tardy_json_str(&parser, id_tok, id_str + 1, sizeof(id_str) - 2);
            int slen = (int)strlen(id_str);
            id_str[slen] = '"';
            id_str[slen + 1] = '\0';
        } else if (parser.tokens[id_tok].type == TARDY_JSON_NUMBER) {
            memcpy(id_str, parser.tokens[id_tok].start,
                   parser.tokens[id_tok].len);
            id_str[parser.tokens[id_tok].len] = '\0';
        }
    }

    if (method_tok < 0)
        return -1;

    /* Route to handler */
    if (tardy_json_eq(&parser, method_tok, "initialize"))
        return handle_initialize(srv, id_str);

    if (tardy_json_eq(&parser, method_tok, "notifications/initialized"))
        return 0; /* notification, no response */

    if (tardy_json_eq(&parser, method_tok, "tools/list"))
        return handle_tools_list(srv, id_str);

    if (tardy_json_eq(&parser, method_tok, "tools/call"))
        return handle_tools_call(srv, &parser, id_str);

    /* Unknown method */
    int rlen = build_error(srv->write_buf, TARDY_MCP_BUF_SIZE,
                            id_str, -32601, "method not found");
    if (rlen > 0)
        mcp_write(srv->write_buf, rlen);
    return 0;
}

int tardy_mcp_run(tardy_mcp_server_t *srv)
{
    while (srv->running) {
        /* Read Content-Length header */
        char hdr[128];
        int hpos = 0;
        int content_length = -1;

        /* Read header line by line */
        while (hpos < (int)sizeof(hdr) - 1) {
            ssize_t n = read(STDIN_FILENO, hdr + hpos, 1);
            if (n <= 0) {
                srv->running = 0;
                return 0; /* EOF — clean shutdown */
            }
            hpos++;

            /* Check for \r\n\r\n (end of headers) */
            if (hpos >= 4 &&
                hdr[hpos - 4] == '\r' && hdr[hpos - 3] == '\n' &&
                hdr[hpos - 2] == '\r' && hdr[hpos - 1] == '\n') {

                /* Parse Content-Length */
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

        if (content_length <= 0 || content_length >= TARDY_MCP_BUF_SIZE)
            continue;

        /* Read body */
        int total = 0;
        while (total < content_length) {
            ssize_t n = read(STDIN_FILENO, srv->read_buf + total,
                            content_length - total);
            if (n <= 0) {
                srv->running = 0;
                return 0;
            }
            total += (int)n;
        }
        srv->read_buf[content_length] = '\0';

        /* Handle the request */
        tardy_mcp_handle(srv, srv->read_buf, content_length);
    }

    return 0;
}

void tardy_mcp_stop(tardy_mcp_server_t *srv)
{
    if (srv)
        srv->running = 0;
}
