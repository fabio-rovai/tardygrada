/*
 * Tardygrada — Laziness Detection Evaluation Harness
 *
 * Constructs 50 synthetic agent traces (25 honest, 25 lazy),
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

#define NUM_TRACES 50
#define HONEST     25
#define LAZY       25

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
        /*
         * Shows sufficient work numerically, but semantics flag
         * max_work_similarity = 0.99 — near-identical to another agent.
         * The work verification layer checks total_ops and thresholds;
         * copied work that meets thresholds passes the numeric checks.
         * We set just-barely-sufficient numbers so it passes the
         * numeric checks — the copy detection would need the similarity
         * metric from the laziness semantics (max_work_similarity).
         */
        t->log.ontology_queries = spec.min_ontology_queries;
        t->log.context_reads    = spec.min_context_reads;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns;
        t->log.memory_used      = 4096;
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
        /*
         * Shows verification work but max_verification_chain exceeded.
         * Like copied, circular verification requires a chain-depth
         * metric beyond the numeric work checks. We set sufficient
         * numbers so the numeric layer passes.
         */
        t->log.ontology_queries = spec.min_ontology_queries;
        t->log.context_reads    = spec.min_context_reads;
        t->log.agents_spawned   = 1;
        t->log.compute_ns       = spec.min_compute_ns;
        t->log.memory_used      = 4096;
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

    printf("%-4s  %-12s  %-8s  %-8s  %-8s  %s\n",
           "#", "Label", "Expected", "Got", "Correct", "Detail");
    printf("%-4s  %-12s  %-8s  %-8s  %-8s  %s\n",
           "----", "------------", "--------", "--------", "--------",
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

        printf("%-4d  %-12s  %-8s  %-8s  %-8s  %.60s\n",
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

    printf("\n=== Notes ===\n");
    printf("  Copied and Circular types require similarity/chain-depth\n");
    printf("  metrics not yet in tardy_verify_work(). These are expected\n");
    printf("  false negatives — roadmap items for the laziness detector.\n");

    return 0;
}
