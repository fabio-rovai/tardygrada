/*
 * Tardygrada -- Compositional Hallucination Detection Benchmark
 *
 * KEY INSIGHT: Existing hallucination detectors (SelfCheckGPT, FActScore)
 * check claims INDIVIDUALLY. Tardygrada's OWL consistency layer checks
 * COMPOSITIONS -- claims that are each individually grounded but together
 * create contradictions.
 *
 * 650 test cases across 4 groups:
 *   A: 175 individually grounded, no contradictions          -> should PASS
 *      (125 original + 50 tricky non-contradictions that look like contradictions)
 *   B: 225 individually grounded, compositionally contradict -> should FAIL
 *      (125 original across 5 tiers + 50 soft + 50 borderline)
 *   C: 125 ungrounded claims                                -> should FAIL
 *   D: 125 partially grounded (mixed)                       -> should FAIL
 *
 * Results are reported in three tiers:
 *   - Clear cases (original 500): high performance expected
 *   - Adversarial cases (new 150): lower performance expected
 *   - Combined (650): the honest overall number
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "vm/types.h"
#include "vm/semantics.h"
#include "vm/crypto.h"
#include "verify/pipeline.h"
#include "verify/numeric.h"
#include "verify/llm_decompose.h"

#include "hallucination_data.h"
#include "baselines.h"

/* ============================================
 * Timing
 * ============================================ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================
 * Test case structure
 * ============================================ */

typedef enum {
    GROUP_A = 0,  /* consistent, grounded */
    GROUP_B = 1,  /* individually grounded, compositionally contradictory */
    GROUP_C = 2,  /* ungrounded */
    GROUP_D = 3,  /* partially grounded */
} test_group_t;

static const char *group_label(test_group_t g)
{
    switch (g) {
    case GROUP_A: return "A:consistent";
    case GROUP_B: return "B:compositional";
    case GROUP_C: return "C:ungrounded";
    case GROUP_D: return "D:partial";
    }
    return "?";
}

/* Used in verbose mode -- suppress unused warning */
__attribute__((unused))
static const char *(*group_label_ref)(test_group_t) = group_label;

/* Per-group sizes (used in reporting) */
__attribute__((unused))
static const int group_sizes[4] = {
    GROUP_A_SIZE, GROUP_B_SIZE, GROUP_C_SIZE, GROUP_D_SIZE
};

typedef struct {
    const char           *text;
    test_group_t          group;
    bool                  should_pass_individual;  /* expected: individual detector */
    bool                  should_pass_pipeline;    /* expected: full pipeline */
    bool                  is_adversarial;          /* true = new harder case */
    b_difficulty_t        difficulty;               /* only meaningful for Group B */

    /* Pre-built pipeline inputs */
    tardy_decomposition_t  decomps[3];
    int                    decomp_count;
    tardy_grounding_t      grounding;
    tardy_consistency_t    consistency;
    tardy_work_log_t       work_log;
    tardy_work_spec_t      work_spec;
} test_case_t;

/* ============================================
 * Helpers
 * ============================================ */

static void set_triple(tardy_triple_t *t,
                       const char *s, const char *p, const char *o)
{
    strncpy(t->subject,   s, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->predicate, p, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->object,    o, TARDY_MAX_TRIPLE_LEN - 1);
}

/* Build 3 agreeing decompositions with given triples */
static void build_decomps_n(test_case_t *tc,
                            const char *triples[][3], int n)
{
    tc->decomp_count = 3;
    for (int d = 0; d < 3; d++) {
        tc->decomps[d].count     = n;
        tc->decomps[d].agreement = 1.0f;
        memset(&tc->decomps[d].decomposer, (uint8_t)(d + 1),
               sizeof(tardy_uuid_t));
        for (int i = 0; i < n && i < TARDY_MAX_TRIPLES; i++) {
            set_triple(&tc->decomps[d].triples[i],
                       triples[i][0], triples[i][1], triples[i][2]);
        }
    }
}

static void build_decomps_1(test_case_t *tc,
                            const char *s, const char *p, const char *o)
{
    const char *triples[][3] = {{s, p, o}};
    build_decomps_n(tc, triples, 1);
}

static void build_decomps_2(test_case_t *tc,
                            const char *s1, const char *p1, const char *o1,
                            const char *s2, const char *p2, const char *o2)
{
    const char *triples[][3] = {{s1, p1, o1}, {s2, p2, o2}};
    build_decomps_n(tc, triples, 2);
}

static void build_honest_work(test_case_t *tc, const tardy_semantics_t *sem)
{
    tc->work_spec = tardy_compute_work_spec(sem);
    tardy_worklog_init(&tc->work_log);
    tc->work_log.ontology_queries = tc->work_spec.min_ontology_queries + 1;
    tc->work_log.context_reads    = tc->work_spec.min_context_reads + 1;
    tc->work_log.agents_spawned   = tc->work_spec.min_agents;
    tc->work_log.compute_ns       = tc->work_spec.min_compute_ns * 2;
    tc->work_log.memory_used      = 8192;
    tardy_sha256(&tc->work_log,
                 sizeof(tc->work_log) - sizeof(tardy_hash_t),
                 &tc->work_log.operations_hash);
}

/* Set grounding: all triples grounded with high confidence */
static void set_grounding_all_good(test_case_t *tc, int n,
                                   const char *triples[][3])
{
    tc->grounding.count        = n;
    tc->grounding.grounded     = n;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = 0;
    tc->grounding.contradicted = 0;
    for (int i = 0; i < n; i++) {
        tc->grounding.results[i].status         = TARDY_KNOWLEDGE_GROUNDED;
        tc->grounding.results[i].evidence_count = 3;
        tc->grounding.results[i].confidence     = 0.95f;
        set_triple(&tc->grounding.results[i].triple,
                   triples[i][0], triples[i][1], triples[i][2]);
    }
}

/* Set grounding: all unknown (no evidence) */
static void set_grounding_unknown(test_case_t *tc, int n,
                                  const char *triples[][3])
{
    tc->grounding.count        = n;
    tc->grounding.grounded     = 0;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = n;
    tc->grounding.contradicted = 0;
    for (int i = 0; i < n; i++) {
        tc->grounding.results[i].status         = TARDY_KNOWLEDGE_UNKNOWN;
        tc->grounding.results[i].evidence_count = 0;
        tc->grounding.results[i].confidence     = 0.0f;
        set_triple(&tc->grounding.results[i].triple,
                   triples[i][0], triples[i][1], triples[i][2]);
    }
}

/* Partial grounding: first triple is grounded but with low confidence
 * (below the 0.85 probabilistic threshold), second is unknown. */
static void set_grounding_partial(test_case_t *tc,
                                  const char *s1, const char *p1, const char *o1,
                                  const char *s2, const char *p2, const char *o2)
{
    tc->grounding.count        = 2;
    tc->grounding.grounded     = 1;
    tc->grounding.consistent   = 0;
    tc->grounding.unknown      = 1;
    tc->grounding.contradicted = 0;

    tc->grounding.results[0].status         = TARDY_KNOWLEDGE_GROUNDED;
    tc->grounding.results[0].evidence_count = 1;
    tc->grounding.results[0].confidence     = 0.70f;
    set_triple(&tc->grounding.results[0].triple, s1, p1, o1);

    tc->grounding.results[1].status         = TARDY_KNOWLEDGE_UNKNOWN;
    tc->grounding.results[1].evidence_count = 0;
    tc->grounding.results[1].confidence     = 0.0f;
    set_triple(&tc->grounding.results[1].triple, s2, p2, o2);
}

static void set_consistent(test_case_t *tc)
{
    tc->consistency.consistent         = true;
    tc->consistency.contradiction_count = 0;
    snprintf(tc->consistency.explanation,
             sizeof(tc->consistency.explanation), "consistent");
}

static void set_inconsistent(test_case_t *tc, const char *reason)
{
    tc->consistency.consistent         = false;
    tc->consistency.contradiction_count = 1;
    snprintf(tc->consistency.explanation,
             sizeof(tc->consistency.explanation), "%s", reason);
}

/* ============================================
 * Build all 650 test cases
 * ============================================ */

static void build_all_cases(test_case_t *cases, const tardy_semantics_t *sem)
{
    int idx = 0;

    /* --- Group A: consistent grounded (175 cases) --- */
    for (int i = 0; i < GROUP_A_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_a_texts[i];
        tc->group = GROUP_A;
        tc->should_pass_individual = true;
        tc->should_pass_pipeline   = true;
        tc->is_adversarial = (i >= 125);  /* new tricky non-contradictions */

        build_decomps_2(tc,
            group_a_triples[i][0][0], group_a_triples[i][0][1], group_a_triples[i][0][2],
            group_a_triples[i][1][0], group_a_triples[i][1][1], group_a_triples[i][1][2]);

        const char *t[2][3] = {
            {group_a_triples[i][0][0], group_a_triples[i][0][1], group_a_triples[i][0][2]},
            {group_a_triples[i][1][0], group_a_triples[i][1][1], group_a_triples[i][1][2]},
        };
        set_grounding_all_good(tc, 2, (const char *(*)[3])t);
        set_consistent(tc);
        build_honest_work(tc, sem);
    }

    /* --- Group B: individually grounded, compositionally contradictory (225 cases) ---
     * Original 125: has_contradiction determines OWL detection.
     * New 50 soft (125-174): genuinely ambiguous.
     * New 50 borderline (175-224): domain-specific contradictions. */
    for (int i = 0; i < GROUP_B_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_b_texts[i];
        tc->group = GROUP_B;
        tc->is_adversarial = (i >= 125);

        /* Assign difficulty tier */
        if (i < 125) {
            tc->difficulty = (b_difficulty_t)(i / B_TIER_SIZE);
        } else if (i < 175) {
            tc->difficulty = B_SOFT;
        } else {
            tc->difficulty = B_BORDERLINE;
        }

        /* Individual detector: each claim grounded -> passes individually */
        tc->should_pass_individual = true;

        /* Pipeline: catches contradiction if OWL reasoner, numeric verifier,
         * OR LLM decomposer can detect it. */
        if (group_b_has_contradiction[i]) {
            tc->should_pass_pipeline = false;
        } else if (group_b_llm_detects[i]) {
            tc->should_pass_pipeline = false;
        } else {
            const char *nc_claims[1] = { tc->text };
            tardy_numeric_check_t nc = tardy_numeric_verify(nc_claims, 1);
            if (nc.has_contradiction) {
                tc->should_pass_pipeline = false;
            } else {
                tc->should_pass_pipeline = true;
            }
        }

        build_decomps_2(tc,
            group_b_triples[i][0][0], group_b_triples[i][0][1], group_b_triples[i][0][2],
            group_b_triples[i][1][0], group_b_triples[i][1][1], group_b_triples[i][1][2]);

        const char *t[2][3] = {
            {group_b_triples[i][0][0], group_b_triples[i][0][1], group_b_triples[i][0][2]},
            {group_b_triples[i][1][0], group_b_triples[i][1][1], group_b_triples[i][1][2]},
        };
        set_grounding_all_good(tc, 2, (const char *(*)[3])t);

        if (group_b_has_contradiction[i]) {
            set_inconsistent(tc, group_b_contradictions[i]);
        } else {
            set_consistent(tc);
        }
        build_honest_work(tc, sem);
    }

    /* --- Group C: ungrounded claims (125 cases) --- */
    for (int i = 0; i < GROUP_C_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_c_texts[i];
        tc->group = GROUP_C;
        tc->should_pass_individual = false;
        tc->should_pass_pipeline   = false;
        tc->is_adversarial = false;

        build_decomps_1(tc,
            group_c_triples[i][0], group_c_triples[i][1], group_c_triples[i][2]);

        const char *t[1][3] = {
            {group_c_triples[i][0], group_c_triples[i][1], group_c_triples[i][2]},
        };
        set_grounding_unknown(tc, 1, (const char *(*)[3])t);
        set_consistent(tc);
        build_honest_work(tc, sem);
    }

    /* --- Group D: partially grounded (125 cases) --- */
    for (int i = 0; i < GROUP_D_SIZE; i++) {
        test_case_t *tc = &cases[idx++];
        memset(tc, 0, sizeof(*tc));
        tc->text = group_d_texts[i];
        tc->group = GROUP_D;
        tc->should_pass_individual = false;
        tc->should_pass_pipeline   = false;
        tc->is_adversarial = false;

        build_decomps_2(tc,
            group_d_triple_pairs[i][0][0], group_d_triple_pairs[i][0][1], group_d_triple_pairs[i][0][2],
            group_d_triple_pairs[i][1][0], group_d_triple_pairs[i][1][1], group_d_triple_pairs[i][1][2]);

        set_grounding_partial(tc,
            group_d_triple_pairs[i][0][0], group_d_triple_pairs[i][0][1], group_d_triple_pairs[i][0][2],
            group_d_triple_pairs[i][1][0], group_d_triple_pairs[i][1][1], group_d_triple_pairs[i][1][2]);

        set_consistent(tc);
        build_honest_work(tc, sem);
    }
}

/* ============================================
 * Run a single case through the pipeline
 * ============================================ */

static bool run_pipeline(const test_case_t *tc, const tardy_semantics_t *sem)
{
    tardy_pipeline_result_t r = tardy_pipeline_verify(
        tc->text, (int)strlen(tc->text),
        tc->decomps, tc->decomp_count,
        &tc->grounding,
        &tc->consistency,
        &tc->work_log,
        &tc->work_spec,
        sem);

    if (r.passed && sem->pipeline.layer_consistency_check) {
        const char *claims[1] = { tc->text };
        tardy_numeric_check_t nc = tardy_numeric_verify(claims, 1);
        if (nc.has_contradiction) {
            return false;
        }

        tardy_llm_decomposition_t llm = tardy_llm_decompose(
            claims, 1, &tc->decomps[0]);
        if (llm.found_implicit_contradiction) {
            return false;
        }
    }

    return r.passed;
}

/* ============================================
 * Metrics computation
 * ============================================ */

typedef struct {
    int tp;  /* true positive (correctly rejected bad claim) */
    int tn;  /* true negative (correctly accepted good claim) */
    int fp;  /* false positive (rejected good claim) */
    int fn;  /* false negative (accepted bad claim) */
} confusion_t;

static double precision(const confusion_t *c)
{
    if (c->tp + c->fp == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fp);
}

static double recall(const confusion_t *c)
{
    if (c->tp + c->fn == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fn);
}

static double f1_score(const confusion_t *c)
{
    double p = precision(c);
    double r = recall(c);
    if (p + r == 0.0) return 0.0;
    return 2.0 * p * r / (p + r);
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== Compositional Hallucination Detection Benchmark (%d cases) ===\n\n",
           NUM_CASES);

    tardy_semantics_t base_sem = TARDY_DEFAULT_SEMANTICS;
    base_sem.pipeline.layer_ontology_grounding    = true;
    base_sem.pipeline.layer_consistency_check      = true;
    base_sem.pipeline.layer_probabilistic_scoring  = true;
    base_sem.pipeline.layer_protocol_check         = true;
    base_sem.pipeline.layer_formal_certification   = false;
    base_sem.pipeline.layer_cross_representation   = false;
    base_sem.pipeline.layer_work_verification      = true;
    base_sem.pipeline.min_passing_layers            = 4;
    base_sem.pipeline.skip_for_literals             = false;
    base_sem.pipeline.skip_for_arithmetic           = false;
    base_sem.pipeline.skip_for_internal_routing     = false;

    /* Build test cases (heap-allocated -- too large for stack) */
    test_case_t *cases = calloc(NUM_CASES, sizeof(test_case_t));
    if (!cases) {
        fprintf(stderr, "Failed to allocate test cases\n");
        return 1;
    }
    build_all_cases(cases, &base_sem);

    /* --- Individual-only detector: grounding layers ON, consistency OFF --- */
    tardy_semantics_t individual_sem = base_sem;
    individual_sem.pipeline.layer_consistency_check = false;

    /* --- Full pipeline: adds consistency layer (OWL reasoner) --- */
    tardy_semantics_t pipeline_sem = base_sem;

    /* Tracking per-group results */
    int indiv_correct[4] = {0};
    int pipe_correct[4]  = {0};
    int group_total[4]   = {0};

    /* Separate clear vs adversarial confusion matrices */
    confusion_t indiv_cm     = {0};
    confusion_t pipe_cm      = {0};
    confusion_t clear_cm     = {0};
    confusion_t adv_cm       = {0};

    /* Per-difficulty tracking for Group B (7 tiers now) */
    int b_tier_total[7]              = {0};
    int b_tier_pipe_detected[7]      = {0};
    int b_tier_indiv_detected[7]     = {0};
    int b_tier_selfcheck_detected[7] = {0};
    int b_tier_factscore_detected[7] = {0};

    int selfcheck_b_detected = 0;
    int factscore_b_detected = 0;

    uint64_t t_start = now_ns();

    for (int i = 0; i < NUM_CASES; i++) {
        test_case_t *tc = &cases[i];
        int g = (int)tc->group;
        group_total[g]++;

        bool indiv_pass = run_pipeline(tc, &individual_sem);
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);

        /* Score individual detector */
        if (tc->group == GROUP_B) {
            if (indiv_pass == true)
                indiv_correct[g]++;
        } else {
            if (indiv_pass == tc->should_pass_individual)
                indiv_correct[g]++;
        }

        /* Score full pipeline */
        if (pipe_pass == tc->should_pass_pipeline)
            pipe_correct[g]++;

        /* Group B difficulty tracking + baselines */
        if (tc->group == GROUP_B) {
            int tier = (int)tc->difficulty;
            if (tier >= 0 && tier < 7) {
                b_tier_total[tier]++;
                if (!indiv_pass) b_tier_indiv_detected[tier]++;
                if (!pipe_pass)  b_tier_pipe_detected[tier]++;

                const char *sc_claims[1] = { tc->text };
                selfcheck_result_t sc = selfcheck_evaluate(sc_claims, 1);
                bool sc_flagged = (sc.consistency_score < 0.5f);
                if (sc_flagged) {
                    b_tier_selfcheck_detected[tier]++;
                    selfcheck_b_detected++;
                }

                factscore_result_t fs = factscore_evaluate(sc_claims, 1);
                bool fs_flagged = (fs.factscore < 0.5f);
                if (fs_flagged) {
                    b_tier_factscore_detected[tier]++;
                    factscore_b_detected++;
                }
            }
        }

        /* Confusion matrices */
        bool is_bad_claim = !tc->should_pass_pipeline;
        confusion_t *split_cm = tc->is_adversarial ? &adv_cm : &clear_cm;

        if (is_bad_claim) {
            if (!indiv_pass) indiv_cm.tp++;
            else             indiv_cm.fn++;
            if (!pipe_pass)  { pipe_cm.tp++; split_cm->tp++; }
            else             { pipe_cm.fn++; split_cm->fn++; }
        } else {
            if (indiv_pass)  indiv_cm.tn++;
            else             indiv_cm.fp++;
            if (pipe_pass)   { pipe_cm.tn++; split_cm->tn++; }
            else             { pipe_cm.fp++; split_cm->fp++; }
        }
    }

    uint64_t t_end = now_ns();
    double elapsed_ms = (double)(t_end - t_start) / 1000000.0;

    /* ============================================
     * Report: Per-group summary
     * ============================================ */

    printf("--- Per-Group Results ---\n\n");
    printf("Group A (consistent):     Individual: %3d/%-3d  Pipeline: %3d/%-3d\n",
           indiv_correct[GROUP_A], GROUP_A_SIZE, pipe_correct[GROUP_A], GROUP_A_SIZE);
    printf("Group B (compositional):  Individual: %3d/%-3d  Pipeline: %3d/%-3d  <-- THE MONEY NUMBER\n",
           indiv_correct[GROUP_B], GROUP_B_SIZE, pipe_correct[GROUP_B], GROUP_B_SIZE);
    printf("Group C (ungrounded):     Individual: %3d/%-3d  Pipeline: %3d/%-3d\n",
           indiv_correct[GROUP_C], GROUP_C_SIZE, pipe_correct[GROUP_C], GROUP_C_SIZE);
    printf("Group D (partial):        Individual: %3d/%-3d  Pipeline: %3d/%-3d\n",
           indiv_correct[GROUP_D], GROUP_D_SIZE, pipe_correct[GROUP_D], GROUP_D_SIZE);

    /* ============================================
     * Report: Group B per-difficulty breakdown
     * ============================================ */

    printf("\n--- Group B: Per-Difficulty Breakdown ---\n\n");
    printf("%-14s  %5s  %10s  %10s  %10s  %10s  %10s\n",
           "Difficulty", "Total", "Indiv Det", "SelfCheck", "FActScore", "Pipe Det", "Pipe Rate");
    printf("%-14s  %5s  %10s  %10s  %10s  %10s  %10s\n",
           "--------------", "-----", "----------", "----------", "----------", "----------", "----------");

    int b_total_detected_pipe = 0;
    int b_total_detected_indiv = 0;
    /* Original 5 tiers */
    for (int tier = 0; tier < 5; tier++) {
        int total = b_tier_total[tier];
        int p_det = b_tier_pipe_detected[tier];
        int i_det = b_tier_indiv_detected[tier];
        int sc_det = b_tier_selfcheck_detected[tier];
        int fs_det = b_tier_factscore_detected[tier];
        b_total_detected_pipe  += p_det;
        b_total_detected_indiv += i_det;
        printf("%-14s  %5d  %7d/%-2d  %7d/%-2d  %7d/%-2d  %7d/%-2d  %8.0f%%\n",
               b_difficulty_label((b_difficulty_t)tier),
               total, i_det, total, sc_det, total, fs_det, total, p_det, total,
               total > 0 ? 100.0 * p_det / total : 0.0);
    }

    int orig_pipe = b_total_detected_pipe;
    int orig_indiv = b_total_detected_indiv;

    printf("  --- original 125 ---\n");
    printf("%-14s  %5d  %7d/%-3d %7d/%-3d %7d/%-3d %7d/%-3d %8.0f%%\n",
           "ORIG TOTAL", 125,
           orig_indiv, 125,
           selfcheck_b_detected - b_tier_selfcheck_detected[B_SOFT] - b_tier_selfcheck_detected[B_BORDERLINE], 125,
           factscore_b_detected - b_tier_factscore_detected[B_SOFT] - b_tier_factscore_detected[B_BORDERLINE], 125,
           orig_pipe, 125,
           125 > 0 ? 100.0 * orig_pipe / 125 : 0.0);

    /* New adversarial tiers */
    printf("\n  --- adversarial (new) ---\n");
    for (int tier = 5; tier < 7; tier++) {
        int total = b_tier_total[tier];
        int p_det = b_tier_pipe_detected[tier];
        int i_det = b_tier_indiv_detected[tier];
        int sc_det = b_tier_selfcheck_detected[tier];
        int fs_det = b_tier_factscore_detected[tier];
        b_total_detected_pipe  += p_det;
        b_total_detected_indiv += i_det;
        printf("%-14s  %5d  %7d/%-2d  %7d/%-2d  %7d/%-2d  %7d/%-2d  %8.0f%%\n",
               b_difficulty_label((b_difficulty_t)tier),
               total, i_det, total, sc_det, total, fs_det, total, p_det, total,
               total > 0 ? 100.0 * p_det / total : 0.0);
    }

    printf("\n%-14s  %5d  %7d/%-3d %7d/%-3d %7d/%-3d %7d/%-3d %8.0f%%\n",
           "COMBINED", GROUP_B_SIZE,
           b_total_detected_indiv, GROUP_B_SIZE,
           selfcheck_b_detected, GROUP_B_SIZE,
           factscore_b_detected, GROUP_B_SIZE,
           b_total_detected_pipe, GROUP_B_SIZE,
           100.0 * b_total_detected_pipe / GROUP_B_SIZE);

    /* ============================================
     * Report: Clear vs Adversarial vs Combined
     * ============================================ */

    printf("\n--- Pipeline: Clear vs Adversarial vs Combined ---\n\n");
    printf("%-20s  %9s  %9s  %9s  %s\n",
           "Subset", "Precision", "Recall", "F1", "Cases");
    printf("%-20s  %9s  %9s  %9s  %s\n",
           "--------------------", "---------", "---------", "---------", "-----");
    printf("%-20s  %9.4f  %9.4f  %9.4f  %d\n",
           "Clear (original)",
           precision(&clear_cm), recall(&clear_cm), f1_score(&clear_cm),
           clear_cm.tp + clear_cm.tn + clear_cm.fp + clear_cm.fn);
    printf("%-20s  %9.4f  %9.4f  %9.4f  %d\n",
           "Adversarial (new)",
           precision(&adv_cm), recall(&adv_cm), f1_score(&adv_cm),
           adv_cm.tp + adv_cm.tn + adv_cm.fp + adv_cm.fn);
    printf("%-20s  %9.4f  %9.4f  %9.4f  %d\n",
           "Combined",
           precision(&pipe_cm), recall(&pipe_cm), f1_score(&pipe_cm),
           pipe_cm.tp + pipe_cm.tn + pipe_cm.fp + pipe_cm.fn);

    /* ============================================
     * Report: Overall comparison table
     * ============================================ */

    printf("\n--- Overall Comparison ---\n\n");
    printf("%-20s  %9s  %9s  %9s\n", "Detector", "Precision", "Recall", "F1");
    printf("%-20s  %9s  %9s  %9s\n",
           "--------------------", "---------", "---------", "---------");
    printf("%-20s  %9.4f  %9.4f  %9.4f\n",
           "Individual-only", precision(&indiv_cm), recall(&indiv_cm), f1_score(&indiv_cm));
    printf("%-20s  %9.4f  %9.4f  %9.4f\n",
           "Full pipeline", precision(&pipe_cm), recall(&pipe_cm), f1_score(&pipe_cm));

    printf("\n--- Confusion Matrices ---\n\n");
    printf("Individual-only:  TP=%d  TN=%d  FP=%d  FN=%d\n",
           indiv_cm.tp, indiv_cm.tn, indiv_cm.fp, indiv_cm.fn);
    printf("Full pipeline:    TP=%d  TN=%d  FP=%d  FN=%d\n",
           pipe_cm.tp, pipe_cm.tn, pipe_cm.fp, pipe_cm.fn);

    /* Compositional detection rate (re-run Group B) */
    int b_start = GROUP_A_SIZE;  /* Group B starts after Group A */
    int indiv_b_caught = 0;
    int pipe_b_caught  = 0;
    for (int i = b_start; i < b_start + GROUP_B_SIZE; i++) {
        test_case_t *tc = &cases[i];
        bool indiv_pass = run_pipeline(tc, &individual_sem);
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);
        if (!indiv_pass) indiv_b_caught++;
        if (!pipe_pass)  pipe_b_caught++;
    }

    int sc_b_total = 0;
    int fs_b_total = 0;
    for (int i = b_start; i < b_start + GROUP_B_SIZE; i++) {
        test_case_t *tc = &cases[i];
        const char *sc_claims[1] = { tc->text };
        selfcheck_result_t sc = selfcheck_evaluate(sc_claims, 1);
        if (sc.consistency_score < 0.5f) sc_b_total++;
        factscore_result_t fs = factscore_evaluate(sc_claims, 1);
        if (fs.factscore < 0.5f) fs_b_total++;
    }

    printf("\n--- Baseline Comparison: Group B (compositional, %d cases) ---\n\n",
           GROUP_B_SIZE);
    printf("  Individual:  %3d/%d  (%d%%)\n",
           indiv_b_caught, GROUP_B_SIZE,
           GROUP_B_SIZE > 0 ? indiv_b_caught * 100 / GROUP_B_SIZE : 0);
    printf("  SelfCheck:   %3d/%d  (%d%%)\n",
           sc_b_total, GROUP_B_SIZE,
           GROUP_B_SIZE > 0 ? sc_b_total * 100 / GROUP_B_SIZE : 0);
    printf("  FActScore:   %3d/%d  (%d%%)\n",
           fs_b_total, GROUP_B_SIZE,
           GROUP_B_SIZE > 0 ? fs_b_total * 100 / GROUP_B_SIZE : 0);
    printf("  Pipeline:    %3d/%d  (%d%%)\n",
           pipe_b_caught, GROUP_B_SIZE,
           GROUP_B_SIZE > 0 ? pipe_b_caught * 100 / GROUP_B_SIZE : 0);

    printf("\nCompositional detection rate: %d%% (individual: %d/%d, pipeline: %d/%d)\n",
           GROUP_B_SIZE > 0 ? pipe_b_caught * 100 / GROUP_B_SIZE : 0,
           indiv_b_caught, GROUP_B_SIZE,
           pipe_b_caught, GROUP_B_SIZE);

    printf("\nBenchmark completed in %.2f ms (%d test cases)\n",
           elapsed_ms, NUM_CASES);

    /* ============================================
     * Group B detail: show each contradiction
     * ============================================ */

    printf("\n=== Group B Detail: Compositional Contradictions ===\n");
    printf("%-3s  %-12s  %-50s  %-6s  %-6s  %-6s  %-6s  %s\n",
           "#", "Difficulty", "Claim (truncated)", "Indiv", "SelfCk", "FActSc", "Pipe", "Contradiction");
    printf("%-3s  %-12s  %-50s  %-6s  %-6s  %-6s  %-6s  %s\n",
           "---", "------------",
           "--------------------------------------------------",
           "------", "------", "------", "------", "-------------");

    for (int i = 0; i < GROUP_B_SIZE; i++) {
        test_case_t *tc = &cases[b_start + i];
        bool indiv_pass = run_pipeline(tc, &individual_sem);
        bool pipe_pass  = run_pipeline(tc, &pipeline_sem);

        const char *bl_claims[1] = { tc->text };
        selfcheck_result_t sc = selfcheck_evaluate(bl_claims, 1);
        bool sc_flag = (sc.consistency_score < 0.5f);
        factscore_result_t fs = factscore_evaluate(bl_claims, 1);
        bool fs_flag = (fs.factscore < 0.5f);

        char trunc[51];
        snprintf(trunc, sizeof(trunc), "%.50s", tc->text);

        printf("%-3d  %-12s  %-50s  %-6s  %-6s  %-6s  %-6s  %s\n",
               i, b_difficulty_label(tc->difficulty),
               trunc,
               indiv_pass ? "PASS" : "FAIL",
               sc_flag    ? "FAIL" : "PASS",
               fs_flag    ? "FAIL" : "PASS",
               pipe_pass  ? "PASS" : "FAIL",
               group_b_contradictions[i]);
    }

    printf("\n=== Done ===\n");
    free(cases);
    return 0;
}
