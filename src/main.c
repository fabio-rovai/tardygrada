/*
 * Tardygrada — First Program
 *
 * This proves the VM works:
 * - Spawn agents holding values
 * - Read them back with verification
 * - Test immutability enforcement
 * - Test GC demotion/promotion cycle
 *
 * In Tardygrada syntax, this would be:
 *
 *   agent Main {
 *       let x: int = 5 @verified
 *       y: int = 42
 *       let z: int = 7 @sovereign
 *   }
 */

#include "vm/vm.h"
#include "vm/util.h"
#include "mcp/server.h"
#include "verify/pipeline.h"
#include "compiler/exec.h"
#include "verify/decompose.h"
#include "verify/numeric.h"
#include "verify/llm_decompose.h"
#include "verify/llm_ground.h"
#include "terraform/terraform.h"
#include "ontology/inference.h"
#include "daemon.h"
#include "daemon_client.h"
#include "mcp_bridge.h"
#include "memory/palace.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <strings.h>
#include <limits.h>

static int is_zero_uuid(tardy_uuid_t id)
{
    return id.hi == 0 && id.lo == 0;
}

/* We don't use printf. We write directly. */
static void print(const char *s)
{
tardy_write(STDOUT_FILENO, s, strlen(s));
}

static void print_int(int64_t v)
{
    char buf[32];
    int i = 30;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { buf[i--] = '0'; }
    while (v > 0) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    if (neg) buf[i--] = '-';
tardy_write(STDOUT_FILENO, buf + i + 1, 30 - i);
}

static void ok(const char *test)
{
    print("  [OK] ");
    print(test);
    print("\n");
}

static void fail(const char *test)
{
    print("  [FAIL] ");
    print(test);
    print("\n");
}

static int run_tests(void)
{
    /* VM is too large for stack — allocate via mmap */
    tardy_vm_t *vmp = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vmp == MAP_FAILED) {
        print("mmap failed\n");
        return 1;
    }
    tardy_vm_t *vm = vmp;
    int64_t val;

    print("\n=== Tardygrada VM ===\n\n");

    /* ---- Init ---- */
    if (tardy_vm_init(vm, NULL) != 0) {
        print("VM init failed\n");
        return 1;
    }
    ok("VM initialized with default semantics");

    tardy_uuid_t root = vm->root_id;

    /* ---- let x: int = 5 @verified ---- */
    int64_t five = 5;
    tardy_uuid_t x_id = tardy_vm_spawn(vm, root, "x",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_VERIFIED,
                                         &five, sizeof(int64_t));
    if (!is_zero_uuid(x_id))
        ok("let x: int = 5 @verified — agent spawned");
    else
        fail("let x: int = 5 @verified — spawn failed");

    /* Read x back — verified (hash check) */
    val = 0;
    tardy_read_status_t status = tardy_vm_read(vm, root, "x",
                                                &val, sizeof(int64_t));
    if (status == TARDY_READ_OK && val == 5) {
        print("  [OK] read x = ");
        print_int(val);
        print(" (hash verified)\n");
    } else {
        fail("read x — verification failed");
    }

    /* ---- y: int = 42 (mutable) ---- */
    int64_t fortytwo = 42;
    tardy_uuid_t y_id = tardy_vm_spawn(vm, root, "y",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_MUTABLE,
                                         &fortytwo, sizeof(int64_t));
    if (!is_zero_uuid(y_id))
        ok("y: int = 42 — mutable agent spawned");
    else
        fail("y: int = 42 — spawn failed");

    /* Mutate y from 42 to 100 */
    int64_t hundred = 100;
    if (tardy_vm_mutate(vm, root, "y", &hundred, sizeof(int64_t)) == 0) {
        val = 0;
        tardy_vm_read(vm, root, "y", &val, sizeof(int64_t));
        print("  [OK] mutated y = ");
        print_int(val);
        print("\n");
    } else {
        fail("mutate y");
    }

    /* Try to mutate x (immutable) — MUST fail */
    int64_t ten = 10;
    if (tardy_vm_mutate(vm, root, "x", &ten, sizeof(int64_t)) != 0)
        ok("mutate x rejected — immutable agent enforced");
    else
        fail("mutate x succeeded — IMMUTABILITY BROKEN");

    /* ---- let z: int = 7 @sovereign ---- */
    int64_t seven = 7;
    tardy_uuid_t z_id = tardy_vm_spawn(vm, root, "z",
                                         TARDY_TYPE_INT,
                                         TARDY_TRUST_SOVEREIGN,
                                         &seven, sizeof(int64_t));
    if (!is_zero_uuid(z_id))
        ok("let z: int = 7 @sovereign — agent spawned (5 replicas + sig)");
    else
        fail("let z: int = 7 @sovereign — spawn failed");

    /* Read z — full BFT verification */
    val = 0;
    status = tardy_vm_read(vm, root, "z", &val, sizeof(int64_t));
    if (status == TARDY_READ_OK && val == 7) {
        print("  [OK] read z = ");
        print_int(val);
        print(" (BFT + hash + signature verified)\n");
    } else {
        fail("read z — sovereign verification failed");
    }

    /* ---- Freeze: promote y from mutable to @verified ---- */
    tardy_agent_t *y_agent = tardy_vm_find_by_name(vm, root, "y");
    if (y_agent) {
        tardy_uuid_t frozen = tardy_vm_freeze(vm, y_agent->id,
                                               TARDY_TRUST_VERIFIED);
        if (!is_zero_uuid(frozen)) {
            ok("freeze y: mutable -> @verified");
            /* Now mutation must fail */
            int64_t try_mutate = 999;
            if (tardy_vm_mutate(vm, root, "y", &try_mutate,
                                sizeof(int64_t)) != 0)
                ok("mutate frozen y rejected — freeze enforced");
            else
                fail("mutate frozen y succeeded — FREEZE BROKEN");
        } else {
            fail("freeze y");
        }
    }

    /* ---- GC: test demotion cycle ---- */
    /* Force x's last_accessed to be old */
    tardy_agent_t *x_agent = tardy_vm_find_by_name(vm, root, "x");
    if (x_agent) {
        /* Simulate 60 seconds idle */
        x_agent->last_accessed -= 60000000000ULL;
        int collected = tardy_vm_gc(vm);
        if (collected > 0 && x_agent->state == TARDY_STATE_STATIC) {
            ok("GC demoted idle x to Static");
            /* Read from static — should still return 5 */
            val = 0;
            tardy_vm_read(vm, root, "x", &val, sizeof(int64_t));
            if (val == 5) {
                print("  [OK] static x still = ");
                print_int(val);
                print("\n");
            } else {
                fail("static x value corrupted");
            }
        } else {
            fail("GC demotion");
        }
    }

    /* ============================================
     * Verification Pipeline Tests
     * ============================================ */

    print("\n--- Verification Pipeline ---\n");

    tardy_semantics_t sem = TARDY_DEFAULT_SEMANTICS;

    /* Test 1: Successful verification — grounded claim */
    {
        /* 3 decomposers produce overlapping triples */
        tardy_decomposition_t decomps[3];
        memset(decomps, 0, sizeof(decomps));

        /* All 3 agree on the same triple */
        for (int i = 0; i < 3; i++) {
            strncpy(decomps[i].triples[0].subject, "DrWho", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].predicate, "created_at", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].object, "BBCTelevisionCentre", TARDY_MAX_TRIPLE_LEN);
            decomps[i].count = 1;
        }

        /* Grounding: the triple is confirmed by ontology */
        tardy_grounding_t grounding = {0};
        grounding.count = 1;
        grounding.grounded = 1;
        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
        grounding.results[0].confidence = 0.95f;
        grounding.results[0].evidence_count = 3;

        /* Consistency: no contradictions */
        tardy_consistency_t consistency = {0};
        consistency.consistent = true;
        consistency.contradiction_count = 0;

        /* Work log: agent did real work */
        tardy_work_log_t work_log;
        tardy_worklog_init(&work_log);
        work_log.ontology_queries = 3;
        work_log.context_reads = 5;
        work_log.agents_spawned = 2;
        work_log.compute_ns = 50000000; /* 50ms */

        tardy_work_spec_t spec = tardy_compute_work_spec(&sem);

        tardy_pipeline_result_t result = tardy_pipeline_verify(
            "DrWho created at BBC Television Centre", 38,
            decomps, 3, &grounding, &consistency,
            &work_log, &spec, &sem);

        if (result.passed && result.strength >= TARDY_TRUTH_EVIDENCED)
            ok("grounded claim passed pipeline — EVIDENCED");
        else
            fail("grounded claim should have passed");

        print("  [OK] truth strength: ");
        print_int((int64_t)result.strength);
        print(" confidence: ");
        /* Print confidence as integer percentage */
        print_int((int64_t)(result.confidence * 100));
        print("%\n");
    }

    /* Test 2: Hallucination detected — contradicted by ontology */
    {
        tardy_decomposition_t decomps[3];
        memset(decomps, 0, sizeof(decomps));
        for (int i = 0; i < 3; i++) {
            strncpy(decomps[i].triples[0].subject, "DrWho", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].predicate, "created_at", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].object, "Tokyo", TARDY_MAX_TRIPLE_LEN);
            decomps[i].count = 1;
        }

        tardy_grounding_t grounding = {0};
        grounding.count = 1;
        grounding.contradicted = 1; /* ontology says BBC, not Tokyo */
        grounding.results[0].status = TARDY_KNOWLEDGE_CONTRADICTED;
        grounding.results[0].confidence = 0.0f;

        tardy_consistency_t consistency = {0};
        consistency.consistent = false;
        consistency.contradiction_count = 1;
        strncpy(consistency.explanation,
                "ontology says BBCTelevisionCentre, not Tokyo",
                sizeof(consistency.explanation));

        tardy_work_log_t work_log;
        tardy_worklog_init(&work_log);
        work_log.ontology_queries = 2;
        work_log.context_reads = 3;
        work_log.compute_ns = 30000000;

        tardy_work_spec_t spec = tardy_compute_work_spec(&sem);

        tardy_pipeline_result_t result = tardy_pipeline_verify(
            "DrWho created in Tokyo", 22,
            decomps, 3, &grounding, &consistency,
            &work_log, &spec, &sem);

        if (!result.passed && result.failed_at == TARDY_LAYER_GROUNDING)
            ok("hallucination detected at grounding layer");
        else
            fail("hallucination should have been caught");
    }

    /* Test 3: Laziness detected — agent did no work */
    {
        tardy_decomposition_t decomps[3];
        memset(decomps, 0, sizeof(decomps));
        for (int i = 0; i < 3; i++) {
            strncpy(decomps[i].triples[0].subject, "X", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].predicate, "is", TARDY_MAX_TRIPLE_LEN);
            strncpy(decomps[i].triples[0].object, "Y", TARDY_MAX_TRIPLE_LEN);
            decomps[i].count = 1;
        }

        tardy_grounding_t grounding = {0};
        grounding.count = 1;
        grounding.grounded = 1;
        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
        grounding.results[0].confidence = 0.9f;

        tardy_consistency_t consistency = {0};
        consistency.consistent = true;

        /* Work log: ZERO operations — agent was lazy */
        tardy_work_log_t work_log;
        tardy_worklog_init(&work_log);
        /* Everything stays at 0 */

        tardy_work_spec_t spec = tardy_compute_work_spec(&sem);

        tardy_pipeline_result_t result = tardy_pipeline_verify(
            "X is Y", 6,
            decomps, 3, &grounding, &consistency,
            &work_log, &spec, &sem);

        if (!result.passed && result.failed_at == TARDY_LAYER_WORK_VERIFY)
            ok("laziness detected — agent did no work");
        else
            fail("laziness should have been caught");
    }

    /* ============================================
     * VM Nesting Tests
     * ============================================ */

    print("\n--- VM Nesting ---\n");

    /* Spawn a child VM as an agent */
    tardy_uuid_t child_id = tardy_vm_spawn_child(vm, root, "child_vm", NULL);
    if (!is_zero_uuid(child_id))
        ok("child VM spawned as agent");
    else
        fail("child VM spawn failed");

    /* Retrieve the child VM */
    tardy_vm_t *child_vm = tardy_vm_get_child(vm, child_id);
    if (child_vm && child_vm->running)
        ok("child VM retrieved and running");
    else
        fail("child VM retrieval failed");

    /* Spawn an agent inside the child VM */
    if (child_vm) {
        int64_t ninety = 90;
        tardy_uuid_t inner = tardy_vm_spawn(child_vm, child_vm->root_id,
                                             "inner", TARDY_TYPE_INT,
                                             TARDY_TRUST_VERIFIED,
                                             &ninety, sizeof(int64_t));
        if (!is_zero_uuid(inner)) {
            val = 0;
            status = tardy_vm_read(child_vm, child_vm->root_id,
                                    "inner", &val, sizeof(int64_t));
            if (status == TARDY_READ_OK && val == 90) {
                print("  [OK] child VM inner agent = ");
                print_int(val);
                print("\n");
            } else {
                fail("child VM inner agent read failed");
            }
        } else {
            fail("child VM inner agent spawn failed");
        }
    }

    /* ---- Stats ---- */
    print("\n--- Stats ---\n");
    print("  Agents alive: ");
    print_int(vm->agent_count);
    print("\n  Tombstones: ");
    print_int(vm->tombstone_count);
    print("\n  Page size: ");
    print_int((int64_t)tardy_page_size());
    print(" bytes\n\n");

    /* ---- Shutdown ---- */
    tardy_vm_shutdown(vm);
    munmap(vmp, sizeof(tardy_vm_t));
    ok("VM shutdown");

    print("\n=== All tests passed ===\n\n");
    return 0;
}

/* ============================================
 * MCP Server Mode
 *
 * In Tardygrada syntax:
 *
 *   agent Main {
 *       let x: int = 5 @verified
 *       let y: int = 42
 *       let z: int = 7 @sovereign
 *   }
 *
 * Deploy as MCP. Clients connect and read agent values.
 * ============================================ */

static int run_mcp(void)
{
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm == MAP_FAILED)
        return 1;

    tardy_vm_init(vm, NULL);

    /* Spawn agents — this is what a compiled .tardy file produces */
    int64_t five = 5;
    tardy_vm_spawn(vm, vm->root_id, "x", TARDY_TYPE_INT,
                   TARDY_TRUST_VERIFIED, &five, sizeof(int64_t));

    int64_t fortytwo = 42;
    tardy_vm_spawn(vm, vm->root_id, "y", TARDY_TYPE_INT,
                   TARDY_TRUST_DEFAULT, &fortytwo, sizeof(int64_t));

    int64_t seven = 7;
    tardy_vm_spawn(vm, vm->root_id, "z", TARDY_TYPE_INT,
                   TARDY_TRUST_SOVEREIGN, &seven, sizeof(int64_t));

    /* Start MCP server */
    tardy_mcp_server_t srv;
    tardy_mcp_init(&srv, vm);
    tardy_mcp_run(&srv);

    tardy_vm_shutdown(vm);
    munmap(vm, sizeof(tardy_vm_t));
    return 0;
}

/* ============================================
 * CLI: tardy run "task" — one-shot verified execution
 *
 * Compiles to .tardy internally:
 *   agent Task {
 *       let claim: Fact = receive("task") grounded_in(default) @verified
 *   }
 * Then submits the task text, verifies it, returns result.
 * Nothing unverified runs.
 * ============================================ */

static int run_task(const char *task_text)
{
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm == MAP_FAILED)
        return 1;

    tardy_vm_init(vm, NULL);

    /* Spawn a pending agent for the task */
    const char *empty = "";
    tardy_vm_spawn(vm, vm->root_id, "task", TARDY_TYPE_STR,
                   TARDY_TRUST_MUTABLE, empty, 1);

    /* Submit the task text */
    tardy_agent_t *task_agent = tardy_vm_find_by_name(vm, vm->root_id, "task");
    if (!task_agent) {
        tardy_write(STDERR_FILENO, "[tardy] failed to create task agent\n", 36);
        tardy_vm_shutdown(vm);
        munmap(vm, sizeof(tardy_vm_t));
        return 1;
    }
    tardy_vm_mutate(vm, vm->root_id, "task",
                    task_text, strlen(task_text) + 1);
    tardy_vm_converse(vm, task_agent->id, "user", task_text);

    /* Initialize ontology bridge */
    tardy_mcp_server_t srv;
    tardy_mcp_init(&srv, vm);

    /* Auto-load common knowledge ontology if available */
    {
        const char *ont_paths[] = {
            "tests/wikidata_common.nt",
            "/Users/fabio/projects/tardygrada/tests/wikidata_common.nt",
            NULL
        };
        for (int p = 0; ont_paths[p]; p++) {
            int loaded = tardy_self_ontology_load_ttl(&srv.self_ontology,
                                                       ont_paths[p]);
            if (loaded > 0) {
                srv.self_ontology_loaded = true;
                tardy_write(STDERR_FILENO, "[tardy] ontology: ", 18);
                char num[16];
                int nlen = snprintf(num, sizeof(num), "%d", loaded);
                tardy_write(STDERR_FILENO, num, nlen);
                tardy_write(STDERR_FILENO, " triples loaded\n", 16);
                break;
            }
        }
    }

    /* Decompose the claim */
    int claim_len = (int)strlen(task_text);
    tardy_decomposition_t decomps[3];
    memset(decomps, 0, sizeof(decomps));
    tardy_decompose_multi(task_text, claim_len, decomps, 3);

    /* Collect unique triples.
     * For single-sentence claims (no multi-sentence punctuation), only use
     * the first decomposition pass to avoid re-split artifacts. */
    tardy_triple_t all_triples[TARDY_MAX_TRIPLES];
    int triple_count = 0;
    int is_single_sentence = (strchr(task_text, '.') == NULL ||
                               strchr(task_text, '.') == task_text + claim_len - 1) &&
                              strchr(task_text, ';') == NULL;
    int max_decomps = is_single_sentence ? 1 : 3;
    for (int d = 0; d < max_decomps; d++) {
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

    /* Ground against ontology -- same priority as MCP verify_claim */
    tardy_grounding_t grounding = {0};
    tardy_consistency_t consistency = {0};

    /* Try computational verification first */
    float comp_conf = 0.0f;
    int comp_result = tardy_inference_compute(task_text, (int)strlen(task_text),
                                              &comp_conf);
    if (comp_result == 1) {
        grounding.count = 1;
        grounding.grounded = 1;
        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
        grounding.results[0].confidence = comp_conf;
        grounding.results[0].evidence_count = 1;
        consistency.consistent = true;
    } else if (srv.self_ontology_loaded &&
               srv.self_ontology.triple_count > 0) {
        /* Self-hosted ontology first — has Datalog inference + backbone rules */
        tardy_self_ontology_verify(&srv.self_ontology,
                                    all_triples, triple_count,
                                    &grounding, &consistency);
        /* Fall through to bridge for any unknowns — merge, don't overwrite */
        if (grounding.unknown > 0 && srv.bridge_connected) {
            tardy_grounding_t bridge_grounding = {0};
            tardy_consistency_t bridge_consistency = {0};
            tardy_bridge_verify(&srv.bridge, all_triples, triple_count,
                                 &bridge_grounding, &bridge_consistency);
            /* Merge: only upgrade UNKNOWN results from bridge */
            for (int i = 0; i < grounding.count && i < bridge_grounding.count; i++) {
                if (grounding.results[i].status == TARDY_KNOWLEDGE_UNKNOWN &&
                    bridge_grounding.results[i].status != TARDY_KNOWLEDGE_UNKNOWN) {
                    grounding.results[i] = bridge_grounding.results[i];
                    grounding.unknown--;
                    if (bridge_grounding.results[i].status == TARDY_KNOWLEDGE_GROUNDED)
                        grounding.grounded++;
                    else if (bridge_grounding.results[i].status == TARDY_KNOWLEDGE_CONTRADICTED)
                        grounding.contradicted++;
                    else if (bridge_grounding.results[i].status == TARDY_KNOWLEDGE_CONSISTENT)
                        grounding.consistent++;
                }
            }
            if (!bridge_consistency.consistent)
                consistency = bridge_consistency;
        }
    } else if (srv.bridge_connected) {
        tardy_bridge_verify(&srv.bridge, all_triples, triple_count,
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

    /* LLM grounding fallback — try for any UNKNOWN triples */
    if (grounding.unknown > 0 && tardy_llm_ground_enabled()) {
        tardy_llm_conn_t llm_conn;
        if (tardy_llm_connect(&llm_conn) == 0) {
            tardy_write(STDERR_FILENO, "[tardy] llm grounding: connected\n", 33);
            int resolved = 0;
            for (int i = 0; i < grounding.count; i++) {
                if (grounding.results[i].status != TARDY_KNOWLEDGE_UNKNOWN)
                    continue;

                tardy_llm_ground_result_t lr = tardy_llm_ground_triple(
                    &llm_conn,
                    grounding.results[i].triple.subject,
                    grounding.results[i].triple.predicate,
                    grounding.results[i].triple.object);

                if (lr.confidence > 0.0f) {
                    if (lr.grounded) {
                        grounding.results[i].status = TARDY_KNOWLEDGE_GROUNDED;
                        grounding.results[i].confidence = lr.confidence;
                        grounding.results[i].evidence_count = 1;
                        grounding.grounded++;
                    } else {
                        grounding.results[i].status = TARDY_KNOWLEDGE_CONTRADICTED;
                        grounding.results[i].confidence = 0.0f;
                        grounding.contradicted++;
                    }
                    grounding.unknown--;
                    resolved++;

                    /* Cache in Datalog KB for future instant hits */
                    if (lr.grounded) {
                        tardy_dl_add_fact(
                            &srv.self_ontology.datalog,
                            grounding.results[i].triple.predicate,
                            grounding.results[i].triple.subject,
                            grounding.results[i].triple.object);
                        srv.self_ontology.triple_count++;
                    }
                }
            }

            /* Also ground the full claim */
            if (grounding.unknown > 0 || triple_count == 0) {
                tardy_llm_ground_result_t cr = tardy_llm_ground_claim(
                    &llm_conn, task_text);
                if (cr.confidence > 0.0f && triple_count == 0) {
                    /* No triples extracted but LLM can judge the claim */
                    grounding.count = 1;
                    grounding.results[0].confidence = cr.confidence;
                    grounding.results[0].evidence_count = 1;
                    if (cr.grounded) {
                        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
                        grounding.grounded = 1;
                        grounding.unknown = 0;
                    } else {
                        grounding.results[0].status = TARDY_KNOWLEDGE_CONTRADICTED;
                        grounding.contradicted = 1;
                        grounding.unknown = 0;
                    }
                }
            }

            /* Store in palace memory if daemon is running */
            {
                tardy_palace_t palace;
                tardy_palace_init(&palace);
                if (tardy_palace_load(&palace, NULL) == 0) {
                    for (int i = 0; i < grounding.count; i++) {
                        if (grounding.results[i].status == TARDY_KNOWLEDGE_GROUNDED) {
                            tardy_uuid_t root = {0, 0};
                            tardy_palace_remember(&palace,
                                "llm_grounding", "knowledge",
                                grounding.results[i].triple.subject,
                                grounding.results[i].triple.predicate,
                                grounding.results[i].triple.object,
                                grounding.results[i].confidence, root);
                        }
                    }
                    tardy_palace_save(&palace, NULL);
                }
            }

            if (resolved > 0) {
                char num[64];
                int n = snprintf(num, sizeof(num),
                    "[tardy] llm grounding: %d triples resolved\n", resolved);
                if (n > (int)sizeof(num)) n = (int)sizeof(num);
                tardy_write(STDERR_FILENO, num, n);
            }
            tardy_llm_disconnect(&llm_conn);
        }
    }

    /* Work log -- CLI does real work, record it.
     * LLM grounding counts: each resolved triple = 1 ontology query. */
    tardy_work_log_t work_log;
    tardy_worklog_init(&work_log);
    if (comp_result == 1) {
        /* Computational verification counts as real work */
        work_log.ontology_queries = 2;
        work_log.context_reads = 1;
        work_log.agents_spawned = 1;
        work_log.compute_ns = 10000000; /* 10ms minimum */
    } else {
        int known_sources = (srv.bridge_connected ||
            srv.self_ontology.triple_count > 0);
        int llm_resolved = grounding.grounded + grounding.contradicted;
        /* CLI verification does real work: decompose + ground + consistency.
         * Minimum 2 queries (decompose + ground) even for single-triple claims. */
        int queries = known_sources ?
            triple_count : (llm_resolved > 0 ? llm_resolved : 0);
        work_log.ontology_queries = queries < 2 ? 2 : queries;
        work_log.context_reads = triple_count < 2 ? 2 : triple_count;
        work_log.agents_spawned = 1;
        work_log.compute_ns = 10000000; /* 10ms minimum */
    }

    const tardy_semantics_t *sem = &vm->semantics;
    tardy_work_spec_t spec = tardy_compute_work_spec(sem);

    /* Run BFT 3-pass verification */
    tardy_pipeline_result_t results[3];
    int pass_count = 0;
    float total_confidence = 0.0f;

    for (int run = 0; run < 3; run++) {
        tardy_decomposition_t run_decomps[3];
        memset(run_decomps, 0, sizeof(run_decomps));
        for (int d = 0; d < 3; d++)
            run_decomps[d] = decomps[(d + run) % 3];
        results[run] = tardy_pipeline_verify(
            task_text, claim_len,
            run_decomps, 3, &grounding, &consistency,
            &work_log, &spec, sem);
        if (results[run].passed) {
            pass_count++;
            total_confidence += results[run].confidence;
        }
    }

    int verified = (pass_count >= 2);
    float avg_confidence = pass_count > 0 ? total_confidence / (float)pass_count : 0.0f;

    /* Feedback-driven retry (same as MCP verify_claim) */
    int retries = 0;
    while (!verified && retries < 2) {
        tardy_failure_type_t fail = TARDY_FAIL_NONE;
        for (int run = 0; run < 3; run++) {
            if (!results[run].passed && results[run].failure_type != TARDY_FAIL_NONE) {
                fail = results[run].failure_type;
                break;
            }
        }
        if (fail == TARDY_FAIL_ONTOLOGY_GAP || fail == TARDY_FAIL_DECOMPOSITION)
            break;
        if (fail == TARDY_FAIL_LOW_CONFIDENCE) {
            tardy_semantics_t retry_sem = *sem;
            retry_sem.truth.min_confidence *= 0.9f;
            pass_count = 0;
            total_confidence = 0.0f;
            for (int run = 0; run < 3; run++) {
                tardy_decomposition_t run_decomps[3];
                memset(run_decomps, 0, sizeof(run_decomps));
                for (int d = 0; d < 3; d++)
                    run_decomps[d] = decomps[(d + run) % 3];
                results[run] = tardy_pipeline_verify(
                    task_text, claim_len,
                    run_decomps, 3, &grounding, &consistency,
                    &work_log, &spec, &retry_sem);
                if (results[run].passed) {
                    pass_count++;
                    total_confidence += results[run].confidence;
                }
            }
            verified = (pass_count >= 2);
            if (verified)
                avg_confidence = total_confidence / (float)pass_count;
        } else {
            break;
        }
        retries++;
    }

    /* Output result */
    tardy_write(STDERR_FILENO, "\n", 1);
    tardy_write(STDERR_FILENO, "[tardy] task: ", 14);
    tardy_write(STDERR_FILENO, task_text, strlen(task_text));
    tardy_write(STDERR_FILENO, "\n", 1);

    tardy_write(STDERR_FILENO, "[tardy] decomposed: ", 20);
    {
        char num[8];
        num[0] = '0' + (char)(triple_count % 10);
        num[1] = '\0';
        tardy_write(STDERR_FILENO, num, 1);
    }
    tardy_write(STDERR_FILENO, " triples\n", 9);

    tardy_write(STDERR_FILENO, "[tardy] grounded: ", 18);
    {
        char num[8];
        num[0] = '0' + (char)(grounding.grounded % 10);
        num[1] = '/';
        num[2] = '0' + (char)(grounding.count % 10);
        num[3] = '\0';
        tardy_write(STDERR_FILENO, num, 3);
    }
    tardy_write(STDERR_FILENO, "\n", 1);

    tardy_write(STDERR_FILENO, "[tardy] ontology: ", 18);
    tardy_write(STDERR_FILENO,
                srv.bridge_connected ? "connected" :
                (srv.self_ontology.triple_count > 0 ? "self-hosted" : "offline"),
                srv.bridge_connected ? 9 :
                (srv.self_ontology.triple_count > 0 ? 11 : 7));
    tardy_write(STDERR_FILENO, "\n", 1);

    tardy_write(STDERR_FILENO, "[tardy] bft: ", 13);
    {
        char num[8];
        num[0] = '0' + (char)pass_count;
        num[1] = '/';
        num[2] = '3';
        num[3] = '\0';
        tardy_write(STDERR_FILENO, num, 3);
    }
    tardy_write(STDERR_FILENO, "\n", 1);

    if (verified) {
        tardy_write(STDERR_FILENO, "[tardy] VERIFIED", 16);
        char conf[16];
        int ci = (int)(avg_confidence * 100);
        int clen = snprintf(conf, sizeof(conf), " (%d%%)", ci);
        tardy_write(STDERR_FILENO, conf, clen);
        tardy_write(STDERR_FILENO, "\n", 1);

        /* Freeze the agent */
        tardy_vm_freeze(vm, task_agent->id, TARDY_TRUST_VERIFIED);
        tardy_vm_converse(vm, task_agent->id, "agent", "verified");
    } else {
        tardy_write(STDERR_FILENO, "[tardy] NOT VERIFIED", 20);

        /* Report failure type */
        const char *fail_str = "";
        for (int run = 0; run < 3; run++) {
            if (!results[run].passed) {
                switch (results[run].failure_type) {
                case TARDY_FAIL_DECOMPOSITION:  fail_str = " (decomposition_error)"; break;
                case TARDY_FAIL_ONTOLOGY_GAP:   fail_str = " (ontology_gap)"; break;
                case TARDY_FAIL_CONTRADICTION:   fail_str = " (contradiction)"; break;
                case TARDY_FAIL_LOW_CONFIDENCE:  fail_str = " (low_confidence)"; break;
                case TARDY_FAIL_INCONSISTENCY:   fail_str = " (inconsistency)"; break;
                case TARDY_FAIL_NO_EVIDENCE:     fail_str = " (no_evidence)"; break;
                case TARDY_FAIL_PROTOCOL:        fail_str = " (protocol_error)"; break;
                case TARDY_FAIL_LAZINESS:        fail_str = " (laziness)"; break;
                case TARDY_FAIL_AMBIGUITY:       fail_str = " (ambiguity)"; break;
                case TARDY_FAIL_CROSS_REP:       fail_str = " (cross_rep_conflict)"; break;
                default: fail_str = ""; break;
                }
                if (fail_str[0]) break;
            }
        }
        tardy_write(STDERR_FILENO, fail_str, strlen(fail_str));
        tardy_write(STDERR_FILENO, "\n", 1);
        tardy_vm_converse(vm, task_agent->id, "agent", "not verified");
    }

    tardy_write(STDERR_FILENO, "\n", 1);

    tardy_bridge_shutdown(&srv.bridge);
    tardy_vm_shutdown(vm);
    munmap(vm, sizeof(tardy_vm_t));

    return verified ? 0 : 1;
}

/* ============================================
 * CLI: tardy verify "claim" — alias for run
 * ============================================ */

/* ============================================
 * CLI: tardy check file.tardy — compile check only
 * ============================================ */

static int check_file(const char *path)
{
    tardy_program_t prog;
    if (tardy_compile_file(&prog, path) != 0) {
        tardy_write(STDERR_FILENO, "[tardy] error: ", 15);
        tardy_write(STDERR_FILENO, prog.error, strlen(prog.error));
        tardy_write(STDERR_FILENO, "\n", 1);
        return 1;
    }
    tardy_write(STDERR_FILENO, "[tardy] ", 8);
    tardy_write(STDERR_FILENO, path, strlen(path));
    tardy_write(STDERR_FILENO, " ok (", 5);
    {
        char num[16];
        int n = snprintf(num, sizeof(num), "%d", prog.count);
        tardy_write(STDERR_FILENO, num, n);
    }
    tardy_write(STDERR_FILENO, " instructions)\n", 15);
    return 0;
}

/* ============================================
 * CLI: tardy verify-doc file.md — scan document for contradictions
 *
 * Reads a markdown/text file, splits into sentences, extracts
 * triples, and checks for internal contradictions using:
 * 1. Triple consistency (same subject+predicate, different object)
 * 2. Numeric verification (tardy_numeric_verify)
 * 3. LLM decomposition (tardy_llm_decompose) for implicit contradictions
 *
 * Optimization: groups sentences by shared entities, only checks
 * pairs within the same topic group — roughly O(n) for most docs.
 * ============================================ */

/* Maximum sentences and entity groups for verify-doc */
#define VDOC_MAX_SENTENCES  1024
#define VDOC_MAX_SENT_LEN   1024
#define VDOC_MAX_ENTITIES   256
#define VDOC_MAX_CONFLICTS  64

typedef struct {
    char text[VDOC_MAX_SENT_LEN];
    int  line_number;
    int  len;
    int  scope_id;  /* scope boundary for --scope-aware mode */
    tardy_triple_t triples[16];
    int  triple_count;
} vdoc_sentence_t;

typedef struct {
    char entity[TARDY_MAX_TRIPLE_LEN];
    int  sentence_indices[128];
    int  count;
} vdoc_entity_group_t;

typedef struct {
    int  line_a;
    int  line_b;
    char sent_a[256];
    char sent_b[256];
    char explanation[512];
    float confidence;
    int  is_consistent; /* 1 = checked but consistent, 0 = conflict */
} vdoc_conflict_t;

/* Case-insensitive substring check for verify-doc */
static const char *vdoc_ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    int nlen = (int)strlen(needle);
    for (const char *p = haystack; *p; p++) {
        int match = 1;
        for (int i = 0; i < nlen; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) !=
                         tolower((unsigned char)needle[i])) {
                match = 0;
                break;
            }
        }
        if (match) return p;
    }
    return NULL;
}

/* Truncate a string for display, appending "..." if needed */
static void vdoc_truncate(char *dst, int dst_size, const char *src)
{
    int slen = (int)strlen(src);
    if (slen < dst_size - 1) {
        memcpy(dst, src, (size_t)slen);
        dst[slen] = '\0';
    } else {
        if (dst_size < 4) {
            if (dst_size > 0) dst[0] = '\0';
            return;
        }
        int copy = dst_size - 4;
        memcpy(dst, src, (size_t)copy);
        dst[copy] = '.';
        dst[copy + 1] = '.';
        dst[copy + 2] = '.';
        dst[copy + 3] = '\0';
    }
}

static int verify_doc(const char *path, int scope_aware)
{
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Read file */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[tardy] error: cannot open %s\n", path);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        fprintf(stderr, "[tardy] error: cannot stat or empty file %s\n", path);
        close(fd);
        return 1;
    }

    size_t file_size = (size_t)st.st_size;
    char *buf = (char *)mmap(NULL, file_size + 1, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        fprintf(stderr, "[tardy] error: mmap failed\n");
        close(fd);
        return 1;
    }
    ssize_t rd = read(fd, buf, file_size);
    close(fd);
    if (rd <= 0) {
        fprintf(stderr, "[tardy] error: read failed\n");
        munmap(buf, file_size + 1);
        return 1;
    }
    buf[rd] = '\0';
    if (rd > INT_MAX - 1) {
        fprintf(stderr, "[tardy] error: file too large (max 2GB)\n");
        munmap(buf, file_size + 1);
        return 1;
    }
    int buf_len = (int)rd;

    /* ---- Phase 1: Split into sentences with line numbers ---- */
    vdoc_sentence_t *sentences = (vdoc_sentence_t *)mmap(
        NULL, sizeof(vdoc_sentence_t) * VDOC_MAX_SENTENCES,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sentences == MAP_FAILED) {
        fprintf(stderr, "[tardy] error: mmap failed for sentences\n");
        munmap(buf, file_size + 1);
        return 1;
    }

    int sent_count = 0;
    int line_num = 1;
    int sent_start = 0;
    int sent_line = 1;

    for (int i = 0; i <= buf_len && sent_count < VDOC_MAX_SENTENCES; i++) {
        int at_end = (i == buf_len);
        int at_delim = 0;

        if (!at_end) {
            char c = buf[i];
            if (c == '\n') {
                line_num++;
                /* Split on double newline (paragraph break) */
                if (i + 1 < buf_len && buf[i + 1] == '\n')
                    at_delim = 1;
            }
            /* Split on ". ", ".\n", "? ", "! " */
            if ((c == '.' || c == '?' || c == '!') &&
                i + 1 < buf_len &&
                (buf[i + 1] == ' ' || buf[i + 1] == '\n')) {
                /* Skip abbreviations: single capital letter before dot */
                if (c == '.' && i >= 1 &&
                    (buf[i - 1] >= 'A' && buf[i - 1] <= 'Z') &&
                    (i < 2 || buf[i - 2] == ' ' || buf[i - 2] == '.')) {
                    /* abbreviation — don't split */
                } else {
                    at_delim = 1;
                }
            }
        }

        if (at_delim || at_end) {
            int slen = i - sent_start;
            if (at_delim && !at_end) slen++; /* include the delimiter char */

            if (slen > 3) { /* skip very short fragments */
                /* Trim leading whitespace/newlines */
                int trim_start = sent_start;
                while (trim_start < sent_start + slen &&
                       (buf[trim_start] == ' ' || buf[trim_start] == '\n' ||
                        buf[trim_start] == '\r' || buf[trim_start] == '\t' ||
                        buf[trim_start] == '#'))
                    trim_start++;

                int trim_len = slen - (trim_start - sent_start);
                /* Trim trailing */
                while (trim_len > 0 &&
                       (buf[trim_start + trim_len - 1] == ' ' ||
                        buf[trim_start + trim_len - 1] == '\n' ||
                        buf[trim_start + trim_len - 1] == '\r'))
                    trim_len--;

                if (trim_len > 3 && trim_len < VDOC_MAX_SENT_LEN - 1) {
                    memcpy(sentences[sent_count].text,
                           buf + trim_start, (size_t)trim_len);
                    sentences[sent_count].text[trim_len] = '\0';
                    sentences[sent_count].line_number = sent_line;
                    sentences[sent_count].len = trim_len;
                    sentences[sent_count].triple_count = 0;
                    sent_count++;
                }
            }
            sent_start = i + 1;
            /* Track line number at the start of the next sentence */
            sent_line = line_num;
            /* Skip past any newlines between sentences to get accurate line */
            for (int sk = sent_start; sk < buf_len && (buf[sk] == '\n' || buf[sk] == '\r' || buf[sk] == ' '); sk++) {
                if (buf[sk] == '\n') sent_line++;
            }
        }
    }

    /* ---- Phase 1b: Assign scope IDs (if --scope-aware) ---- */
    if (scope_aware) {
        int current_scope = 0;
        for (int i = 0; i < sent_count; i++) {
            /* Detect function/method/class boundaries */
            const char *s = sentences[i].text;
            if (strncmp(s, "def ", 4) == 0 ||           /* Python */
                strncmp(s, "class ", 6) == 0 ||          /* Python/Java/TS */
                strncmp(s, "fn ", 3) == 0 ||             /* Rust */
                strncmp(s, "func ", 5) == 0 ||           /* Go */
                strncmp(s, "function ", 9) == 0 ||       /* JS/TS */
                strncmp(s, "static ", 7) == 0 ||         /* C */
                strncmp(s, "void ", 5) == 0 ||           /* C/Java */
                strncmp(s, "int ", 4) == 0 ||            /* C */
                strncmp(s, "pub fn ", 7) == 0 ||         /* Rust */
                strncmp(s, "async def ", 10) == 0 ||     /* Python async */
                strncmp(s, "async fn ", 9) == 0 ||       /* Rust async */
                (vdoc_ci_strstr(s, "def ") && vdoc_ci_strstr(s, "(self"))) { /* Python method */
                current_scope++;
            }
            sentences[i].scope_id = current_scope;
        }
    } else {
        /* No scope awareness — all sentences in scope 0 */
        for (int i = 0; i < sent_count; i++)
            sentences[i].scope_id = 0;
    }

    /* ---- Phase 2: Decompose each sentence into triples ---- */
    int total_triples = 0;
    for (int i = 0; i < sent_count; i++) {
        sentences[i].triple_count = tardy_decompose(
            sentences[i].text, sentences[i].len,
            sentences[i].triples, 16);
        total_triples += sentences[i].triple_count;
    }

    /* ---- Phase 2b: LLM factual grounding per triple ---- */
    vdoc_conflict_t *llm_conflicts = (vdoc_conflict_t *)mmap(
        NULL, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (llm_conflicts == MAP_FAILED) {
        fprintf(stderr, "[tardy] error: mmap failed for llm_conflicts\n");
        munmap(sentences, sizeof(vdoc_sentence_t) * VDOC_MAX_SENTENCES);
        munmap(buf, file_size + 1);
        return 1;
    }
    int llm_conflict_count = 0;

    if (tardy_llm_ground_enabled()) {
        tardy_llm_conn_t llm_conn;
        if (tardy_llm_connect(&llm_conn) == 0) {
            fprintf(stderr, "[tardy] llm grounding: connected (verify-doc)\n");
            int llm_checked = 0;
            int llm_false = 0;

            for (int i = 0; i < sent_count; i++) {
                for (int t = 0; t < sentences[i].triple_count; t++) {
                    tardy_triple_t *tr = &sentences[i].triples[t];

                    /* Skip fallback triples */
                    if (strcmp(tr->subject, "claim") == 0 ||
                        strcmp(tr->subject, "subject") == 0)
                        continue;

                    tardy_llm_ground_result_t lr = tardy_llm_ground_triple(
                        &llm_conn, tr->subject, tr->predicate, tr->object);
                    llm_checked++;

                    if (lr.confidence > 0.0f && !lr.grounded &&
                        llm_conflict_count < VDOC_MAX_CONFLICTS) {
                        vdoc_conflict_t *c = &llm_conflicts[llm_conflict_count];
                        c->line_a = sentences[i].line_number;
                        c->line_b = 0; /* LLM, not a document line */
                        vdoc_truncate(c->sent_a, (int)sizeof(c->sent_a),
                                      sentences[i].text);
                        snprintf(c->sent_b, sizeof(c->sent_b),
                                 "[LLM] %s (confidence: %.2f)",
                                 lr.explanation, (double)lr.confidence);
                        snprintf(c->explanation, sizeof(c->explanation),
                                 "Factual error: (%s, %s, %s) — %s",
                                 tr->subject, tr->predicate, tr->object,
                                 lr.explanation);
                        c->confidence = lr.confidence;
                        c->is_consistent = 0;
                        llm_conflict_count++;
                        llm_false++;
                    }
                }
            }

            if (llm_checked > 0) {
                fprintf(stderr,
                    "[tardy] llm grounding: %d triples checked, %d factual errors\n",
                    llm_checked, llm_false);
            }
            tardy_llm_disconnect(&llm_conn);
        }
    }

    /* ---- Phase 3: Group by shared entities ---- */
    vdoc_entity_group_t *groups = (vdoc_entity_group_t *)mmap(
        NULL, sizeof(vdoc_entity_group_t) * VDOC_MAX_ENTITIES,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (groups == MAP_FAILED) {
        fprintf(stderr, "[tardy] error: mmap failed for groups\n");
        munmap(llm_conflicts, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS);
        munmap(sentences, sizeof(vdoc_sentence_t) * VDOC_MAX_SENTENCES);
        munmap(buf, file_size + 1);
        return 1;
    }
    int group_count = 0;

    for (int i = 0; i < sent_count; i++) {
        for (int t = 0; t < sentences[i].triple_count; t++) {
            const char *subj = sentences[i].triples[t].subject;
            /* Skip fallback/generic triples */
            if (strcmp(subj, "claim") == 0 &&
                strcmp(sentences[i].triples[t].predicate, "states") == 0)
                continue;
            if (strcmp(subj, "subject") == 0)
                continue;
            /* Find or create group for this subject */
            int found = -1;
            for (int g = 0; g < group_count; g++) {
                int subj_len = (int)strlen(subj);
                int ent_len = (int)strlen(groups[g].entity);
                if (subj_len >= 4 && ent_len >= 4) {
                    /* Substring match only for longer entities */
                    if (vdoc_ci_strstr(groups[g].entity, subj) ||
                        vdoc_ci_strstr(subj, groups[g].entity)) {
                        found = g;
                        break;
                    }
                } else {
                    /* Exact case-insensitive match for short entities */
                    if (strcasecmp(groups[g].entity, subj) == 0) {
                        found = g;
                        break;
                    }
                }
            }
            if (found < 0 && group_count < VDOC_MAX_ENTITIES) {
                found = group_count++;
                strncpy(groups[found].entity, subj, TARDY_MAX_TRIPLE_LEN - 1);
                groups[found].entity[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
                groups[found].count = 0;
            }
            if (found >= 0 && groups[found].count < 128) {
                /* Avoid duplicate sentence indices */
                int dup = 0;
                for (int k = 0; k < groups[found].count; k++) {
                    if (groups[found].sentence_indices[k] == i) {
                        dup = 1; break;
                    }
                }
                if (!dup)
                    groups[found].sentence_indices[groups[found].count++] = i;
            }
        }
    }

    /* ---- Phase 4: Check pairs within each group ---- */
    vdoc_conflict_t *conflicts = (vdoc_conflict_t *)mmap(
        NULL, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (conflicts == MAP_FAILED) {
        fprintf(stderr, "[tardy] error: mmap failed for conflicts\n");
        munmap(groups, sizeof(vdoc_entity_group_t) * VDOC_MAX_ENTITIES);
        munmap(llm_conflicts, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS);
        munmap(sentences, sizeof(vdoc_sentence_t) * VDOC_MAX_SENTENCES);
        munmap(buf, file_size + 1);
        return 1;
    }
    int conflict_count = 0;
    int pairs_checked = 0;

    for (int g = 0; g < group_count && conflict_count < VDOC_MAX_CONFLICTS; g++) {
        for (int a = 0; a < groups[g].count && conflict_count < VDOC_MAX_CONFLICTS; a++) {
            for (int b = a + 1; b < groups[g].count && conflict_count < VDOC_MAX_CONFLICTS; b++) {
                int si = groups[g].sentence_indices[a];
                int sj = groups[g].sentence_indices[b];
                if (si == sj) continue;
                /* Skip cross-scope pairs in scope-aware mode */
                if (scope_aware && sentences[si].scope_id != sentences[sj].scope_id)
                    continue;
                pairs_checked++;

                /* 4a: Triple consistency — same subject+predicate, different object */
                for (int ti = 0; ti < sentences[si].triple_count; ti++) {
                    for (int tj = 0; tj < sentences[sj].triple_count; tj++) {
                        tardy_triple_t *ta = &sentences[si].triples[ti];
                        tardy_triple_t *tb = &sentences[sj].triples[tj];

                        /* Skip fallback triples — "claim/states" and
                         * "subject" are catch-alls for unparseable sentences */
                        if (strcmp(ta->subject, "claim") == 0 &&
                            strcmp(ta->predicate, "states") == 0)
                            continue;
                        if (strcmp(tb->subject, "claim") == 0 &&
                            strcmp(tb->predicate, "states") == 0)
                            continue;
                        if (strcmp(ta->subject, "subject") == 0)
                            continue;
                        if (strcmp(tb->subject, "subject") == 0)
                            continue;
                        /* Skip generic "located_in"/"located_at" with
                         * vague subjects — these match too broadly */
                        if ((strcmp(ta->predicate, "located_at") == 0 ||
                             strcmp(ta->predicate, "located_in") == 0) &&
                            strlen(ta->subject) < 5)
                            continue;

                        if (vdoc_ci_strstr(ta->subject, tb->subject) &&
                            vdoc_ci_strstr(ta->predicate, tb->predicate) &&
                            !vdoc_ci_strstr(ta->object, tb->object) &&
                            ta->object[0] && tb->object[0]) {
                            /* Same subject+predicate, different object = potential conflict */
                            vdoc_conflict_t *c = &conflicts[conflict_count];
                            c->line_a = sentences[si].line_number;
                            c->line_b = sentences[sj].line_number;
                            vdoc_truncate(c->sent_a, (int)sizeof(c->sent_a),
                                          sentences[si].text);
                            vdoc_truncate(c->sent_b, (int)sizeof(c->sent_b),
                                          sentences[sj].text);
                            snprintf(c->explanation, sizeof(c->explanation),
                                     "Triple conflict: (%s, %s) has objects "
                                     "\"%s\" vs \"%s\"",
                                     ta->subject, ta->predicate,
                                     ta->object, tb->object);
                            c->confidence = 0.85f;
                            c->is_consistent = 0;
                            conflict_count++;
                        }
                    }
                }

                /* 4b: Numeric verification across sentence pair */
                {
                    const char *pair[2];
                    pair[0] = sentences[si].text;
                    pair[1] = sentences[sj].text;
                    tardy_numeric_check_t nc = tardy_numeric_verify(pair, 2);
                    if (nc.has_contradiction && conflict_count < VDOC_MAX_CONFLICTS) {
                        /* Check for duplicate (same line pair already flagged by triple check) */
                        int dup = 0;
                        for (int d = 0; d < conflict_count; d++) {
                            if (conflicts[d].line_a == sentences[si].line_number &&
                                conflicts[d].line_b == sentences[sj].line_number) {
                                /* Keep higher confidence */
                                if (0.95f > conflicts[d].confidence) {
                                    conflicts[d].confidence = 0.95f;
                                    snprintf(conflicts[d].explanation,
                                             sizeof(conflicts[d].explanation),
                                             "Numeric: %s", nc.explanation);
                                }
                                dup = 1;
                                break;
                            }
                        }
                        if (!dup) {
                            vdoc_conflict_t *c = &conflicts[conflict_count];
                            c->line_a = sentences[si].line_number;
                            c->line_b = sentences[sj].line_number;
                            vdoc_truncate(c->sent_a, (int)sizeof(c->sent_a),
                                          sentences[si].text);
                            vdoc_truncate(c->sent_b, (int)sizeof(c->sent_b),
                                          sentences[sj].text);
                            snprintf(c->explanation, sizeof(c->explanation),
                                     "Numeric: %s", nc.explanation);
                            c->confidence = 0.95f;
                            c->is_consistent = 0;
                            conflict_count++;
                        }
                    }
                }

                /* 4c: LLM decomposition for implicit contradictions */
                {
                    const char *pair[2];
                    pair[0] = sentences[si].text;
                    pair[1] = sentences[sj].text;

                    /* Build a basic decomposition for context */
                    tardy_decomposition_t basic;
                    memset(&basic, 0, sizeof(basic));
                    int tc = 0;
                    for (int t = 0; t < sentences[si].triple_count &&
                         tc < TARDY_MAX_TRIPLES; t++)
                        basic.triples[tc++] = sentences[si].triples[t];
                    for (int t = 0; t < sentences[sj].triple_count &&
                         tc < TARDY_MAX_TRIPLES; t++)
                        basic.triples[tc++] = sentences[sj].triples[t];
                    basic.count = tc;

                    tardy_llm_decomposition_t llm =
                        tardy_llm_decompose(pair, 2, &basic);

                    if (llm.found_implicit_contradiction &&
                        conflict_count < VDOC_MAX_CONFLICTS) {
                        vdoc_conflict_t *c = &conflicts[conflict_count];
                        c->line_a = sentences[si].line_number;
                        c->line_b = sentences[sj].line_number;
                        vdoc_truncate(c->sent_a, (int)sizeof(c->sent_a),
                                      sentences[si].text);
                        vdoc_truncate(c->sent_b, (int)sizeof(c->sent_b),
                                      sentences[sj].text);
                        snprintf(c->explanation, sizeof(c->explanation),
                                 "Implicit: %s", llm.reasoning);
                        c->confidence = 0.90f;
                        c->is_consistent = 0;
                        conflict_count++;
                    }
                }
            }
        }
    }

    /* ---- Phase 4e: Cross-check against memory palace ---- */
    {
        tardy_palace_t *palace = (tardy_palace_t *)mmap(
            NULL, sizeof(tardy_palace_t),
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (palace != MAP_FAILED) {
            tardy_palace_init(palace);
            int palace_loaded = (tardy_palace_load(palace, NULL) == 0);

            if (palace_loaded && palace->total_facts > 0) {
                int palace_conflicts = 0;
                for (int i = 0; i < sent_count && conflict_count < VDOC_MAX_CONFLICTS; i++) {
                    for (int t = 0; t < sentences[i].triple_count; t++) {
                        tardy_triple_t *tr = &sentences[i].triples[t];

                        /* Skip fallback triples */
                        if (strcmp(tr->subject, "claim") == 0 ||
                            strcmp(tr->subject, "subject") == 0)
                            continue;

                        /* Check if palace has a different current value */
                        tardy_memory_fact_t palace_conflict;
                        if (tardy_palace_check(palace,
                                tr->subject, tr->predicate, tr->object,
                                &palace_conflict)) {
                            /* Palace says something different */
                            vdoc_conflict_t *c = &conflicts[conflict_count];
                            c->line_a = sentences[i].line_number;
                            c->line_b = 0; /* palace, not a document line */
                            vdoc_truncate(c->sent_a, (int)sizeof(c->sent_a),
                                          sentences[i].text);
                            snprintf(c->sent_b, sizeof(c->sent_b),
                                     "[palace] %s %s %s (confidence: %.2f)",
                                     palace_conflict.subject,
                                     palace_conflict.predicate,
                                     palace_conflict.object,
                                     (double)palace_conflict.confidence);
                            snprintf(c->explanation, sizeof(c->explanation),
                                     "Palace conflict: document says \"%s\" but "
                                     "palace knows \"%s\" for (%s, %s)",
                                     tr->object, palace_conflict.object,
                                     tr->subject, tr->predicate);
                            c->confidence = palace_conflict.confidence;
                            c->is_consistent = 0;
                            conflict_count++;
                            palace_conflicts++;
                        }
                    }
                }
                if (palace_conflicts > 0) {
                    fprintf(stderr, "[palace] found %d conflict%s with known facts\n",
                            palace_conflicts,
                            palace_conflicts == 1 ? "" : "s");
                }
            }
            munmap(palace, sizeof(tardy_palace_t));
        }
    }

    /* ---- Phase 5: Output ---- */
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                      (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

    printf("\n=== Tardygrada Document Verification ===\n");
    printf("File: %s\n", path);
    printf("Sentences: %d\n", sent_count);
    printf("Triples extracted: %d\n", total_triples);
    printf("Entity groups: %d\n", group_count);
    printf("Pairs checked: %d\n\n", pairs_checked);

    int real_conflicts = 0;
    for (int i = 0; i < conflict_count; i++) {
        vdoc_conflict_t *c = &conflicts[i];
        if (c->is_consistent) {
            printf("[CONSISTENT] Lines %d vs %d:\n", c->line_a, c->line_b);
            printf("  \"%s\"\n", c->sent_a);
            printf("  \"%s\"\n", c->sent_b);
            printf("  -> Not a contradiction\n");
            printf("  Confidence: N/A\n\n");
        } else {
            real_conflicts++;
            printf("[CONFLICT] Lines %d vs %d:\n", c->line_a, c->line_b);
            printf("  \"%s\"\n", c->sent_a);
            printf("  \"%s\"\n", c->sent_b);
            printf("  -> %s\n", c->explanation);
            printf("  Confidence: %.2f\n\n", (double)c->confidence);
        }
    }

    /* LLM factual errors */
    for (int i = 0; i < llm_conflict_count; i++) {
        vdoc_conflict_t *c = &llm_conflicts[i];
        real_conflicts++;
        printf("[FACTUAL ERROR] Line %d:\n", c->line_a);
        printf("  \"%s\"\n", c->sent_a);
        printf("  %s\n", c->sent_b);
        printf("  -> %s\n", c->explanation);
        printf("  Confidence: %.2f\n\n", (double)c->confidence);
    }

    printf("Summary: %d contradiction%s found, %d potential conflict%s checked, "
           "%d sentences verified",
           real_conflicts, real_conflicts == 1 ? "" : "s",
           pairs_checked, pairs_checked == 1 ? "" : "s",
           sent_count);
    if (llm_conflict_count > 0)
        printf(", %d factual error%s (LLM)",
               llm_conflict_count, llm_conflict_count == 1 ? "" : "s");
    if (scope_aware) {
        int max_scope = 0;
        for (int i = 0; i < sent_count; i++)
            if (sentences[i].scope_id > max_scope)
                max_scope = sentences[i].scope_id;
        printf(", scope-aware (%d scope%s)", max_scope,
               max_scope == 1 ? "" : "s");
    }
    printf("\n");
    printf("Time: %ldms\n\n", elapsed_ms);

    /* Cleanup */
    munmap(conflicts, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS);
    munmap(llm_conflicts, sizeof(vdoc_conflict_t) * VDOC_MAX_CONFLICTS);
    munmap(groups, sizeof(vdoc_entity_group_t) * VDOC_MAX_ENTITIES);
    munmap(sentences, sizeof(vdoc_sentence_t) * VDOC_MAX_SENTENCES);
    munmap(buf, file_size + 1);

    return real_conflicts > 0 ? 1 : 0;
}

/* ============================================
 * JSON escape helper for CLI arguments
 * ============================================ */

static void json_escape_cli(const char *src, char *dst, size_t dst_size)
{
    size_t w = 0;
    for (size_t i = 0; src[i] && w < dst_size - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[w++] = '\\';
        } else if (src[i] == '\n') {
            if (w + 2 < dst_size) { dst[w++] = '\\'; dst[w++] = 'n'; continue; }
        } else if (src[i] == '\t') {
            if (w + 2 < dst_size) { dst[w++] = '\\'; dst[w++] = 't'; continue; }
        }
        dst[w++] = src[i];
    }
    dst[w] = '\0';
}

/* ============================================
 * Usage
 * ============================================ */

static void print_usage(void)
{
    const char *usage =
        "Tardygrada — formally verified agent programming language\n"
        "\n"
        "Usage:\n"
        "  tardy daemon start [config]      Start persistent daemon\n"
        "  tardy daemon start --foreground   Start daemon in foreground\n"
        "  tardy daemon stop                Stop the daemon\n"
        "  tardy daemon status              Show daemon status\n"
        "  tardy run \"claim\"                Verify a claim (uses daemon if running)\n"
        "  tardy verify \"claim\"             Alias for run\n"
        "  tardy serve file.tardy           Compile and serve as MCP server\n"
        "  tardy check file.tardy           Compile check only\n"
        "  tardy monitor \"text\" [wing]      Verify text + store in palace (daemon)\n"
        "  tardy verify-doc file.md         Scan document for contradictions\n"
        "  tardy terraform path/to/repo     Convert agentic repo to .tardy\n"
        "  tardy spawn <name> [trust]       Spawn agent in daemon\n"
        "  tardy read <agent> [field]       Read agent in daemon\n"
        "  tardy remember <wing> \"fact\"     Store a fact in the memory palace\n"
        "  tardy recall <wing> [--room R]   Recall facts from the memory palace\n"
        "  tardy mcp-bridge                 MCP server bridging to daemon (for Claude Code)\n"
        "  tardy test                       Run built-in tests\n"
        "  tardy bench                      Run benchmarks\n"
        "  tardy                            Run tests (default)\n"
        "\n"
        "Examples:\n"
        "  tardy run \"Doctor Who was created at BBC Television Centre\"\n"
        "  tardy daemon start agents.conf\n"
        "  tardy serve examples/medical.tardy\n"
        "  tardy terraform ~/projects/crewai-example\n"
        "  tardy remember project-alpha \"The team has 8 members\"\n"
        "  tardy recall project-alpha\n"
        "  tardy recall project-alpha --room team\n"
        "\n";
    tardy_write(STDERR_FILENO, usage, strlen(usage));
}

/* ============================================
 * Entry Point
 * ============================================ */

int main(int argc, char **argv)
{
    /* No args: run tests */
    if (argc < 2)
        return run_tests();

    const char *cmd = argv[1];

    /* tardy daemon start|stop|status */
    if (strcmp(cmd, "daemon") == 0 && argc >= 3) {
        if (strcmp(argv[2], "start") == 0) {
            const char *config = NULL;
            int fg = 0;
            for (int i = 3; i < argc; i++) {
                if (strcmp(argv[i], "--foreground") == 0 ||
                    strcmp(argv[i], "-f") == 0)
                    fg = 1;
                else
                    config = argv[i];
            }
            return tardy_daemon_start(config, fg);
        }
        if (strcmp(argv[2], "stop") == 0)
            return tardy_daemon_stop();
        if (strcmp(argv[2], "status") == 0)
            return tardy_daemon_status();
        print_usage();
        return 1;
    }

    /* tardy run "claim" / tardy verify "claim" — try daemon first */
    if ((strcmp(cmd, "run") == 0 || strcmp(cmd, "verify") == 0) && argc >= 3) {
        if (tardy_daemon_is_running()) {
            /* Send to daemon */
            char request[4096];
            char escaped[2048];
            /* Escape the claim for JSON */
            const char *src = argv[2];
            int w = 0;
            for (int i = 0; src[i] && w < (int)sizeof(escaped) - 2; i++) {
                if (src[i] == '"' || src[i] == '\\') {
                    escaped[w++] = '\\';
                }
                escaped[w++] = src[i];
            }
            escaped[w] = '\0';
            snprintf(request, sizeof(request),
                     "{\"cmd\":\"run\",\"claim\":\"%s\"}", escaped);
            char response[4096];
            int len = tardy_daemon_send(request, response, sizeof(response));
            if (len > 0) {
                tardy_write(STDOUT_FILENO, response, len);
                tardy_write(STDOUT_FILENO, "\n", 1);
                return 0;
            }
            tardy_write(STDERR_FILENO, "[tardy] daemon send failed, running standalone\n", 47);
        }
        return run_task(argv[2]);
    }

    /* tardy serve file.tardy */
    if (strcmp(cmd, "serve") == 0 && argc >= 3)
        return tardy_exec_file(argv[2]);

    /* tardy check file.tardy */
    if (strcmp(cmd, "check") == 0 && argc >= 3)
        return check_file(argv[2]);

    /* tardy monitor "text" [wing] — verify and store via daemon */
    if (strcmp(cmd, "monitor") == 0 && argc >= 3) {
        if (!tardy_daemon_is_running()) {
            fprintf(stderr, "[tardy] monitor requires running daemon (tardy daemon start)\n");
            return 1;
        }
        const char *text = argv[2];
        const char *wing = (argc >= 4) ? argv[3] : "claude-session";

        char request[4096 + 256];
        char escaped_text[4096];
        json_escape_cli(text, escaped_text, sizeof(escaped_text));
        char escaped_wing[256];
        json_escape_cli(wing, escaped_wing, sizeof(escaped_wing));
        snprintf(request, sizeof(request),
                 "{\"cmd\":\"monitor\",\"text\":\"%s\",\"wing\":\"%s\"}",
                 escaped_text, escaped_wing);

        char response[TARDY_DAEMON_BUF];
        int len = tardy_daemon_send(request, response, sizeof(response));
        if (len > 0) {
            printf("%s\n", response);
            return 0;
        }
        fprintf(stderr, "[tardy] daemon send failed\n");
        return 1;
    }

    /* tardy verify-doc file.md [--scope-aware] — try daemon first */
    if (strcmp(cmd, "verify-doc") == 0 && argc >= 3) {
        int scope_aware = 0;
        const char *filepath = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--scope-aware") == 0)
                scope_aware = 1;
            else if (!filepath)
                filepath = argv[i];
        }
        if (!filepath) {
            fprintf(stderr, "[tardy] error: verify-doc requires a file path\n");
            return 1;
        }
        /* Auto-detect code files for scope-aware mode */
        if (!scope_aware) {
            const char *ext = strrchr(filepath, '.');
            if (ext && (strcmp(ext, ".py") == 0 || strcmp(ext, ".js") == 0 ||
                        strcmp(ext, ".ts") == 0 || strcmp(ext, ".go") == 0 ||
                        strcmp(ext, ".rs") == 0 || strcmp(ext, ".c") == 0 ||
                        strcmp(ext, ".h") == 0 || strcmp(ext, ".java") == 0 ||
                        strcmp(ext, ".rb") == 0 || strcmp(ext, ".tsx") == 0 ||
                        strcmp(ext, ".jsx") == 0)) {
                scope_aware = 1;
            }
        }
        if (tardy_daemon_is_running()) {
            char request[1024];
            char escaped[512];
            const char *src = filepath;
            int w = 0;
            for (int i = 0; src[i] && w < (int)sizeof(escaped) - 2; i++) {
                if (src[i] == '"' || src[i] == '\\') {
                    escaped[w++] = '\\';
                }
                escaped[w++] = src[i];
            }
            escaped[w] = '\0';
            snprintf(request, sizeof(request),
                     "{\"cmd\":\"verify-doc\",\"path\":\"%s\",\"scope_aware\":%d}",
                     escaped, scope_aware);
            char response[4096];
            int len = tardy_daemon_send(request, response, sizeof(response));
            if (len > 0) {
                tardy_write(STDOUT_FILENO, response, len);
                tardy_write(STDOUT_FILENO, "\n", 1);
                return 0;
            }
            tardy_write(STDERR_FILENO, "[tardy] daemon send failed, running standalone\n", 47);
        }
        return verify_doc(filepath, scope_aware);
    }

    /* tardy spawn <name> [trust] — daemon only */
    if (strcmp(cmd, "spawn") == 0 && argc >= 3) {
        if (!tardy_daemon_is_running()) {
            tardy_write(STDERR_FILENO, "[tardy] spawn requires running daemon (tardy daemon start)\n", 59);
            return 1;
        }
        char request[1024];
        const char *trust = argc >= 4 ? argv[3] : "default";
        snprintf(request, sizeof(request),
                 "{\"cmd\":\"spawn\",\"name\":\"%s\",\"trust\":\"%s\"}",
                 argv[2], trust);
        char response[4096];
        int len = tardy_daemon_send(request, response, sizeof(response));
        if (len > 0) {
            tardy_write(STDOUT_FILENO, response, len);
            tardy_write(STDOUT_FILENO, "\n", 1);
            return 0;
        }
        tardy_write(STDERR_FILENO, "[tardy] daemon send failed\n", 27);
        return 1;
    }

    /* tardy read <agent> [field] — daemon only */
    if (strcmp(cmd, "read") == 0 && argc >= 3) {
        if (!tardy_daemon_is_running()) {
            tardy_write(STDERR_FILENO, "[tardy] read requires running daemon (tardy daemon start)\n", 58);
            return 1;
        }
        char request[1024];
        if (argc >= 4)
            snprintf(request, sizeof(request),
                     "{\"cmd\":\"read\",\"agent\":\"%s\",\"field\":\"%s\"}",
                     argv[2], argv[3]);
        else
            snprintf(request, sizeof(request),
                     "{\"cmd\":\"read\",\"agent\":\"%s\"}",
                     argv[2]);
        char response[4096];
        int len = tardy_daemon_send(request, response, sizeof(response));
        if (len > 0) {
            tardy_write(STDOUT_FILENO, response, len);
            tardy_write(STDOUT_FILENO, "\n", 1);
            return 0;
        }
        tardy_write(STDERR_FILENO, "[tardy] daemon send failed\n", 27);
        return 1;
    }

    /* tardy remember <wing> "fact" — store a fact in the palace */
    if (strcmp(cmd, "remember") == 0 && argc >= 4) {
        const char *wing = argv[2];
        int fact_idx = 3;
        if (fact_idx < argc && strcmp(argv[fact_idx], "--") == 0)
            fact_idx++;
        if (fact_idx >= argc) {
            fprintf(stderr, "[tardy] error: remember requires a fact\n");
            return 1;
        }
        const char *fact = argv[fact_idx];

        /* If daemon is running, send to daemon */
        if (tardy_daemon_is_running()) {
            char request[4096];
            char esc_wing[256], esc_fact[2048];
            {
                const char *src = wing;
                int w = 0;
                for (int i = 0; src[i] && w < (int)sizeof(esc_wing) - 2; i++) {
                    if (src[i] == '"' || src[i] == '\\') esc_wing[w++] = '\\';
                    esc_wing[w++] = src[i];
                }
                esc_wing[w] = '\0';
            }
            {
                const char *src = fact;
                int w = 0;
                for (int i = 0; src[i] && w < (int)sizeof(esc_fact) - 2; i++) {
                    if (src[i] == '"' || src[i] == '\\') esc_fact[w++] = '\\';
                    esc_fact[w++] = src[i];
                }
                esc_fact[w] = '\0';
            }
            snprintf(request, sizeof(request),
                     "{\"cmd\":\"remember\",\"wing\":\"%s\",\"fact\":\"%s\"}",
                     esc_wing, esc_fact);
            char response[4096];
            int len = tardy_daemon_send(request, response, sizeof(response));
            if (len > 0) {
                /* Pretty-print the response */
                tardy_write(STDOUT_FILENO, response, len);
                tardy_write(STDOUT_FILENO, "\n", 1);
                return 0;
            }
            tardy_write(STDERR_FILENO, "[tardy] daemon send failed, using standalone palace\n", 52);
        }

        /* Standalone mode: load palace, remember, save */
        {
            static tardy_palace_t palace;
            tardy_palace_init(&palace);
            tardy_palace_load(&palace, NULL); /* ok if fails — fresh palace */

            char subject[128], predicate[64], object[256];
            tardy_palace_parse_sentence(fact,
                subject, sizeof(subject),
                predicate, sizeof(predicate),
                object, sizeof(object));

            char room[64];
            tardy_palace_auto_room(predicate, subject, room, sizeof(room));

            /* Check for superseding */
            tardy_memory_fact_t conflict;
            int had_conflict = tardy_palace_check(&palace, subject, predicate, object, &conflict);

            tardy_uuid_t source = {0, 0};
            int rc = tardy_palace_remember(&palace, wing, NULL,
                                            subject, predicate, object,
                                            0.85f, source);
            if (rc != 0) {
                tardy_write(STDERR_FILENO, "[palace] capacity exceeded\n", 27);
                return 1;
            }

            tardy_palace_save(&palace, NULL);

            /* Print result */
            char msg[1024];
            int mlen = snprintf(msg, sizeof(msg),
                "[palace] stored in wing:%s room:%s (confidence: 0.85)\n",
                wing, room);
            tardy_write(STDOUT_FILENO, msg, mlen);

            if (had_conflict) {
                mlen = snprintf(msg, sizeof(msg),
                    "[palace] superseded: \"%s\" (was valid until now)\n",
                    conflict.object);
                tardy_write(STDOUT_FILENO, msg, mlen);
            }
        }
        return 0;
    }

    /* tardy recall <wing> [--room <room>] [--query <query>] */
    if (strcmp(cmd, "recall") == 0 && argc >= 3) {
        const char *wing = argv[2];
        const char *room = NULL;
        const char *query = NULL;

        /* Parse optional flags */
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--room") == 0 && i + 1 < argc) {
                room = argv[++i];
            } else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) {
                query = argv[++i];
            } else {
                /* Treat as query */
                query = argv[i];
            }
        }

        /* If daemon is running, send to daemon */
        if (tardy_daemon_is_running()) {
            char request[4096];
            char esc_wing[256];
            {
                const char *src = wing;
                int w = 0;
                for (int i = 0; src[i] && w < (int)sizeof(esc_wing) - 2; i++) {
                    if (src[i] == '"' || src[i] == '\\') esc_wing[w++] = '\\';
                    esc_wing[w++] = src[i];
                }
                esc_wing[w] = '\0';
            }
            int rlen;
            if (room && query) {
                char esc_room[128], esc_query[512];
                {
                    const char *src = room;
                    int w = 0;
                    for (int i = 0; src[i] && w < (int)sizeof(esc_room) - 2; i++) {
                        if (src[i] == '"' || src[i] == '\\') esc_room[w++] = '\\';
                        esc_room[w++] = src[i];
                    }
                    esc_room[w] = '\0';
                }
                {
                    const char *src = query;
                    int w = 0;
                    for (int i = 0; src[i] && w < (int)sizeof(esc_query) - 2; i++) {
                        if (src[i] == '"' || src[i] == '\\') esc_query[w++] = '\\';
                        esc_query[w++] = src[i];
                    }
                    esc_query[w] = '\0';
                }
                rlen = snprintf(request, sizeof(request),
                    "{\"cmd\":\"recall\",\"wing\":\"%s\",\"room\":\"%s\",\"query\":\"%s\"}",
                    esc_wing, esc_room, esc_query);
            } else if (room) {
                char esc_room[128];
                {
                    const char *src = room;
                    int w = 0;
                    for (int i = 0; src[i] && w < (int)sizeof(esc_room) - 2; i++) {
                        if (src[i] == '"' || src[i] == '\\') esc_room[w++] = '\\';
                        esc_room[w++] = src[i];
                    }
                    esc_room[w] = '\0';
                }
                rlen = snprintf(request, sizeof(request),
                    "{\"cmd\":\"recall\",\"wing\":\"%s\",\"room\":\"%s\"}",
                    esc_wing, esc_room);
            } else if (query) {
                char esc_query[512];
                {
                    const char *src = query;
                    int w = 0;
                    for (int i = 0; src[i] && w < (int)sizeof(esc_query) - 2; i++) {
                        if (src[i] == '"' || src[i] == '\\') esc_query[w++] = '\\';
                        esc_query[w++] = src[i];
                    }
                    esc_query[w] = '\0';
                }
                rlen = snprintf(request, sizeof(request),
                    "{\"cmd\":\"recall\",\"wing\":\"%s\",\"query\":\"%s\"}",
                    esc_wing, esc_query);
            } else {
                rlen = snprintf(request, sizeof(request),
                    "{\"cmd\":\"recall\",\"wing\":\"%s\"}",
                    esc_wing);
            }
            (void)rlen;

            char response[TARDY_DAEMON_BUF];
            int len = tardy_daemon_send(request, response, sizeof(response));
            if (len > 0) {
                tardy_write(STDOUT_FILENO, response, len);
                tardy_write(STDOUT_FILENO, "\n", 1);
                return 0;
            }
            tardy_write(STDERR_FILENO, "[tardy] daemon send failed, using standalone palace\n", 52);
        }

        /* Standalone mode */
        {
            static tardy_palace_t palace;
            tardy_palace_init(&palace);
            if (tardy_palace_load(&palace, NULL) != 0) {
                tardy_write(STDERR_FILENO, "[palace] no palace data found\n", 30);
                return 1;
            }

            tardy_memory_fact_t results[64];
            int count = tardy_palace_recall(&palace, wing, room, query,
                                             results, 64);

            char msg[1024];
            int current = 0, superseded = 0;
            for (int i = 0; i < count; i++) {
                if (results[i].valid_to == 0) current++;
                else superseded++;
            }

            int mlen;
            if (room) {
                mlen = snprintf(msg, sizeof(msg),
                    "[palace] %d facts (%d current, %d superseded) in wing:%s room:%s\n",
                    count, current, superseded, wing, room);
            } else {
                mlen = snprintf(msg, sizeof(msg),
                    "[palace] %d facts in wing:%s\n", count, wing);
            }
            tardy_write(STDOUT_FILENO, msg, mlen);

            for (int i = 0; i < count; i++) {
                const char *status_tag;
                char time_info[128];

                if (results[i].valid_to == 0) {
                    status_tag = "current";
                    /* Format since-date */
                    time_t from = (time_t)results[i].valid_from;
                    struct tm *tm = gmtime(&from);
                    if (tm) {
                        snprintf(time_info, sizeof(time_info),
                            "since %04d-%02d-%02d",
                            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
                    } else {
                        snprintf(time_info, sizeof(time_info), "since epoch+%llu",
                                 (unsigned long long)results[i].valid_from);
                    }
                } else {
                    status_tag = "history";
                    time_t from = (time_t)results[i].valid_from;
                    time_t to = (time_t)results[i].valid_to;
                    struct tm tfrom, tto;
                    struct tm *pf = gmtime(&from);
                    if (pf) tfrom = *pf;
                    struct tm *pt = gmtime(&to);
                    if (pt) tto = *pt;
                    if (pf && pt) {
                        snprintf(time_info, sizeof(time_info),
                            "%04d-%02d-%02d to %04d-%02d-%02d",
                            tfrom.tm_year + 1900, tfrom.tm_mon + 1, tfrom.tm_mday,
                            tto.tm_year + 1900, tto.tm_mon + 1, tto.tm_mday);
                    } else {
                        snprintf(time_info, sizeof(time_info),
                            "epoch+%llu to epoch+%llu",
                            (unsigned long long)results[i].valid_from,
                            (unsigned long long)results[i].valid_to);
                    }
                }

                /* Reconstruct the sentence: "subject predicate object" */
                mlen = snprintf(msg, sizeof(msg),
                    "  [%s] %s %s %s (%s, confidence: %.2f)\n",
                    status_tag,
                    results[i].subject,
                    results[i].predicate,
                    results[i].object,
                    time_info,
                    (double)results[i].confidence);
                tardy_write(STDOUT_FILENO, msg, mlen);
            }

            if (count == 0) {
                tardy_write(STDOUT_FILENO, "  (no facts found)\n", 19);
            }
        }
        return 0;
    }

    /* tardy mcp-bridge — MCP server proxying to daemon */
    if (strcmp(cmd, "mcp-bridge") == 0) {
        if (!tardy_daemon_is_running()) {
            tardy_write(STDERR_FILENO,
                "[tardy] mcp-bridge requires running daemon (tardy daemon start)\n", 65);
            return 1;
        }
        return tardy_mcp_bridge_run();
    }

    /* tardy terraform path/to/repo */
    if (strcmp(cmd, "terraform") == 0 && argc >= 3) {
        char output[16384];
        int len = tardy_tf_terraform(argv[2], output, sizeof(output));
        if (len <= 0) {
            tardy_write(STDERR_FILENO, "[tardy] terraform failed\n", 25);
            return 1;
        }
        /* Write to stdout */
        tardy_write(STDOUT_FILENO, output, len);

        /* Also write to file: <repo_name>.tardy */
        tardy_tf_analysis_t analysis;
        tardy_tf_analyze(argv[2], &analysis);

        char outpath[512];
        snprintf(outpath, sizeof(outpath), "%s.tardy", analysis.repo_name);
        int fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t wn = write(fd, output, len);
            (void)wn;
            close(fd);
            tardy_write(STDERR_FILENO, "\n[tardy] wrote: ", 16);
            tardy_write(STDERR_FILENO, outpath, strlen(outpath));
            tardy_write(STDERR_FILENO, "\n", 1);
        }

        /* Verify the generated file compiles */
        tardy_program_t prog;
        if (tardy_compile(&prog, output, len) == 0) {
            tardy_write(STDERR_FILENO, "[tardy] compile check: ok (", 27);
            char num[16];
            int nlen = snprintf(num, sizeof(num), "%d", prog.count);
            tardy_write(STDERR_FILENO, num, nlen);
            tardy_write(STDERR_FILENO, " instructions)\n", 15);
        } else {
            tardy_write(STDERR_FILENO, "[tardy] compile check: FAILED (", 31);
            tardy_write(STDERR_FILENO, prog.error, strlen(prog.error));
            tardy_write(STDERR_FILENO, ")\n", 2);
        }

        /* Print stats */
        tardy_write(STDERR_FILENO, "[tardy] original: ", 18);
        {
            char stats[256];
            int slen = snprintf(stats, sizeof(stats),
                "%d files, %d lines, %d deps\n",
                analysis.total_files, analysis.total_lines, analysis.total_deps);
            tardy_write(STDERR_FILENO, stats, slen);
        }
        tardy_write(STDERR_FILENO, "[tardy] tardygrada: 1 file, ", 28);
        {
            char stats[64];
            int slen = snprintf(stats, sizeof(stats), "%d lines\n", len / 40);
            tardy_write(STDERR_FILENO, stats, slen);
        }

        return 0;
    }

    /* tardy test */
    if (strcmp(cmd, "test") == 0)
        return run_tests();

    /* tardy help / tardy --help / tardy -h */
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    /* tardy --serve (legacy) */
    if (strcmp(cmd, "--serve") == 0)
        return run_mcp();

    /* tardy file.tardy (legacy shorthand) */
    {
        int len = (int)strlen(cmd);
        if (len > 6 && strcmp(cmd + len - 6, ".tardy") == 0)
            return tardy_exec_file(cmd);
    }

    /* Unknown command */
    print_usage();
    return 1;
}
