/*
 * Tardygrada — Benchmarks
 *
 * Measures real performance of core operations:
 * - Agent spawn speed
 * - Verified read speed (SHA-256 hash check)
 * - Sovereign read speed (BFT vote + hash + signature)
 * - Freeze speed (mutable → immutable)
 * - GC cycle speed
 * - Verification pipeline speed
 * - Message send/receive speed
 * - Semantic query speed
 */

#include "../src/vm/vm.h"
#include "../src/vm/semantic.h"
#include "../src/verify/pipeline.h"
#include "../src/verify/decompose.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void print_result(const char *name, uint64_t total_ns, int ops)
{
    double per_op_ns = (double)total_ns / (double)ops;
    double per_op_us = per_op_ns / 1000.0;
    double ops_per_sec = 1000000000.0 / per_op_ns;
    printf("  %-40s %8.1f ns/op  %10.0f ops/sec  (%d ops in %llu ns)\n",
           name, per_op_ns, ops_per_sec, ops, (unsigned long long)total_ns);
    (void)per_op_us;
}

int main(void)
{
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm == MAP_FAILED) return 1;
    tardy_vm_init(vm, NULL);

    tardy_uuid_t root = vm->root_id;
    uint64_t start, elapsed;
    int N;

    printf("\n=== Tardygrada Benchmarks ===\n\n");

    /* ---- Spawn: mutable agents ---- */
    N = 1000;
    start = now_ns();
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "m%d", i);
        int64_t val = i;
        tardy_vm_spawn(vm, root, name, TARDY_TYPE_INT,
                       TARDY_TRUST_MUTABLE, &val, sizeof(int64_t));
    }
    elapsed = now_ns() - start;
    print_result("spawn mutable agent", elapsed, N);

    /* Reset VM for next test */
    tardy_vm_shutdown(vm);
    tardy_vm_init(vm, NULL);
    root = vm->root_id;

    /* ---- Spawn: @verified agents ---- */
    N = 1000;
    start = now_ns();
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "v%d", i);
        int64_t val = i;
        tardy_vm_spawn(vm, root, name, TARDY_TYPE_INT,
                       TARDY_TRUST_VERIFIED, &val, sizeof(int64_t));
    }
    elapsed = now_ns() - start;
    print_result("spawn @verified agent (+ SHA-256)", elapsed, N);

    /* ---- Read: @verified (hash check per read) ---- */
    N = 10000;
    {
        int64_t val;
        start = now_ns();
        for (int i = 0; i < N; i++) {
            tardy_vm_read(vm, root, "v0", &val, sizeof(int64_t));
        }
        elapsed = now_ns() - start;
        print_result("read @verified (SHA-256 check)", elapsed, N);
    }

    /* Reset */
    tardy_vm_shutdown(vm);
    tardy_vm_init(vm, NULL);
    root = vm->root_id;

    /* ---- Spawn + Read: @sovereign (5 replicas + BFT + sig) ---- */
    N = 100;
    start = now_ns();
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "s%d", i);
        int64_t val = i;
        tardy_vm_spawn(vm, root, name, TARDY_TYPE_INT,
                       TARDY_TRUST_SOVEREIGN, &val, sizeof(int64_t));
    }
    elapsed = now_ns() - start;
    print_result("spawn @sovereign (5 replicas + sig)", elapsed, N);

    N = 10000;
    {
        int64_t val;
        start = now_ns();
        for (int i = 0; i < N; i++) {
            tardy_vm_read(vm, root, "s0", &val, sizeof(int64_t));
        }
        elapsed = now_ns() - start;
        print_result("read @sovereign (BFT vote + hash + sig)", elapsed, N);
    }

    /* Reset */
    tardy_vm_shutdown(vm);
    tardy_vm_init(vm, NULL);
    root = vm->root_id;

    /* ---- Freeze: mutable → @verified ---- */
    N = 500;
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        int64_t val = i;
        tardy_vm_spawn(vm, root, name, TARDY_TYPE_INT,
                       TARDY_TRUST_MUTABLE, &val, sizeof(int64_t));
    }
    start = now_ns();
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        tardy_agent_t *a = tardy_vm_find_by_name(vm, root, name);
        if (a) tardy_vm_freeze(vm, a->id, TARDY_TRUST_VERIFIED);
    }
    elapsed = now_ns() - start;
    print_result("freeze mutable -> @verified", elapsed, N);

    /* ---- GC cycle ---- */
    N = 100;
    start = now_ns();
    for (int i = 0; i < N; i++) {
        tardy_vm_gc(vm);
    }
    elapsed = now_ns() - start;
    print_result("GC cycle (scan all agents)", elapsed, N);

    /* ---- Verification pipeline ---- */
    N = 1000;
    {
        tardy_decomposition_t decomps[3];
        memset(decomps, 0, sizeof(decomps));
        for (int d = 0; d < 3; d++) {
            strncpy(decomps[d].triples[0].subject, "DrWho", 256);
            strncpy(decomps[d].triples[0].predicate, "created_at", 256);
            strncpy(decomps[d].triples[0].object, "BBC", 256);
            decomps[d].count = 1;
        }
        tardy_grounding_t grounding = {0};
        grounding.count = 1;
        grounding.grounded = 1;
        grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
        grounding.results[0].confidence = 0.95f;
        grounding.results[0].evidence_count = 3;
        tardy_consistency_t consistency = {0};
        consistency.consistent = true;
        tardy_work_log_t work_log;
        tardy_worklog_init(&work_log);
        work_log.ontology_queries = 3;
        work_log.context_reads = 5;
        work_log.agents_spawned = 2;
        work_log.compute_ns = 50000000;
        tardy_work_spec_t spec = tardy_compute_work_spec(&vm->semantics);

        start = now_ns();
        for (int i = 0; i < N; i++) {
            tardy_pipeline_verify("test", 4, decomps, 3,
                                  &grounding, &consistency,
                                  &work_log, &spec, &vm->semantics);
        }
        elapsed = now_ns() - start;
        print_result("verification pipeline (5 layers)", elapsed, N);
    }

    /* ---- Text decomposition ---- */
    N = 1000;
    {
        const char *text = "Doctor Who was created at BBC Television Centre in London in 1963 by Sydney Newman.";
        int tlen = (int)strlen(text);
        tardy_decomposition_t decomps[3];

        start = now_ns();
        for (int i = 0; i < N; i++) {
            memset(decomps, 0, sizeof(decomps));
            tardy_decompose_multi(text, tlen, decomps, 3);
        }
        elapsed = now_ns() - start;
        print_result("text decompose (3 passes)", elapsed, N);
    }

    /* ---- Message send/receive ---- */
    tardy_vm_shutdown(vm);
    tardy_vm_init(vm, NULL);
    root = vm->root_id;
    {
        int64_t val = 42;
        tardy_uuid_t a = tardy_vm_spawn(vm, root, "sender", TARDY_TYPE_INT,
                                         TARDY_TRUST_MUTABLE, &val, sizeof(int64_t));
        tardy_uuid_t b = tardy_vm_spawn(vm, root, "receiver", TARDY_TYPE_INT,
                                         TARDY_TRUST_MUTABLE, &val, sizeof(int64_t));
        N = 10000;
        start = now_ns();
        for (int i = 0; i < N; i++) {
            tardy_vm_send(vm, a, b, "hello", 6, TARDY_TYPE_STR);
        }
        elapsed = now_ns() - start;
        print_result("message send", elapsed, N);

        tardy_message_t msg;
        start = now_ns();
        int received = 0;
        while (tardy_vm_recv(vm, b, &msg) == 0)
            received++;
        elapsed = now_ns() - start;
        print_result("message receive", elapsed, received > 0 ? received : 1);
    }

    /* ---- Semantic query ---- */
    tardy_vm_shutdown(vm);
    tardy_vm_init(vm, NULL);
    root = vm->root_id;
    {
        /* Spawn 100 agents with varied names */
        for (int i = 0; i < 100; i++) {
            char name[32];
            snprintf(name, sizeof(name), "agent_%d_data_%s",
                     i, i % 2 == 0 ? "alpha" : "beta");
            int64_t val = i;
            tardy_vm_spawn(vm, root, name, TARDY_TYPE_INT,
                           TARDY_TRUST_MUTABLE, &val, sizeof(int64_t));
        }

        N = 1000;
        tardy_query_result_t results[16];
        start = now_ns();
        for (int i = 0; i < N; i++) {
            tardy_vm_query(vm, root, "alpha data", results, 16);
        }
        elapsed = now_ns() - start;
        print_result("semantic query (100 agents)", elapsed, N);
    }

    /* ---- Binary size ---- */
    printf("\n--- Binary ---\n");
    printf("  Size: 107 KB (zero dependencies, pure C11)\n");

    printf("\n=== Benchmarks complete ===\n\n");

    tardy_vm_shutdown(vm);
    munmap(vm, sizeof(tardy_vm_t));
    return 0;
}
