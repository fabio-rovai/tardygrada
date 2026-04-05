/*
 * Tardygrada — Scaling Benchmark
 *
 * Sweeps agent counts from 5 to 5000, measuring wall-clock time
 * for spawn, score-store, read-back, messaging, and GC.
 * Outputs CSV for plotting scaling curves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm/types.h"
#include "vm/vm.h"
#include "vm/semantics.h"

/* ============================================
 * Timing helper
 * ============================================ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double ns_to_ms(uint64_t ns)
{
    return (double)ns / 1000000.0;
}

/* ============================================
 * Agent count sweep
 * ============================================ */

static const int AGENT_COUNTS[] = {
    5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000
};
#define NUM_SWEEPS ((int)(sizeof(AGENT_COUNTS) / sizeof(AGENT_COUNTS[0])))

#define SCORES_PER_AGENT 5
#define MAX_MESSAGES     100

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== Tardygrada Scaling Benchmark ===\n\n");

    /* CSV header */
    printf("agent_count,spawn_ms,score_ms,read_ms,message_ms,gc_ms,total_ms\n");

    for (int s = 0; s < NUM_SWEEPS; s++) {
        int N = AGENT_COUNTS[s];

        /* Skip counts that exceed VM capacity */
        if (N > TARDY_MAX_AGENTS - 1) {
            fprintf(stderr, "skipping N=%d (exceeds TARDY_MAX_AGENTS)\n", N);
            continue;
        }

        tardy_semantics_t sem = TARDY_DEFAULT_SEMANTICS;
        tardy_vm_t *vm = calloc(1, sizeof(tardy_vm_t));
        if (!vm) {
            fprintf(stderr, "OOM allocating VM for N=%d\n", N);
            continue;
        }

        tardy_vm_init(vm, &sem);

        uint64_t total_start = now_ns();

        /* --- Phase 1: Spawn N agents --- */
        uint64_t spawn_start = now_ns();

        tardy_uuid_t *agent_ids = calloc((size_t)N, sizeof(tardy_uuid_t));
        if (!agent_ids) {
            fprintf(stderr, "OOM allocating agent IDs for N=%d\n", N);
            free(vm);
            continue;
        }

        for (int i = 0; i < N; i++) {
            char name[64];
            snprintf(name, sizeof(name), "agent_%d", i);
            int64_t val = 0;
            agent_ids[i] = tardy_vm_spawn(vm, vm->root_id, name,
                                           TARDY_TYPE_INT,
                                           TARDY_TRUST_DEFAULT,
                                           &val, sizeof(val));
        }
        uint64_t spawn_ns = now_ns() - spawn_start;

        /* --- Phase 2: Each agent stores 5 scores (verified Facts) --- */
        uint64_t score_start = now_ns();

        for (int i = 0; i < N; i++) {
            for (int j = 0; j < SCORES_PER_AGENT; j++) {
                char score_name[64];
                snprintf(score_name, sizeof(score_name), "score_%d_%d", i, j);
                int64_t score_val = (int64_t)(i * 100 + j);
                tardy_vm_spawn(vm, agent_ids[i], score_name,
                               TARDY_TYPE_FACT,
                               TARDY_TRUST_VERIFIED,
                               &score_val, sizeof(score_val));
            }
        }
        uint64_t score_ns = now_ns() - score_start;

        /* --- Phase 3: Read all scores back (hash-verified) --- */
        uint64_t read_start = now_ns();

        for (int i = 0; i < N; i++) {
            for (int j = 0; j < SCORES_PER_AGENT; j++) {
                char score_name[64];
                snprintf(score_name, sizeof(score_name), "score_%d_%d", i, j);
                int64_t out = 0;
                tardy_vm_read(vm, agent_ids[i], score_name,
                              &out, sizeof(out));
            }
        }
        uint64_t read_ns = now_ns() - read_start;

        /* --- Phase 4: Challenge messages (all pairs, capped) --- */
        uint64_t msg_start = now_ns();

        int msg_count = 0;
        int64_t challenge = 42;
        for (int i = 0; i < N && msg_count < MAX_MESSAGES; i++) {
            for (int j = i + 1; j < N && msg_count < MAX_MESSAGES; j++) {
                tardy_vm_send(vm, agent_ids[i], agent_ids[j],
                              &challenge, sizeof(challenge), TARDY_TYPE_INT);
                msg_count++;
            }
        }
        uint64_t msg_ns = now_ns() - msg_start;

        /* --- Phase 5: GC --- */
        uint64_t gc_start = now_ns();
        tardy_vm_gc(vm);
        uint64_t gc_ns = now_ns() - gc_start;

        uint64_t total_ns = now_ns() - total_start;

        /* Print CSV row */
        printf("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
               N,
               ns_to_ms(spawn_ns),
               ns_to_ms(score_ns),
               ns_to_ms(read_ns),
               ns_to_ms(msg_ns),
               ns_to_ms(gc_ns),
               ns_to_ms(total_ns));

        /* Cleanup */
        free(agent_ids);
        tardy_vm_shutdown(vm);
        free(vm);
    }

    printf("\n=== Done ===\n");
    return 0;
}
