/*
 * Tardygrada — Ablation Benchmark
 *
 * Creates 20 test claims (10 grounded, 10 with issues), runs each
 * through the pipeline under 7 layer configurations, and measures
 * accuracy + time per verification + which claims slip through.
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
 * Timing
 * ============================================ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double ns_to_us(uint64_t ns)
{
    return (double)ns / 1000.0;
}

/* ============================================
 * Test claim categories
 * ============================================ */

typedef enum {
    CLAIM_FULLY_GROUNDED = 0,   /* should pass all layers */
    CLAIM_PARTIALLY_GROUNDED,   /* some triples unknown */
    CLAIM_CONTRADICTED,         /* should fail consistency */
    CLAIM_NO_EVIDENCE,          /* should fail grounding */
} claim_category_t;

static const char *category_name(claim_category_t c)
{
    switch (c) {
    case CLAIM_FULLY_GROUNDED:     return "fully_grounded";
    case CLAIM_PARTIALLY_GROUNDED: return "partial_ground";
    case CLAIM_CONTRADICTED:       return "contradicted";
    case CLAIM_NO_EVIDENCE:        return "no_evidence";
    }
    return "unknown";
}

/* ============================================
 * Test claim descriptor
 * ============================================ */

typedef struct {
    const char         *text;
    claim_category_t    category;
    bool                should_pass;  /* expected result under full pipeline */

    /* Pre-built pipeline inputs */
    tardy_decomposition_t  decomps[3];
    int                    decomp_count;
    tardy_grounding_t      grounding;
    tardy_consistency_t    consistency;
    tardy_work_log_t       work_log;
    tardy_work_spec_t      work_spec;
} test_claim_t;

#define NUM_CLAIMS 20

/* ============================================
 * Build shared triple for all decomposers
 * ============================================ */

static void set_triple(tardy_triple_t *t,
                       const char *s, const char *p, const char *o)
{
    strncpy(t->subject,   s, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->predicate, p, TARDY_MAX_TRIPLE_LEN - 1);
    strncpy(t->object,    o, TARDY_MAX_TRIPLE_LEN - 1);
}

/* Build 3 agreeing decompositions with the same triple */
static void build_decomps(test_claim_t *tc,
                          const char *s, const char *p, const char *o)
{
    tc->decomp_count = 3;
    for (int d = 0; d < 3; d++) {
        tc->decomps[d].count     = 1;
        tc->decomps[d].agreement = 1.0f;
        memset(&tc->decomps[d].decomposer, (uint8_t)(d + 1),
               sizeof(tardy_uuid_t));
        set_triple(&tc->decomps[d].triples[0], s, p, o);
    }
}

/* Build valid work log that meets spec */
static void build_honest_work(test_claim_t *tc, const tardy_semantics_t *sem)
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

/* ============================================
 * Build the 20 test claims
 * ============================================ */

static void build_claims(test_claim_t *claims, const tardy_semantics_t *sem)
{
    int idx = 0;

    /* ---- 5 fully grounded (should pass all) ---- */
    const char *grounded_claims[] = {
        "Water boils at 100 degrees Celsius",
        "The Earth orbits the Sun",
        "Oxygen is element number 8",
        "DNA contains adenine thymine guanine cytosine",
        "Light travels at approximately 300000 km per second",
    };
    for (int i = 0; i < 5; i++) {
        test_claim_t *tc = &claims[idx++];
        tc->text        = grounded_claims[i];
        tc->category    = CLAIM_FULLY_GROUNDED;
        tc->should_pass = true;

        build_decomps(tc, "subject", "is", "object");

        tc->grounding.count       = 1;
        tc->grounding.grounded    = 1;
        tc->grounding.consistent  = 0;
        tc->grounding.unknown     = 0;
        tc->grounding.contradicted = 0;
        tc->grounding.results[0].status         = TARDY_KNOWLEDGE_GROUNDED;
        tc->grounding.results[0].evidence_count = 3;
        tc->grounding.results[0].confidence     = 0.95f;
        set_triple(&tc->grounding.results[0].triple,
                   "subject", "is", "object");

        tc->consistency.consistent         = true;
        tc->consistency.contradiction_count = 0;
        snprintf(tc->consistency.explanation,
                 sizeof(tc->consistency.explanation), "consistent");

        build_honest_work(tc, sem);
    }

    /* ---- 5 partially grounded (some triples unknown) ---- */
    const char *partial_claims[] = {
        "Mars has subsurface water ice deposits",
        "Quantum entanglement enables faster-than-light signaling",
        "Dark matter composes 27 percent of the universe",
        "Neutrinos have a small but nonzero mass",
        "The Higgs boson mass is 125 GeV",
    };
    for (int i = 0; i < 5; i++) {
        test_claim_t *tc = &claims[idx++];
        tc->text        = partial_claims[i];
        tc->category    = CLAIM_PARTIALLY_GROUNDED;
        /*
         * Partially grounded: 1 grounded + 1 unknown.
         * With min_evidence_triples=1 and the grounded triple having
         * confidence 0.70, the probabilistic layer will fail
         * (0.70 < 0.85 threshold).
         */
        tc->should_pass = false;

        build_decomps(tc, "subject", "has", "property");

        tc->grounding.count        = 2;
        tc->grounding.grounded     = 1;
        tc->grounding.consistent   = 0;
        tc->grounding.unknown      = 1;
        tc->grounding.contradicted = 0;
        tc->grounding.results[0].status         = TARDY_KNOWLEDGE_GROUNDED;
        tc->grounding.results[0].evidence_count = 1;
        tc->grounding.results[0].confidence     = 0.70f;
        set_triple(&tc->grounding.results[0].triple,
                   "subject", "has", "property");
        tc->grounding.results[1].status         = TARDY_KNOWLEDGE_UNKNOWN;
        tc->grounding.results[1].evidence_count = 0;
        tc->grounding.results[1].confidence     = 0.0f;
        set_triple(&tc->grounding.results[1].triple,
                   "subject", "exhibits", "phenomenon");

        tc->consistency.consistent         = true;
        tc->consistency.contradiction_count = 0;
        snprintf(tc->consistency.explanation,
                 sizeof(tc->consistency.explanation), "no contradictions");

        build_honest_work(tc, sem);
    }

    /* ---- 5 contradicted (should fail consistency) ---- */
    const char *contradicted_claims[] = {
        "The Sun orbits the Earth in a perfect circle",
        "Water freezes at 200 degrees Celsius at sea level",
        "Heavier objects fall faster than lighter ones in vacuum",
        "The speed of light is 100 meters per second",
        "Electrons are larger than protons",
    };
    for (int i = 0; i < 5; i++) {
        test_claim_t *tc = &claims[idx++];
        tc->text        = contradicted_claims[i];
        tc->category    = CLAIM_CONTRADICTED;
        tc->should_pass = false;

        build_decomps(tc, "subject", "contradicts", "known_fact");

        tc->grounding.count        = 1;
        tc->grounding.grounded     = 0;
        tc->grounding.consistent   = 0;
        tc->grounding.unknown      = 0;
        tc->grounding.contradicted = 1;
        tc->grounding.results[0].status         = TARDY_KNOWLEDGE_CONTRADICTED;
        tc->grounding.results[0].evidence_count = 0;
        tc->grounding.results[0].confidence     = 0.0f;
        set_triple(&tc->grounding.results[0].triple,
                   "subject", "contradicts", "known_fact");

        tc->consistency.consistent          = false;
        tc->consistency.contradiction_count  = 1;
        snprintf(tc->consistency.explanation,
                 sizeof(tc->consistency.explanation),
                 "contradicts established knowledge");

        build_honest_work(tc, sem);
    }

    /* ---- 5 no evidence (should fail grounding) ---- */
    const char *no_evidence_claims[] = {
        "Atlantis was located beneath modern-day Antarctica",
        "Telepathy is a proven human capability",
        "Crystals emit healing frequencies at 432 Hz",
        "The moon is made of green cheese",
        "Homeopathic dilutions retain molecular memory",
    };
    for (int i = 0; i < 5; i++) {
        test_claim_t *tc = &claims[idx++];
        tc->text        = no_evidence_claims[i];
        tc->category    = CLAIM_NO_EVIDENCE;
        tc->should_pass = false;

        build_decomps(tc, "subject", "claims", "unsupported");

        tc->grounding.count        = 1;
        tc->grounding.grounded     = 0;
        tc->grounding.consistent   = 0;
        tc->grounding.unknown      = 1;
        tc->grounding.contradicted = 0;
        tc->grounding.results[0].status         = TARDY_KNOWLEDGE_UNKNOWN;
        tc->grounding.results[0].evidence_count = 0;
        tc->grounding.results[0].confidence     = 0.0f;
        set_triple(&tc->grounding.results[0].triple,
                   "subject", "claims", "unsupported");

        tc->consistency.consistent         = true;
        tc->consistency.contradiction_count = 0;
        snprintf(tc->consistency.explanation,
                 sizeof(tc->consistency.explanation),
                 "not contradicted but no evidence");

        build_honest_work(tc, sem);
    }
}

/* ============================================
 * Layer configurations for ablation
 * ============================================ */

typedef struct {
    const char               *name;
    tardy_pipeline_semantics_t pipeline;
} layer_config_t;

static layer_config_t make_all_on(void)
{
    layer_config_t c;
    c.name = "all_layers";
    c.pipeline = (tardy_pipeline_semantics_t){
        .layer_ontology_grounding    = true,
        .layer_consistency_check     = true,
        .layer_probabilistic_scoring = true,
        .layer_protocol_check        = true,
        .layer_formal_certification  = true,
        .layer_cross_representation  = true,
        .layer_work_verification     = true,
        .min_passing_layers          = 5,
        .skip_for_literals           = false,
        .skip_for_arithmetic         = false,
        .skip_for_internal_routing   = false,
    };
    return c;
}

#define NUM_CONFIGS 7

static void build_configs(layer_config_t configs[NUM_CONFIGS])
{
    /* 0: All 8 layers ON */
    configs[0] = make_all_on();

    /* 1: Layer 1 OFF (no decomposition/grounding -> both off) */
    configs[1] = make_all_on();
    configs[1].name = "no_decomp_ground";
    configs[1].pipeline.layer_ontology_grounding = false;

    /* 2: Layer 2 OFF — grounding is coupled with decomposition in
     *    pipeline_semantics, so we disable ontology_grounding.
     *    This is the same as config 1. Instead, we keep grounding
     *    but disable consistency. Relabel: */
    /* Actually, rethinking: layer_ontology_grounding controls both
     * Layer 1 (decompose) and Layer 2 (grounding) in the pipeline.
     * Let's make config 1 = no_decomp_ground, config 2 = no_consistency */
    configs[2] = make_all_on();
    configs[2].name = "no_consistency";
    configs[2].pipeline.layer_consistency_check = false;

    /* 3: Layer 3 OFF — no probabilistic scoring */
    configs[3] = make_all_on();
    configs[3].name = "no_probabilistic";
    configs[3].pipeline.layer_probabilistic_scoring = false;

    /* 4: Layer 7/8 OFF — no work verification */
    configs[4] = make_all_on();
    configs[4].name = "no_work_verify";
    configs[4].pipeline.layer_work_verification = false;

    /* 5: Only layers 1-3 (decompose + ground + consistency) */
    configs[5] = make_all_on();
    configs[5].name = "layers_1_2_3_only";
    configs[5].pipeline.layer_probabilistic_scoring = false;
    configs[5].pipeline.layer_protocol_check        = false;
    configs[5].pipeline.layer_formal_certification  = false;
    configs[5].pipeline.layer_cross_representation  = false;
    configs[5].pipeline.layer_work_verification     = false;
    configs[5].pipeline.min_passing_layers           = 2;

    /* 6: Only layers 1+8 (decompose/ground + work verify) */
    configs[6] = make_all_on();
    configs[6].name = "layers_1_and_8";
    configs[6].pipeline.layer_consistency_check     = false;
    configs[6].pipeline.layer_probabilistic_scoring = false;
    configs[6].pipeline.layer_protocol_check        = false;
    configs[6].pipeline.layer_formal_certification  = false;
    configs[6].pipeline.layer_cross_representation  = false;
    configs[6].pipeline.min_passing_layers           = 2;
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== Tardygrada Ablation Benchmark ===\n\n");

    tardy_semantics_t base_sem = TARDY_DEFAULT_SEMANTICS;

    test_claim_t claims[NUM_CLAIMS];
    build_claims(claims, &base_sem);

    layer_config_t configs[NUM_CONFIGS];
    build_configs(configs);

    /* ---- Header ---- */
    printf("%-22s  ", "Config");
    printf("%-6s  %-10s  ", "Acc", "Avg_us");
    printf("Slipped\n");
    printf("%-22s  ", "----------------------");
    printf("%-6s  %-10s  ", "------", "----------");
    printf("-------\n");

    /* Per-config results for the full table */
    for (int c = 0; c < NUM_CONFIGS; c++) {
        tardy_semantics_t sem = base_sem;
        sem.pipeline = configs[c].pipeline;

        int correct = 0;
        uint64_t total_time = 0;
        int slipped[NUM_CLAIMS];
        int slip_count = 0;

        for (int i = 0; i < NUM_CLAIMS; i++) {
            test_claim_t *tc = &claims[i];

            uint64_t t0 = now_ns();
            tardy_pipeline_result_t r = tardy_pipeline_verify(
                tc->text, (int)strlen(tc->text),
                tc->decomps, tc->decomp_count,
                &tc->grounding,
                &tc->consistency,
                &tc->work_log,
                &tc->work_spec,
                &sem);
            uint64_t dt = now_ns() - t0;
            total_time += dt;

            bool actual_pass = r.passed;
            if (actual_pass == tc->should_pass) {
                correct++;
            } else if (actual_pass && !tc->should_pass) {
                /* Bad claim slipped through */
                slipped[slip_count++] = i;
            }
        }

        double accuracy = (double)correct / NUM_CLAIMS;
        double avg_us   = ns_to_us(total_time) / NUM_CLAIMS;

        printf("%-22s  %.4f  %10.1f  ",
               configs[c].name, accuracy, avg_us);

        if (slip_count == 0) {
            printf("none");
        } else {
            for (int s = 0; s < slip_count; s++) {
                int ci = slipped[s];
                printf("#%d(%s)", ci, category_name(claims[ci].category));
                if (s < slip_count - 1) printf(",");
            }
        }
        printf("\n");
    }

    /* ---- Detailed per-claim table ---- */
    printf("\n=== Per-Claim Detail (all_layers config) ===\n");
    printf("%-3s  %-20s  %-18s  %-8s  %-8s  %s\n",
           "#", "Category", "Claim (trunc)", "Expected", "Got", "FailAt");
    printf("%-3s  %-20s  %-18s  %-8s  %-8s  %s\n",
           "---", "--------------------", "------------------",
           "--------", "--------", "------");

    tardy_semantics_t full_sem = base_sem;
    full_sem.pipeline = configs[0].pipeline;

    for (int i = 0; i < NUM_CLAIMS; i++) {
        test_claim_t *tc = &claims[i];
        tardy_pipeline_result_t r = tardy_pipeline_verify(
            tc->text, (int)strlen(tc->text),
            tc->decomps, tc->decomp_count,
            &tc->grounding,
            &tc->consistency,
            &tc->work_log,
            &tc->work_spec,
            &full_sem);

        char trunc[19];
        snprintf(trunc, sizeof(trunc), "%.18s", tc->text);

        const char *fail_layer = "none";
        if (!r.passed) {
            switch (r.failed_at) {
            case TARDY_LAYER_DECOMPOSE:     fail_layer = "decompose"; break;
            case TARDY_LAYER_GROUNDING:     fail_layer = "grounding"; break;
            case TARDY_LAYER_CONSISTENCY:   fail_layer = "consistency"; break;
            case TARDY_LAYER_PROBABILISTIC: fail_layer = "probabilistic"; break;
            case TARDY_LAYER_PROTOCOL:      fail_layer = "protocol"; break;
            case TARDY_LAYER_CERTIFICATION: fail_layer = "certification"; break;
            case TARDY_LAYER_CROSS_REP:     fail_layer = "cross_rep"; break;
            case TARDY_LAYER_WORK_VERIFY:   fail_layer = "work_verify"; break;
            default:                        fail_layer = "???"; break;
            }
        }

        printf("%-3d  %-20s  %-18s  %-8s  %-8s  %s\n",
               i,
               category_name(tc->category),
               trunc,
               tc->should_pass ? "PASS" : "FAIL",
               r.passed ? "PASS" : "FAIL",
               fail_layer);
    }

    printf("\n=== Done ===\n");
    return 0;
}
