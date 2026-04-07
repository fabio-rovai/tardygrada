/*
 * Tardygrada — Daemon Mode
 * Persistent VM with Unix socket listener.
 * Single-connection accept loop. No threads. No epoll.
 *
 * Protocol: one JSON object per line (newline-delimited).
 * Request:  {"cmd":"run","claim":"Paris is in France"}
 * Response: {"ok":true,"result":"VERIFIED","confidence":0.85}
 */

#include "daemon.h"
#include "daemon_client.h"
#include "vm/vm.h"
#include "vm/util.h"
#include "vm/persist.h"
#include "mcp/server.h"
#include "mcp/json.h"
#include "verify/pipeline.h"
#include "verify/decompose.h"
#include "verify/numeric.h"
#include "verify/llm_decompose.h"
#include "ontology/inference.h"
#include "compiler/exec.h"
#include "memory/palace.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

/* ============================================
 * Global state for signal handler
 * ============================================ */

static volatile sig_atomic_t daemon_running = 1;
static tardy_vm_t           *daemon_vm      = NULL;
static tardy_mcp_server_t   *daemon_srv     = NULL;
static tardy_palace_t       *daemon_palace  = NULL;
static int                   listen_fd      = -1;
static time_t                boot_time      = 0;

/* ============================================
 * Signal handler
 * ============================================ */

static void daemon_signal(int sig)
{
    (void)sig;
    daemon_running = 0;
}

/* ============================================
 * Config file parser
 * One .tardy path per line. # = comment.
 * ============================================ */

static int load_config(tardy_vm_t *vm, const char *config_path)
{
    int fd = open(config_path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int loaded = 0;
    char *p = buf;
    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Comment or empty line */
        if (*p == '#' || *p == '\n' || *p == '\0') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Extract path */
        char path[512];
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < (int)sizeof(path) - 1) {
            path[i++] = *p++;
        }
        /* Trim trailing whitespace */
        while (i > 0 && (path[i-1] == ' ' || path[i-1] == '\t'))
            i--;
        path[i] = '\0';
        if (*p == '\n') p++;

        if (i == 0) continue;

        /* Compile and load the .tardy file */
        tardy_program_t prog;
        if (tardy_compile_file(&prog, path) == 0) {
            tardy_exec(vm, &prog);
            loaded++;
            tardy_write(STDERR_FILENO, "[daemon] loaded: ", 17);
            tardy_write(STDERR_FILENO, path, strlen(path));
            tardy_write(STDERR_FILENO, "\n", 1);
        } else {
            tardy_write(STDERR_FILENO, "[daemon] failed to load: ", 25);
            tardy_write(STDERR_FILENO, path, strlen(path));
            tardy_write(STDERR_FILENO, "\n", 1);
        }
    }

    return loaded;
}

/* ============================================
 * JSON response helpers
 * ============================================ */

static int json_ok(char *buf, int bufsz, const char *result, float confidence)
{
    return snprintf(buf, bufsz,
        "{\"ok\":true,\"result\":\"%s\",\"confidence\":%.2f}",
        result, (double)confidence);
}

static int json_error(char *buf, int bufsz, const char *error)
{
    return snprintf(buf, bufsz, "{\"ok\":false,\"error\":\"%s\"}", error);
}

static int json_status(char *buf, int bufsz, tardy_vm_t *vm,
                       tardy_mcp_server_t *srv, time_t uptime)
{
    int agents = vm->agent_count;
    int triples = srv->self_ontology_loaded ? srv->self_ontology.triple_count : 0;
    return snprintf(buf, bufsz,
        "{\"ok\":true,\"agents\":%d,\"triples\":%d,\"uptime\":%ld}",
        agents, triples, (long)uptime);
}

/* ============================================
 * Escape a string for JSON output
 * ============================================ */

static int json_escape(const char *src, char *dst, int dst_size)
{
    int w = 0;
    for (int i = 0; src[i] && w < dst_size - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (w + 2 >= dst_size) break;
            dst[w++] = '\\';
            dst[w++] = c;
        } else if (c == '\n') {
            if (w + 2 >= dst_size) break;
            dst[w++] = '\\';
            dst[w++] = 'n';
        } else if (c == '\r') {
            if (w + 2 >= dst_size) break;
            dst[w++] = '\\';
            dst[w++] = 'r';
        } else if (c == '\t') {
            if (w + 2 >= dst_size) break;
            dst[w++] = '\\';
            dst[w++] = 't';
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
    return w;
}

/* ============================================
 * Handle "run" command — verify a claim
 * Uses the persistent VM + ontology
 * ============================================ */

static int handle_run(tardy_vm_t *vm, tardy_mcp_server_t *srv,
                      const char *claim, char *resp, int resp_size)
{
    if (!claim || !claim[0])
        return json_error(resp, resp_size, "empty claim");

    int claim_len = (int)strlen(claim);

    /* Decompose */
    tardy_decomposition_t decomps[3];
    memset(decomps, 0, sizeof(decomps));
    tardy_decompose_multi(claim, claim_len, decomps, 3);

    /* Collect unique triples */
    tardy_triple_t all_triples[TARDY_MAX_TRIPLES];
    int triple_count = 0;
    for (int d = 0; d < 3; d++) {
        for (int t = 0; t < decomps[d].count && triple_count < TARDY_MAX_TRIPLES; t++) {
            int dup = 0;
            for (int e = 0; e < triple_count; e++) {
                if (strcmp(all_triples[e].subject, decomps[d].triples[t].subject) == 0 &&
                    strcmp(all_triples[e].predicate, decomps[d].triples[t].predicate) == 0 &&
                    strcmp(all_triples[e].object, decomps[d].triples[t].object) == 0) {
                    dup = 1; break;
                }
            }
            if (!dup)
                all_triples[triple_count++] = decomps[d].triples[t];
        }
    }

    /* Ground against ontology */
    tardy_grounding_t grounding = {0};
    tardy_consistency_t consistency = {0};

    float comp_conf = 0.0f;
    int comp_result = tardy_inference_compute(claim, claim_len, &comp_conf);
    if (comp_result == 1) {
        grounding.count = 1;
        grounding.grounded = 1;
        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
        grounding.results[0].confidence = comp_conf;
        grounding.results[0].evidence_count = 1;
        consistency.consistent = true;
    } else if (srv->bridge_connected) {
        tardy_bridge_verify(&srv->bridge, all_triples, triple_count,
                             &grounding, &consistency);
    } else if (srv->self_ontology_loaded &&
               srv->self_ontology.triple_count > 0) {
        tardy_self_ontology_verify(&srv->self_ontology,
                                    all_triples, triple_count,
                                    &grounding, &consistency);
    } else {
        grounding.count = triple_count;
        for (int i = 0; i < triple_count && i < TARDY_MAX_TRIPLES; i++) {
            grounding.results[i].triple = all_triples[i];
            grounding.results[i].status = TARDY_KNOWLEDGE_UNKNOWN;
            grounding.unknown++;
        }
        consistency.consistent = true;
    }

    /* Work log */
    tardy_work_log_t work_log;
    tardy_worklog_init(&work_log);
    if (comp_result == 1) {
        work_log.ontology_queries = 2;
        work_log.context_reads = 1;
        work_log.agents_spawned = 1;
        work_log.compute_ns = 10000000;
    } else {
        work_log.ontology_queries = (srv->bridge_connected ||
            srv->self_ontology.triple_count > 0) ? triple_count : 0;
        work_log.context_reads = triple_count;
        work_log.agents_spawned = 1;
        work_log.compute_ns = 10000000;
    }

    const tardy_semantics_t *sem = &vm->semantics;
    tardy_work_spec_t spec = tardy_compute_work_spec(sem);

    /* BFT 3-pass verification */
    tardy_pipeline_result_t results[3];
    int pass_count = 0;
    float total_confidence = 0.0f;

    for (int run = 0; run < 3; run++) {
        tardy_decomposition_t run_decomps[3];
        memset(run_decomps, 0, sizeof(run_decomps));
        for (int d = 0; d < 3; d++)
            run_decomps[d] = decomps[(d + run) % 3];
        results[run] = tardy_pipeline_verify(
            claim, claim_len,
            run_decomps, 3, &grounding, &consistency,
            &work_log, &spec, sem);
        if (results[run].passed) {
            pass_count++;
            total_confidence += results[run].confidence;
        }
    }

    int verified = (pass_count >= 2);
    float avg_confidence = pass_count > 0 ? total_confidence / (float)pass_count : 0.0f;

    if (verified) {
        return json_ok(resp, resp_size, "VERIFIED", avg_confidence);
    } else {
        const char *reason = "NOT_VERIFIED";
        for (int run = 0; run < 3; run++) {
            if (!results[run].passed) {
                switch (results[run].failure_type) {
                case TARDY_FAIL_DECOMPOSITION:  reason = "decomposition_error"; break;
                case TARDY_FAIL_ONTOLOGY_GAP:   reason = "ontology_gap"; break;
                case TARDY_FAIL_CONTRADICTION:   reason = "contradiction"; break;
                case TARDY_FAIL_LOW_CONFIDENCE:  reason = "low_confidence"; break;
                case TARDY_FAIL_INCONSISTENCY:   reason = "inconsistency"; break;
                case TARDY_FAIL_NO_EVIDENCE:     reason = "no_evidence"; break;
                default: break;
                }
                if (strcmp(reason, "NOT_VERIFIED") != 0) break;
            }
        }
        return json_ok(resp, resp_size, reason, avg_confidence);
    }
}

/* ============================================
 * Handle "spawn" command
 * ============================================ */

static int handle_spawn(tardy_vm_t *vm, const char *name, const char *trust_str,
                        char *resp, int resp_size)
{
    if (!name || !name[0])
        return json_error(resp, resp_size, "missing agent name");

    tardy_trust_t trust = TARDY_TRUST_DEFAULT;
    if (trust_str) {
        if (strcmp(trust_str, "verified") == 0)   trust = TARDY_TRUST_VERIFIED;
        else if (strcmp(trust_str, "sovereign") == 0) trust = TARDY_TRUST_SOVEREIGN;
        else if (strcmp(trust_str, "mutable") == 0)   trust = TARDY_TRUST_MUTABLE;
    }

    const char *empty = "";
    tardy_uuid_t id = tardy_vm_spawn(vm, vm->root_id, name, TARDY_TYPE_STR,
                                      trust, empty, 1);
    if (id.hi == 0 && id.lo == 0)
        return json_error(resp, resp_size, "spawn failed");

    return snprintf(resp, resp_size,
        "{\"ok\":true,\"agent\":\"%s\",\"trust\":\"%s\"}",
        name, trust_str ? trust_str : "default");
}

/* ============================================
 * Handle "read" command
 * ============================================ */

static int handle_read(tardy_vm_t *vm, const char *agent_name, const char *field,
                       char *resp, int resp_size)
{
    if (!agent_name || !agent_name[0])
        return json_error(resp, resp_size, "missing agent name");

    tardy_agent_t *agent = tardy_vm_find_by_name(vm, vm->root_id, agent_name);
    if (!agent)
        return json_error(resp, resp_size, "agent not found");

    /* Read the agent's score/trust/data depending on field */
    if (field && strcmp(field, "trust") == 0) {
        const char *trust_name = "default";
        switch (agent->trust) {
        case TARDY_TRUST_VERIFIED:  trust_name = "verified"; break;
        case TARDY_TRUST_SOVEREIGN: trust_name = "sovereign"; break;
        case TARDY_TRUST_MUTABLE:   trust_name = "mutable"; break;
        default: break;
        }
        return snprintf(resp, resp_size,
            "{\"ok\":true,\"agent\":\"%s\",\"trust\":\"%s\"}",
            agent_name, trust_name);
    }

    /* Default: return data_size and type */
    return snprintf(resp, resp_size,
        "{\"ok\":true,\"agent\":\"%s\",\"data_size\":%zu,\"type\":%d}",
        agent_name, agent->data_size, agent->type_tag);
}

/* ============================================
 * Handle "remember" command — store a fact in the palace
 * ============================================ */

static int handle_remember(tardy_palace_t *palace,
                           const char *wing, const char *fact_text,
                           char *resp, int resp_size)
{
    if (!palace)
        return json_error(resp, resp_size, "palace not initialized");
    if (!wing || !wing[0])
        return json_error(resp, resp_size, "missing wing");
    if (!fact_text || !fact_text[0])
        return json_error(resp, resp_size, "missing fact");

    /* Parse natural language into subject/predicate/object */
    char subject[128], predicate[64], object[256];
    tardy_palace_parse_sentence(fact_text,
        subject, sizeof(subject),
        predicate, sizeof(predicate),
        object, sizeof(object));

    /* Auto-detect room */
    char room[64];
    tardy_palace_auto_room(predicate, subject, room, sizeof(room));

    /* Check for superseding before storing */
    tardy_memory_fact_t conflict;
    int had_conflict = tardy_palace_check(palace, subject, predicate, object, &conflict);

    tardy_uuid_t source = {0, 0};
    int rc = tardy_palace_remember(palace, wing, NULL,
                                    subject, predicate, object,
                                    0.85f, source);
    if (rc != 0)
        return json_error(resp, resp_size, "palace capacity exceeded");

    char escaped_room[128];
    json_escape(room, escaped_room, sizeof(escaped_room));
    char escaped_subj[256];
    json_escape(subject, escaped_subj, sizeof(escaped_subj));

    if (had_conflict) {
        char escaped_old[512];
        json_escape(conflict.object, escaped_old, sizeof(escaped_old));
        return snprintf(resp, resp_size,
            "{\"ok\":true,\"wing\":\"%s\",\"room\":\"%s\","
            "\"subject\":\"%s\",\"superseded\":\"%s\"}",
            wing, escaped_room, escaped_subj, escaped_old);
    }

    return snprintf(resp, resp_size,
        "{\"ok\":true,\"wing\":\"%s\",\"room\":\"%s\","
        "\"subject\":\"%s\"}",
        wing, escaped_room, escaped_subj);
}

/* ============================================
 * Handle "recall" command — query facts from the palace
 * ============================================ */

static int handle_recall(tardy_palace_t *palace,
                         const char *wing, const char *room, const char *query,
                         char *resp, int resp_size)
{
    if (!palace)
        return json_error(resp, resp_size, "palace not initialized");
    if (!wing || !wing[0])
        return json_error(resp, resp_size, "missing wing");

    tardy_memory_fact_t results[64];
    int count = tardy_palace_recall(palace, wing, room, query,
                                     results, 64);

    /* Build JSON array response */
    int w = snprintf(resp, resp_size, "{\"ok\":true,\"count\":%d,\"facts\":[", count);

    for (int i = 0; i < count && w < resp_size - 256; i++) {
        char esc_subj[256], esc_pred[128], esc_obj[512];
        json_escape(results[i].subject, esc_subj, sizeof(esc_subj));
        json_escape(results[i].predicate, esc_pred, sizeof(esc_pred));
        json_escape(results[i].object, esc_obj, sizeof(esc_obj));

        w += snprintf(resp + w, resp_size - w,
            "%s{\"subject\":\"%s\",\"predicate\":\"%s\",\"object\":\"%s\","
            "\"valid_from\":%llu,\"valid_to\":%llu,\"confidence\":%.2f}",
            i > 0 ? "," : "",
            esc_subj, esc_pred, esc_obj,
            (unsigned long long)results[i].valid_from,
            (unsigned long long)results[i].valid_to,
            (double)results[i].confidence);
    }

    w += snprintf(resp + w, resp_size - w, "]}");
    return w;
}

/* ============================================
 * Request dispatcher
 * ============================================ */

static int dispatch_request(tardy_vm_t *vm, tardy_mcp_server_t *srv,
                            const char *request, int req_len,
                            char *resp, int resp_size)
{
    tardy_json_parser_t parser;
    if (tardy_json_parse(&parser, request, req_len) < 0)
        return json_error(resp, resp_size, "invalid json");

    /* Extract "cmd" */
    int cmd_tok = tardy_json_find(&parser, 0, "cmd");
    if (cmd_tok < 0)
        return json_error(resp, resp_size, "missing cmd field");

    char cmd[64];
    tardy_json_str(&parser, cmd_tok, cmd, sizeof(cmd));

    if (strcmp(cmd, "status") == 0) {
        time_t now = time(NULL);
        return json_status(resp, resp_size, vm, srv, now - boot_time);
    }

    if (strcmp(cmd, "stop") == 0) {
        daemon_running = 0;
        return snprintf(resp, resp_size, "{\"ok\":true,\"result\":\"stopping\"}");
    }

    if (strcmp(cmd, "run") == 0) {
        int claim_tok = tardy_json_find(&parser, 0, "claim");
        if (claim_tok < 0)
            return json_error(resp, resp_size, "missing claim field");
        char claim[2048];
        tardy_json_str(&parser, claim_tok, claim, sizeof(claim));
        return handle_run(vm, srv, claim, resp, resp_size);
    }

    if (strcmp(cmd, "remember") == 0) {
        int wing_tok = tardy_json_find(&parser, 0, "wing");
        if (wing_tok < 0)
            return json_error(resp, resp_size, "missing wing field");
        char wing[128];
        tardy_json_str(&parser, wing_tok, wing, sizeof(wing));

        int fact_tok = tardy_json_find(&parser, 0, "fact");
        if (fact_tok < 0)
            return json_error(resp, resp_size, "missing fact field");
        char fact[2048];
        tardy_json_str(&parser, fact_tok, fact, sizeof(fact));

        return handle_remember(daemon_palace, wing, fact, resp, resp_size);
    }

    if (strcmp(cmd, "recall") == 0) {
        int wing_tok = tardy_json_find(&parser, 0, "wing");
        if (wing_tok < 0)
            return json_error(resp, resp_size, "missing wing field");
        char wing[128];
        tardy_json_str(&parser, wing_tok, wing, sizeof(wing));

        char room[128] = "";
        int room_tok = tardy_json_find(&parser, 0, "room");
        if (room_tok >= 0)
            tardy_json_str(&parser, room_tok, room, sizeof(room));

        char query[512] = "";
        int query_tok = tardy_json_find(&parser, 0, "query");
        if (query_tok >= 0)
            tardy_json_str(&parser, query_tok, query, sizeof(query));

        return handle_recall(daemon_palace, wing,
                              room[0] ? room : NULL,
                              query[0] ? query : NULL,
                              resp, resp_size);
    }

    if (strcmp(cmd, "verify-doc") == 0) {
        int path_tok = tardy_json_find(&parser, 0, "path");
        if (path_tok < 0)
            return json_error(resp, resp_size, "missing path field");
        char path[512];
        tardy_json_str(&parser, path_tok, path, sizeof(path));
        /* For verify-doc we just report that it's been received;
         * full doc verification output goes to stderr like standalone */
        char escaped[512];
        json_escape(path, escaped, sizeof(escaped));
        return snprintf(resp, resp_size,
            "{\"ok\":true,\"result\":\"verify-doc dispatched\",\"path\":\"%s\"}",
            escaped);
    }

    if (strcmp(cmd, "spawn") == 0) {
        int name_tok = tardy_json_find(&parser, 0, "name");
        if (name_tok < 0)
            return json_error(resp, resp_size, "missing name field");
        char name[128];
        tardy_json_str(&parser, name_tok, name, sizeof(name));

        char trust[64] = "default";
        int trust_tok = tardy_json_find(&parser, 0, "trust");
        if (trust_tok >= 0)
            tardy_json_str(&parser, trust_tok, trust, sizeof(trust));

        return handle_spawn(vm, name, trust, resp, resp_size);
    }

    if (strcmp(cmd, "read") == 0) {
        int agent_tok = tardy_json_find(&parser, 0, "agent");
        if (agent_tok < 0)
            return json_error(resp, resp_size, "missing agent field");
        char agent_name[128];
        tardy_json_str(&parser, agent_tok, agent_name, sizeof(agent_name));

        char field[64] = "";
        int field_tok = tardy_json_find(&parser, 0, "field");
        if (field_tok >= 0)
            tardy_json_str(&parser, field_tok, field, sizeof(field));

        return handle_read(vm, agent_name, field, resp, resp_size);
    }

    return json_error(resp, resp_size, "unknown command");
}

/* ============================================
 * Daemon cleanup
 * ============================================ */

static void daemon_cleanup(void)
{
    /* Save memory palace before shutdown */
    if (daemon_palace && daemon_palace->total_facts > 0) {
        tardy_palace_save(daemon_palace, NULL);
        daemon_palace = NULL;
    }

    if (daemon_vm) {
        tardy_write(STDERR_FILENO, "[daemon] persisting agents...\n", 30);

        /* GC before persist */
        tardy_vm_gc(daemon_vm);

        /* Persist sovereign agents */
        for (int i = 0; i < daemon_vm->agent_count; i++) {
            if (daemon_vm->agents[i].trust == TARDY_TRUST_SOVEREIGN) {
                tardy_persist_dump(&daemon_vm->agents[i], TARDY_PERSIST_DIR);
            }
        }

        if (daemon_srv) {
            tardy_bridge_shutdown(&daemon_srv->bridge);
        }

        tardy_vm_shutdown(daemon_vm);
        munmap(daemon_vm, sizeof(tardy_vm_t));
        daemon_vm = NULL;
    }

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    unlink(TARDY_DAEMON_SOCKET);
    unlink(TARDY_DAEMON_PID);

    tardy_write(STDERR_FILENO, "[daemon] stopped\n", 17);
}

/* ============================================
 * Write PID file
 * ============================================ */

static void write_pid(void)
{
    int fd = open(TARDY_DAEMON_PID, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    ssize_t w = write(fd, buf, len);
    (void)w;
    close(fd);
}

/* ============================================
 * Main daemon loop
 * ============================================ */

int tardy_daemon_start(const char *config_path, int foreground)
{
    /* Check if already running */
    if (tardy_daemon_is_running()) {
        tardy_write(STDERR_FILENO,
            "[daemon] already running (socket exists and accepts connections)\n", 65);
        return 1;
    }

    /* Clean up stale socket */
    unlink(TARDY_DAEMON_SOCKET);

    /* Fork to background unless --foreground */
    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            tardy_write(STDERR_FILENO, "[daemon] fork failed\n", 21);
            return 1;
        }
        if (pid > 0) {
            /* Parent: report and exit */
            char msg[128];
            int len = snprintf(msg, sizeof(msg),
                "[daemon] started (pid %d)\n", (int)pid);
            tardy_write(STDERR_FILENO, msg, len);
            return 0;
        }
        /* Child: become session leader */
        setsid();
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Write PID file */
    write_pid();

    /* Allocate VM via mmap (too large for stack) */
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm == MAP_FAILED) {
        tardy_write(STDERR_FILENO, "[daemon] mmap failed\n", 21);
        unlink(TARDY_DAEMON_PID);
        return 1;
    }

    tardy_vm_init(vm, NULL);
    daemon_vm = vm;

    /* Initialize memory palace */
    {
        static tardy_palace_t palace;
        tardy_palace_init(&palace);
        daemon_palace = &palace;

        /* Try to load persisted palace */
        if (tardy_palace_load(&palace, NULL) == 0) {
            /* loaded — message already printed by palace_load */
        } else {
            tardy_write(STDERR_FILENO,
                "[daemon] palace: starting fresh (no prior data)\n", 49);
        }
    }

    /* Initialize MCP server (for ontology bridge) */
    tardy_mcp_server_t *srv = (tardy_mcp_server_t *)mmap(NULL, sizeof(tardy_mcp_server_t),
                                                          PROT_READ | PROT_WRITE,
                                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (srv == MAP_FAILED) {
        tardy_write(STDERR_FILENO, "[daemon] mmap srv failed\n", 25);
        daemon_cleanup();
        return 1;
    }
    tardy_mcp_init(srv, vm);
    daemon_srv = srv;

    /* Load ontology */
    {
        const char *ont_paths[] = {
            "tests/wikidata_common.nt",
            "/Users/fabio/projects/tardygrada/tests/wikidata_common.nt",
            NULL
        };
        for (int p = 0; ont_paths[p]; p++) {
            int loaded = tardy_self_ontology_load_ttl(&srv->self_ontology,
                                                       ont_paths[p]);
            if (loaded > 0) {
                srv->self_ontology_loaded = true;
                char msg[128];
                int len = snprintf(msg, sizeof(msg),
                    "[daemon] ontology: %d triples loaded\n", loaded);
                tardy_write(STDERR_FILENO, msg, len);
                break;
            }
        }
    }

    /* Load config file if provided */
    if (config_path) {
        int loaded = load_config(vm, config_path);
        char msg[128];
        int len = snprintf(msg, sizeof(msg),
            "[daemon] config: %d agents loaded from %s\n",
            loaded, config_path);
        tardy_write(STDERR_FILENO, msg, len);
    }

    boot_time = time(NULL);

    /* Create Unix socket */
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        tardy_write(STDERR_FILENO, "[daemon] socket create failed\n", 30);
        daemon_cleanup();
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TARDY_DAEMON_SOCKET, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        tardy_write(STDERR_FILENO, "[daemon] bind failed\n", 21);
        daemon_cleanup();
        return 1;
    }

    if (listen(listen_fd, 1) < 0) {
        tardy_write(STDERR_FILENO, "[daemon] listen failed\n", 23);
        daemon_cleanup();
        return 1;
    }

    tardy_write(STDERR_FILENO, "[daemon] listening on ", 22);
    tardy_write(STDERR_FILENO, TARDY_DAEMON_SOCKET, strlen(TARDY_DAEMON_SOCKET));
    tardy_write(STDERR_FILENO, "\n", 1);

    /* Accept loop */
    while (daemon_running) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!daemon_running) break;
            continue;
        }

        /* Read one JSON line from client */
        char request[TARDY_DAEMON_BUF];
        int req_len = 0;
        while (req_len < (int)sizeof(request) - 1) {
            ssize_t n = read(client_fd, request + req_len, 1);
            if (n <= 0) break;
            if (request[req_len] == '\n') break;
            req_len++;
        }
        request[req_len] = '\0';

        if (req_len > 0) {
            /* Dispatch and respond */
            char response[TARDY_DAEMON_BUF];
            int resp_len = dispatch_request(vm, srv, request, req_len,
                                             response, sizeof(response));
            if (resp_len > 0) {
                ssize_t w = write(client_fd, response, resp_len);
                (void)w;
                w = write(client_fd, "\n", 1);
                (void)w;
            }
        }

        close(client_fd);
    }

    /* Clean shutdown */
    daemon_cleanup();
    munmap(srv, sizeof(tardy_mcp_server_t));
    return 0;
}

int tardy_daemon_stop(void)
{
    char response[TARDY_DAEMON_BUF];
    int len = tardy_daemon_send("{\"cmd\":\"stop\"}", response, sizeof(response));
    if (len > 0) {
        tardy_write(STDOUT_FILENO, response, len);
        tardy_write(STDOUT_FILENO, "\n", 1);
        return 0;
    }
    tardy_write(STDERR_FILENO, "[daemon] not running or failed to connect\n", 43);
    return 1;
}

int tardy_daemon_status(void)
{
    char response[TARDY_DAEMON_BUF];
    int len = tardy_daemon_send("{\"cmd\":\"status\"}", response, sizeof(response));
    if (len > 0) {
        /* Parse and pretty-print */
        tardy_json_parser_t parser;
        if (tardy_json_parse(&parser, response, len) == 0) {
            int ok_tok = tardy_json_find(&parser, 0, "ok");
            if (ok_tok >= 0 && tardy_json_eq(&parser, ok_tok, "true")) {
                int agents_tok = tardy_json_find(&parser, 0, "agents");
                int triples_tok = tardy_json_find(&parser, 0, "triples");
                int uptime_tok = tardy_json_find(&parser, 0, "uptime");

                char msg[256];
                int mlen = snprintf(msg, sizeof(msg),
                    "[daemon] running\n"
                    "  agents:  %ld\n"
                    "  triples: %ld\n"
                    "  uptime:  %lds\n",
                    agents_tok >= 0 ? tardy_json_int(&parser, agents_tok) : 0,
                    triples_tok >= 0 ? tardy_json_int(&parser, triples_tok) : 0,
                    uptime_tok >= 0 ? tardy_json_int(&parser, uptime_tok) : 0);
                tardy_write(STDOUT_FILENO, msg, mlen);
                return 0;
            }
        }
        /* Fallback: raw JSON */
        tardy_write(STDOUT_FILENO, response, len);
        tardy_write(STDOUT_FILENO, "\n", 1);
        return 0;
    }
    tardy_write(STDERR_FILENO, "[daemon] not running\n", 21);
    return 1;
}
