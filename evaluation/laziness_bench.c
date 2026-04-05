/*
 * Tardygrada — Laziness Detection Evaluation Harness
 *
 * Constructs 60 synthetic agent traces (30 honest, 25 lazy, 5 edge cases),
 * runs each through tardy_verify_work(), and computes
 * precision / recall / F1 with per-type detection rates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm/types.h"
#include "vm/semantics.h"
#include "vm/crypto.h"
#include "verify/pipeline.h"

/* ============================================
 * Test trace descriptor
 * ============================================ */

typedef struct {
    const char          *label;
    tardy_laziness_t     expected_type;  /* NONE = honest */
    bool                 expected_lazy;
    tardy_work_log_t     log;
    tardy_work_spec_t    spec;
} trace_t;

#define NUM_TRACES 60
#define HONEST     25
#define LAZY       25
#define EDGE       10

/* ============================================
 * Helpers
 * ============================================ */

/* Build a valid operations hash from the log contents */
static tardy_hash_t make_valid_hash(const tardy_work_log_t *log)
{
    tardy_hash_t h;
    tardy_sha256(log, sizeof(*log) - sizeof(tardy_hash_t), &h);
    return h;
}

/* Build a bogus operations hash (tampered) */
static tardy_hash_t make_bogus_hash(void)
{
    tardy_hash_t h;
    memset(&h, 0xAB, sizeof(h));
    return h;
}

/* ============================================
 * Construct traces
 * ============================================ */

static void build_traces(trace_t *traces, const tardy_semantics_t *sem)
{
    tardy_work_spec_t spec = tardy_compute_work_spec(sem);
    int idx = 0;

    /* ---- 25 HONEST traces ---- */
    for (int i = 0; i < HONEST; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "honest";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* Meet or exceed every threshold */
        t->log.ontology_queries = spec.min_ontology_queries + i;
        t->log.context_reads    = spec.min_context_reads + i;
        t->log.agents_spawned   = spec.min_agents;
        t->log.compute_ns       = spec.min_compute_ns + (uint64_t)(i + 1) * 500000;
        t->log.memory_used      = 4096 * (size_t)(i + 1);
        t->log.work_similarity  = 0.0f;  /* unique work */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* ---- 5 NO_WORK traces ---- */
    for (int i = 0; i < 5; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "no_work";
        t->expected_type = TARDY_LAZY_NO_WORK;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* All zeros — agent claims output but did nothing */
        t->log.ontology_queries = 0;
        t->log.context_reads    = 0;
        t->log.agents_spawned   = 0;
        t->log.compute_ns       = 0;
        t->log.memory_used      = 0;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* ---- 5 SHALLOW traces ---- */
    for (int i = 0; i < 5; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "shallow";
        t->expected_type = TARDY_LAZY_SHALLOW;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* 1 query, 0 context reads, minimal compute — below thresholds */
        t->log.ontology_queries = 1;
        t->log.context_reads    = 0;
        t->log.agents_spawned   = 0;
        t->log.compute_ns       = 100;   /* well below min_compute_ns */
        t->log.memory_used      = 64;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* ---- 5 FAKE_PROOF traces ---- */
    for (int i = 0; i < 5; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "fake_proof";
        t->expected_type = TARDY_LAZY_FAKE_PROOF;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* Claims lots of work but hash doesn't match (tampered) */
        t->log.ontology_queries = 5;
        t->log.context_reads    = 4;
        t->log.agents_spawned   = 2;
        t->log.compute_ns       = spec.min_compute_ns * 2;
        t->log.memory_used      = 8192;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_bogus_hash();
    }

    /* ---- 5 COPIED traces ---- */
    for (int i = 0; i < 5; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "copied";
        t->expected_type = TARDY_LAZY_COPIED;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* Meets numeric thresholds but near-identical to another agent */
        t->log.ontology_queries = spec.min_ontology_queries;
        t->log.context_reads    = spec.min_context_reads;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.99f;  /* above max_work_similarity (0.95) */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* ---- 5 CIRCULAR traces ---- */
    for (int i = 0; i < 5; i++) {
        trace_t *t = &traces[idx++];
        t->label         = "circular";
        t->expected_type = TARDY_LAZY_CIRCULAR;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        /* Meets numeric thresholds but verification chain too deep */
        t->log.ontology_queries = spec.min_ontology_queries;
        t->log.context_reads    = spec.min_context_reads;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 5;  /* above max_verification_chain (3) */
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* ---- 10 EDGE CASE traces ---- */

    /* Edge 1: similarity just below threshold (0.94 < 0.95) — should PASS */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_sim_ok";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 2;
        t->log.context_reads    = spec.min_context_reads + 2;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 100000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.94f;  /* just below 0.95 threshold */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 2: chain depth exactly at max (3 == 3) — should PASS (> not >=) */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_chain_ok";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 1;
        t->log.context_reads    = spec.min_context_reads + 1;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 50000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 3;  /* exactly at max — not exceeded */
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 3: valid hash but low work — should be SHALLOW not FAKE_PROOF */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_valid_low";
        t->expected_type = TARDY_LAZY_SHALLOW;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = 1;
        t->log.context_reads    = 0;
        t->log.agents_spawned   = 0;
        t->log.compute_ns       = 50;
        t->log.memory_used      = 32;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);  /* valid hash */
    }

    /* Edge 4: high similarity AND low work — should be caught as NO_WORK first */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_sim+nowork";
        t->expected_type = TARDY_LAZY_NO_WORK;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = 0;
        t->log.context_reads    = 0;
        t->log.agents_spawned   = 0;
        t->log.compute_ns       = 0;
        t->log.memory_used      = 0;
        t->log.work_similarity  = 0.99f;  /* also high similarity */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 5: zero hash (never set) — should NOT be flagged as FAKE_PROOF */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_zero_hash";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 3;
        t->log.context_reads    = spec.min_context_reads + 3;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 200000;
        t->log.memory_used      = 8192;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 0;
        /* operations_hash stays zero from worklog_init — should pass */
    }

    /* Edge 6: similarity exactly at threshold (0.95 == 0.95) — should PASS
     * because check is > not >= */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_sim_eq";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 1;
        t->log.context_reads    = spec.min_context_reads + 1;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 100000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.95f;  /* exactly at threshold */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 7: chain depth 4 (just above max 3) — should be CIRCULAR */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_chain_4";
        t->expected_type = TARDY_LAZY_CIRCULAR;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 1;
        t->log.context_reads    = spec.min_context_reads + 1;
        t->log.agents_spawned   = 2;
        t->log.compute_ns       = spec.min_compute_ns + 100000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.0f;
        t->log.verification_chain_depth = 4;  /* just above max 3 */
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 8: fake hash + high similarity — FAKE_PROOF wins (checked first) */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_fake+copy";
        t->expected_type = TARDY_LAZY_FAKE_PROOF;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 2;
        t->log.context_reads    = spec.min_context_reads + 2;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 100000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.99f;  /* also copied */
        t->log.verification_chain_depth = 0;
        t->log.operations_hash  = make_bogus_hash();  /* but hash is fake */
    }

    /* Edge 9: copied + deep chain — COPIED wins (checked before circular) */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_copy+circ";
        t->expected_type = TARDY_LAZY_COPIED;
        t->expected_lazy = true;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 1;
        t->log.context_reads    = spec.min_context_reads + 1;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns + 50000;
        t->log.memory_used      = 4096;
        t->log.work_similarity  = 0.98f;  /* above threshold */
        t->log.verification_chain_depth = 5;  /* also above chain max */
        t->log.operations_hash  = make_valid_hash(&t->log);
    }

    /* Edge 10: everything maxed out but honest — should PASS */
    {
        trace_t *t = &traces[idx++];
        t->label         = "edge_max_ok";
        t->expected_type = TARDY_LAZY_NONE;
        t->expected_lazy = false;
        t->spec          = spec;

        tardy_worklog_init(&t->log);
        t->log.ontology_queries = spec.min_ontology_queries + 100;
        t->log.context_reads    = spec.min_context_reads + 100;
        t->log.agents_spawned   = 10;
        t->log.compute_ns       = spec.min_compute_ns + 10000000;
        t->log.memory_used      = 1048576;
        t->log.work_similarity  = 0.10f;  /* low similarity */
        t->log.verification_chain_depth = 1;  /* well below max */
        t->log.operations_hash  = make_valid_hash(&t->log);
    }
}

/* ============================================
 * Metrics
 * ============================================ */

typedef struct {
    int tp, fp, tn, fn;
} confusion_t;

static double precision(const confusion_t *c)
{
    return (c->tp + c->fp) > 0 ? (double)c->tp / (c->tp + c->fp) : 0.0;
}

static double recall(const confusion_t *c)
{
    return (c->tp + c->fn) > 0 ? (double)c->tp / (c->tp + c->fn) : 0.0;
}

static double f1(const confusion_t *c)
{
    double p = precision(c);
    double r = recall(c);
    return (p + r) > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== Tardygrada Laziness Detection Evaluation ===\n\n");

    tardy_semantics_t sem = TARDY_DEFAULT_SEMANTICS;
    /* Enable all pipeline layers for thorough testing */
    sem.pipeline.layer_work_verification = true;

    trace_t traces[NUM_TRACES];
    build_traces(traces, &sem);

    confusion_t overall = {0, 0, 0, 0};

    /* Per-type counters: how many of each type were correctly detected */
    const char *type_names[] = {
        "NoWork", "Shallow", "FakeProof", "Copied", "Circular"
    };
    int type_detected[5]  = {0};
    int type_total[5]     = {0};

    printf("%-4s  %-14s  %-8s  %-8s  %-8s  %s\n",
           "#", "Label", "Expected", "Got", "Correct", "Detail");
    printf("%-4s  %-14s  %-8s  %-8s  %-8s  %s\n",
           "----", "--------------", "--------", "--------", "--------",
           "-------------------------------");

    for (int i = 0; i < NUM_TRACES; i++) {
        trace_t *t = &traces[i];

        tardy_layer_result_t r = tardy_verify_work(&t->log, &t->spec, &sem);

        bool detected_lazy = !r.passed;

        /* Classify */
        if (t->expected_lazy && detected_lazy)       overall.tp++;
        else if (!t->expected_lazy && detected_lazy)  overall.fp++;
        else if (!t->expected_lazy && !detected_lazy) overall.tn++;
        else                                          overall.fn++;

        bool correct = (t->expected_lazy == detected_lazy);

        /* Per-type tracking */
        if (t->expected_type != TARDY_LAZY_NONE) {
            int tidx = (int)t->expected_type - 1;  /* enum starts at 1 */
            if (tidx >= 0 && tidx < 5) {
                type_total[tidx]++;
                if (detected_lazy)
                    type_detected[tidx]++;
            }
        }

        printf("%-4d  %-14s  %-8s  %-8s  %-8s  %.60s\n",
               i,
               t->label,
               t->expected_lazy ? "LAZY" : "HONEST",
               detected_lazy ? "LAZY" : "HONEST",
               correct ? "YES" : "NO",
               r.detail);
    }

    /* ---- Summary ---- */
    printf("\n=== Overall Metrics ===\n");
    printf("  TP: %d  FP: %d  TN: %d  FN: %d\n",
           overall.tp, overall.fp, overall.tn, overall.fn);
    printf("  Precision: %.4f\n", precision(&overall));
    printf("  Recall:    %.4f\n", recall(&overall));
    printf("  F1:        %.4f\n", f1(&overall));

    printf("\n=== Per-Type Detection Rate ===\n");
    printf("%-14s  %s/%s  %s\n", "Type", "Detected", "Total", "Rate");
    printf("%-14s  %s/%s  %s\n", "--------------", "--------", "-----", "------");
    for (int i = 0; i < 5; i++) {
        double rate = type_total[i] > 0
                          ? (double)type_detected[i] / type_total[i]
                          : 0.0;
        printf("%-14s  %d/%d        %.2f%%\n",
               type_names[i], type_detected[i], type_total[i], rate * 100.0);
    }

    printf("\n=== Edge Case Summary ===\n");
    printf("  10 edge cases test boundary conditions:\n");
    printf("  - Similarity just below/at threshold\n");
    printf("  - Chain depth at/just above max\n");
    printf("  - Valid hash with low work (SHALLOW not FAKE_PROOF)\n");
    printf("  - Combined laziness types (priority ordering)\n");
    printf("  - Zero hash (should not trigger FAKE_PROOF)\n");

    return 0;
}
