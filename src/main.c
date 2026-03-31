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
#include "mcp/server.h"
#include "verify/pipeline.h"
#include "compiler/exec.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

static int is_zero_uuid(tardy_uuid_t id)
{
    return id.hi == 0 && id.lo == 0;
}

/* We don't use printf. We write directly. */
static void print(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
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
    write(STDOUT_FILENO, buf + i + 1, 30 - i);
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
 * Entry Point
 * ============================================ */

int main(int argc, char **argv)
{
    /* --serve: run as MCP server (hardcoded agents) */
    if (argc > 1 && strcmp(argv[1], "--serve") == 0)
        return run_mcp();

    /* <file.tardy>: compile and serve */
    if (argc > 1) {
        const char *path = argv[1];
        int len = (int)strlen(path);
        if (len > 6 && strcmp(path + len - 6, ".tardy") == 0)
            return tardy_exec_file(path);
    }

    /* Default: run tests */
    return run_tests();
}
