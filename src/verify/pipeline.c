/*
 * Tardygrada — Verification Pipeline Implementation
 *
 * 8 layers. Fail fast. The VM is the incorruptible judge.
 * Deterministic C. Not an LLM. Can't be bribed, lazy, or hallucinate.
 */

#include "pipeline.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================
 * Work Log — the dashcam
 * ============================================ */

void tardy_worklog_init(tardy_work_log_t *log)
{
    memset(log, 0, sizeof(tardy_work_log_t));
}

void tardy_worklog_record_query(tardy_work_log_t *log)
{
    log->ontology_queries++;
}

void tardy_worklog_record_read(tardy_work_log_t *log)
{
    log->context_reads++;
}

void tardy_worklog_record_spawn(tardy_work_log_t *log)
{
    log->agents_spawned++;
}

void tardy_worklog_record_compute(tardy_work_log_t *log, uint64_t ns)
{
    log->compute_ns += ns;
}

void tardy_worklog_record_memory(tardy_work_log_t *log, size_t bytes)
{
    log->memory_used += bytes;
}

/* ============================================
 * Work Spec — minimum work required
 * Deterministic. Computed from semantics.
 * ============================================ */

tardy_work_spec_t tardy_compute_work_spec(const tardy_semantics_t *sem)
{
    tardy_work_spec_t spec = {0};

    /* Must query at least as many ontologies as decomposers require */
    spec.min_ontology_queries = sem->hallucination.require_dual_ontology ? 2 : 1;

    /* Must read at least the minimum evidence triples */
    spec.min_context_reads = sem->truth.min_evidence_triples;

    /* Must have minimum consensus agents */
    spec.min_agents = sem->truth.min_consensus_agents;

    /* Minimum compute time: at least 1ms (prevents instant fake responses) */
    spec.min_compute_ns = 1000000;

    /* Minimum evidence triples from semantics */
    spec.min_evidence_triples = sem->truth.min_evidence_triples;

    return spec;
}

/* ============================================
 * Layer 1: Decomposition Verification
 *
 * Multiple agents decomposed the text independently.
 * Check: did enough agents participate? Do they agree?
 * ============================================ */

tardy_layer_result_t tardy_verify_decomposition(
    const tardy_decomposition_t *decomps, int count,
    const tardy_semantics_t *sem)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_DECOMPOSE;

    /* Check minimum decomposers */
    if (count < sem->hallucination.min_decomposers) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail),
                 "only %d decomposers, need %d",
                 count, sem->hallucination.min_decomposers);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Compute pairwise agreement between decomposers */
    float total_agreement = 0.0f;
    int pairs = 0;

    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            /* Count overlapping triples */
            int overlap = 0;
            int total = decomps[i].count + decomps[j].count;
            if (total == 0) continue;

            for (int a = 0; a < decomps[i].count; a++) {
                for (int b = 0; b < decomps[j].count; b++) {
                    if (strcmp(decomps[i].triples[a].subject,
                              decomps[j].triples[b].subject) == 0 &&
                        strcmp(decomps[i].triples[a].predicate,
                              decomps[j].triples[b].predicate) == 0 &&
                        strcmp(decomps[i].triples[a].object,
                              decomps[j].triples[b].object) == 0) {
                        overlap++;
                        break;
                    }
                }
            }
            /* Jaccard-like: overlap / union */
            float agreement = (float)(overlap * 2) / (float)total;
            total_agreement += agreement;
            pairs++;
        }
    }

    float avg_agreement = pairs > 0 ? total_agreement / (float)pairs : 0.0f;

    if (avg_agreement < sem->hallucination.min_decomposition_agreement) {
        r.passed = false;
        r.confidence = avg_agreement;
        snprintf(r.detail, sizeof(r.detail),
                 "decomposition agreement %.2f < %.2f threshold",
                 avg_agreement, sem->hallucination.min_decomposition_agreement);
    } else {
        r.passed = true;
        r.confidence = avg_agreement;
        snprintf(r.detail, sizeof(r.detail),
                 "%d decomposers agree at %.2f", count, avg_agreement);
    }

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Layer 2: Ontology Grounding
 *
 * Each triple checked against the knowledge graph.
 * Contradicted = hallucination. Grounded = evidence.
 * ============================================ */

tardy_layer_result_t tardy_verify_grounding(
    const tardy_grounding_t *grounding,
    const tardy_semantics_t *sem)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_GROUNDING;

    if (!grounding || grounding->count == 0) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail), "no grounding data");
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Any contradictions = immediate fail */
    if (grounding->contradicted > 0) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail),
                 "%d triples contradicted by ontology — hallucination detected",
                 grounding->contradicted);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Need minimum grounded triples */
    if (grounding->grounded < sem->truth.min_evidence_triples) {
        r.passed = false;
        r.confidence = (float)grounding->grounded / (float)grounding->count;
        snprintf(r.detail, sizeof(r.detail),
                 "only %d grounded triples, need %d",
                 grounding->grounded, sem->truth.min_evidence_triples);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Compute grounding ratio */
    float ratio = (float)grounding->grounded / (float)grounding->count;
    r.passed = true;
    r.confidence = ratio;
    snprintf(r.detail, sizeof(r.detail),
             "%d/%d triples grounded (%.0f%%), %d unknown",
             grounding->grounded, grounding->count,
             ratio * 100.0f, grounding->unknown);

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Layer 3: Consistency Check
 *
 * OWL reasoner: do the grounded triples contradict each other
 * or the existing knowledge graph?
 * ============================================ */

tardy_layer_result_t tardy_verify_consistency(
    const tardy_consistency_t *consistency,
    const tardy_semantics_t *sem)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_CONSISTENCY;

    if (!consistency) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail), "no consistency data");
        r.compute_ns = now_ns() - start;
        return r;
    }

    if (consistency->contradiction_count > sem->truth.max_contradictions) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail),
                 "%d contradictions found: %s",
                 consistency->contradiction_count, consistency->explanation);
    } else {
        r.passed = consistency->consistent;
        r.confidence = consistency->consistent ? 1.0f : 0.0f;
        snprintf(r.detail, sizeof(r.detail),
                 consistency->consistent ? "consistent" : "inconsistent");
    }

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Layer 4: Probabilistic Scoring
 *
 * Quantitative confidence across all grounded triples.
 * Aggregates evidence strength.
 * ============================================ */

tardy_layer_result_t tardy_verify_probabilistic(
    const tardy_grounding_t *grounding,
    const tardy_semantics_t *sem)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_PROBABILISTIC;

    if (!grounding || grounding->count == 0) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail), "no data for scoring");
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Average confidence across all grounded triples */
    float total_conf = 0.0f;
    int counted = 0;
    for (int i = 0; i < grounding->count; i++) {
        if (grounding->results[i].status == TARDY_KNOWLEDGE_GROUNDED) {
            total_conf += grounding->results[i].confidence;
            counted++;
        }
    }

    float avg = counted > 0 ? total_conf / (float)counted : 0.0f;

    if (avg < sem->truth.min_confidence) {
        r.passed = false;
        r.confidence = avg;
        snprintf(r.detail, sizeof(r.detail),
                 "confidence %.3f < %.3f threshold",
                 avg, sem->truth.min_confidence);
    } else {
        r.passed = true;
        r.confidence = avg;
        snprintf(r.detail, sizeof(r.detail),
                 "confidence %.3f from %d grounded triples", avg, counted);
    }

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Layer 5: Protocol Check
 *
 * Session types: did the agent follow the protocol?
 * Yoshida MPST style — currently stub.
 * ============================================ */

tardy_layer_result_t tardy_verify_protocol(void)
{
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_PROTOCOL;
    r.passed = true;
    r.confidence = 1.0f;
    snprintf(r.detail, sizeof(r.detail),
             "protocol check — session types pending");
    return r;
}

/* ============================================
 * Layer 6: Formal Certification
 *
 * Proof-certificate asymmetry (davidad):
 * agent produces proof, VM checks cheaply.
 * Currently stub — opt-in, expensive.
 * ============================================ */

tardy_layer_result_t tardy_verify_certification(void)
{
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_CERTIFICATION;
    r.passed = true;
    r.confidence = 1.0f;
    snprintf(r.detail, sizeof(r.detail),
             "formal certification — Coq integration pending");
    return r;
}

/* ============================================
 * Layer 7: Cross-Representation Bridge
 *
 * All previous layers must agree.
 * If grounding says "confident" but consistency says
 * "contradicted", something is wrong.
 * ============================================ */

tardy_layer_result_t tardy_verify_cross_representation(
    const tardy_layer_result_t *layers, int count)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_CROSS_REP;

    /* Check for conflicting signals */
    bool has_high_confidence = false;
    bool has_low_confidence = false;

    for (int i = 0; i < count; i++) {
        if (layers[i].confidence > 0.9f)
            has_high_confidence = true;
        if (layers[i].confidence < 0.3f && layers[i].layer != TARDY_LAYER_CROSS_REP)
            has_low_confidence = true;
    }

    /* If one layer is very confident but another is very not,
     * there's a cross-representation disagreement */
    if (has_high_confidence && has_low_confidence) {
        r.passed = false;
        r.confidence = 0.5f;
        snprintf(r.detail, sizeof(r.detail),
                 "cross-representation conflict: layers disagree");
    } else {
        r.passed = true;
        /* Confidence = minimum across all preceding layers */
        r.confidence = 1.0f;
        for (int i = 0; i < count; i++) {
            if (layers[i].confidence < r.confidence)
                r.confidence = layers[i].confidence;
        }
        snprintf(r.detail, sizeof(r.detail),
                 "all layers consistent, min confidence %.3f", r.confidence);
    }

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Layer 8: Work Verification — Laziness Detection
 *
 * Compare what the VM observed vs what was required.
 * The agent didn't self-report. The VM saw everything.
 * ============================================ */

tardy_layer_result_t tardy_verify_work(
    const tardy_work_log_t *log,
    const tardy_work_spec_t *spec,
    const tardy_semantics_t *sem)
{
    uint64_t start = now_ns();
    tardy_layer_result_t r = {0};
    r.layer = TARDY_LAYER_WORK_VERIFY;

    if (!log || !spec) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail), "no work log or spec");
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Check minimum observed operations */
    int total_ops = log->ontology_queries + log->context_reads +
                    log->agents_spawned;

    if (total_ops < sem->laziness.min_observed_operations) {
        r.passed = false;
        r.confidence = 0.0f;
        snprintf(r.detail, sizeof(r.detail),
                 "NO WORK: %d total operations, need %d — laziness detected",
                 total_ops, sem->laziness.min_observed_operations);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Check ontology queries */
    if (log->ontology_queries < spec->min_ontology_queries) {
        r.passed = false;
        r.confidence = (float)log->ontology_queries /
                       (float)spec->min_ontology_queries;
        snprintf(r.detail, sizeof(r.detail),
                 "SHALLOW: %d ontology queries, need %d",
                 log->ontology_queries, spec->min_ontology_queries);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* Check context reads */
    if (log->context_reads < spec->min_context_reads) {
        r.passed = false;
        r.confidence = (float)log->context_reads /
                       (float)spec->min_context_reads;
        snprintf(r.detail, sizeof(r.detail),
                 "SHALLOW: %d context reads, need %d",
                 log->context_reads, spec->min_context_reads);
        r.compute_ns = now_ns() - start;
        return r;
    }

    /* All checks passed */
    r.passed = true;
    r.confidence = 1.0f;
    snprintf(r.detail, sizeof(r.detail),
             "genuine work: %d queries, %d reads, %d spawns",
             log->ontology_queries, log->context_reads, log->agents_spawned);

    r.compute_ns = now_ns() - start;
    return r;
}

/* ============================================
 * Truth Strength Computation
 * ============================================ */

tardy_truth_strength_t tardy_compute_truth_strength(
    const tardy_pipeline_result_t *result)
{
    if (!result->passed)
        return TARDY_TRUTH_REFUTED;

    float c = result->confidence;

    if (c >= 0.99f) return TARDY_TRUTH_PROVEN;
    if (c >= 0.85f) return TARDY_TRUTH_EVIDENCED;
    if (c >= 0.60f) return TARDY_TRUTH_ATTESTED;
    if (c >= 0.30f) return TARDY_TRUTH_HYPOTHETICAL;
    return TARDY_TRUTH_CONTESTED;
}

/* ============================================
 * Full Pipeline
 * ============================================ */

tardy_pipeline_result_t tardy_pipeline_verify(
    const char *claim,
    int claim_len,
    const tardy_decomposition_t *decompositions,
    int decomposition_count,
    const tardy_grounding_t *grounding,
    const tardy_consistency_t *consistency,
    const tardy_work_log_t *work_log,
    const tardy_work_spec_t *work_spec,
    const tardy_semantics_t *semantics)
{
    (void)claim;
    (void)claim_len;

    tardy_pipeline_result_t result = {0};
    result.passed = true;
    result.confidence = 1.0f;

    int layer_idx = 0;

    /* Layer 1: Decomposition */
    if (semantics->pipeline.layer_ontology_grounding) {
        result.layers[layer_idx] = tardy_verify_decomposition(
            decompositions, decomposition_count, semantics);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_DECOMPOSE;
        }
        if (result.layers[layer_idx].confidence < result.confidence)
            result.confidence = result.layers[layer_idx].confidence;
        layer_idx++;
    }

    /* Layer 2: Grounding */
    if (semantics->pipeline.layer_ontology_grounding) {
        result.layers[layer_idx] = tardy_verify_grounding(
            grounding, semantics);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_GROUNDING;
        }
        if (result.layers[layer_idx].confidence < result.confidence)
            result.confidence = result.layers[layer_idx].confidence;
        layer_idx++;
    }

    /* Layer 3: Consistency */
    if (semantics->pipeline.layer_consistency_check) {
        result.layers[layer_idx] = tardy_verify_consistency(
            consistency, semantics);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_CONSISTENCY;
        }
        if (result.layers[layer_idx].confidence < result.confidence)
            result.confidence = result.layers[layer_idx].confidence;
        layer_idx++;
    }

    /* Layer 4: Probabilistic */
    if (semantics->pipeline.layer_probabilistic_scoring) {
        result.layers[layer_idx] = tardy_verify_probabilistic(
            grounding, semantics);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_PROBABILISTIC;
        }
        if (result.layers[layer_idx].confidence < result.confidence)
            result.confidence = result.layers[layer_idx].confidence;
        layer_idx++;
    }

    /* Layer 5: Protocol */
    if (semantics->pipeline.layer_protocol_check) {
        result.layers[layer_idx] = tardy_verify_protocol();
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_PROTOCOL;
        }
        layer_idx++;
    }

    /* Layer 6: Formal certification (opt-in) */
    if (semantics->pipeline.layer_formal_certification) {
        result.layers[layer_idx] = tardy_verify_certification();
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_CERTIFICATION;
        }
        layer_idx++;
    }

    /* Layer 7: Cross-representation (opt-in) */
    if (semantics->pipeline.layer_cross_representation) {
        result.layers[layer_idx] = tardy_verify_cross_representation(
            result.layers, layer_idx);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_CROSS_REP;
        }
        if (result.layers[layer_idx].confidence < result.confidence)
            result.confidence = result.layers[layer_idx].confidence;
        layer_idx++;
    }

    /* Layer 8: Work verification */
    if (semantics->pipeline.layer_work_verification) {
        result.layers[layer_idx] = tardy_verify_work(
            work_log, work_spec, semantics);
        if (!result.layers[layer_idx].passed && result.passed) {
            result.passed = false;
            result.failed_at = TARDY_LAYER_WORK_VERIFY;
        }
        layer_idx++;
    }

    /* Count passed/failed */
    for (int i = 0; i < layer_idx; i++) {
        if (result.layers[i].passed)
            result.layers_passed++;
        else
            result.layers_failed++;
    }

    /* Compute truth strength */
    result.strength = tardy_compute_truth_strength(&result);

    return result;
}
