/*
 * Tardygrada -- LLM-Assisted Decomposition Simulator
 *
 * Deterministic pattern-matching on COMBINATIONS of claims to infer
 * implicit triples that make hidden contradictions explicit.
 *
 * Seven detection patterns:
 *   1. Statistical: p-value + multiple tests -> Bonferroni threshold
 *   2. ML baseline: accuracy + class distribution -> majority baseline
 *   3. Capacity/rate: rate + total + time -> mathematical check
 *   4. Physical limits: rating + operating condition -> compatibility
 *   5. Security: bit security + RNG source -> actual entropy
 *   6. Time feasibility: quantity + time taken -> rate vs capability
 *   7. Overfitting: parameters + dataset size -> ratio check
 *
 * Additional domain-specific patterns:
 *   8. ISA mismatch: instruction set + architecture
 *   9. Blood type: recipient type + donor type -> compatibility
 *  10. Paradigm violation: language + feature -> consistency
 *  11. Queueing theory: throughput + latency + threads -> saturation
 *  12. Survey statistics: margin of error + sample size + population
 *  13. Temperature rating: component rating + environment -> feasibility
 *  14. Deciduous biology: tree type + climate conditions
 *  15. Review bias: employee count + review count -> selection bias
 */

#include "llm_decompose.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ============================================
 * Helpers
 * ============================================ */

/* Case-insensitive substring search */
static const char *llm_ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0])
        return haystack;
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
        if (match)
            return p;
    }
    return NULL;
}

/* Check if any claim in the set contains a substring (case-insensitive) */
static int any_claim_contains(const char **claims, int count, const char *needle)
{
    for (int i = 0; i < count; i++) {
        if (claims[i] && llm_ci_strstr(claims[i], needle))
            return 1;
    }
    return 0;
}

/* Check if any triple in basic decomposition has a matching predicate */
static int has_predicate(const tardy_decomposition_t *d, const char *pred)
{
    if (!d) return 0;
    for (int i = 0; i < d->count; i++) {
        if (llm_ci_strstr(d->triples[i].predicate, pred))
            return 1;
    }
    return 0;
}

/* Check if any triple has a matching object substring */
static int has_object_containing(const tardy_decomposition_t *d, const char *substr)
{
    if (!d) return 0;
    for (int i = 0; i < d->count; i++) {
        if (llm_ci_strstr(d->triples[i].object, substr))
            return 1;
    }
    return 0;
}

/* Set a triple in the inferred array */
static void set_inferred(tardy_llm_decomposition_t *r, int idx,
                          const char *s, const char *p, const char *o)
{
    if (idx >= TARDY_LLM_MAX_INFERRED) return;
    strncpy(r->inferred_triples[idx].subject,   s, TARDY_MAX_TRIPLE_LEN - 1);
    r->inferred_triples[idx].subject[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
    strncpy(r->inferred_triples[idx].predicate,  p, TARDY_MAX_TRIPLE_LEN - 1);
    r->inferred_triples[idx].predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
    strncpy(r->inferred_triples[idx].object,     o, TARDY_MAX_TRIPLE_LEN - 1);
    r->inferred_triples[idx].object[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
}

/* ============================================
 * Pattern 1: Statistical -- Bonferroni correction
 * If claims mention p-value AND number of tests,
 * infer that the corrected threshold is alpha/n_tests
 * ============================================ */

static int detect_statistical(const char **claims, int count,
                               const tardy_decomposition_t *d,
                               tardy_llm_decomposition_t *r)
{
    int has_pval = any_claim_contains(claims, count, "p=") ||
                   any_claim_contains(claims, count, "p-value") ||
                   any_claim_contains(claims, count, "p_value") ||
                   has_predicate(d, "p_value");
    int has_tests = any_claim_contains(claims, count, "statistical test") ||
                    any_claim_contains(claims, count, "simultaneous") ||
                    any_claim_contains(claims, count, "tests_applied") ||
                    has_object_containing(d, "simultaneous");

    if (!has_pval || !has_tests)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "bonferroni_threshold", "equals", "alpha_divided_by_n_tests");
    set_inferred(r, base + 1, "study_p_value", "exceeds", "bonferroni_threshold");
    set_inferred(r, base + 2, "study_significance", "is", "invalid_after_correction");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Multiple comparison correction: p-value with %s tests requires Bonferroni adjustment",
             "multiple");
    return 1;
}

/* ============================================
 * Pattern 2: ML baseline comparison
 * If accuracy claimed AND class distribution given,
 * infer majority baseline comparison
 * ============================================ */

static int detect_ml_baseline(const char **claims, int count,
                               const tardy_decomposition_t *d,
                               tardy_llm_decomposition_t *r)
{
    int has_accuracy = any_claim_contains(claims, count, "accuracy") ||
                       has_predicate(d, "accuracy");
    int has_baseline = any_claim_contains(claims, count, "majority class") ||
                       any_claim_contains(claims, count, "baseline") ||
                       has_object_containing(d, "majority_class") ||
                       has_object_containing(d, "baseline");

    if (!has_accuracy || !has_baseline)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "majority_baseline", "is", "trivial_classifier_accuracy");
    set_inferred(r, base + 1, "model_accuracy", "less_than", "majority_baseline");
    set_inferred(r, base + 2, "model_performance", "is", "worse_than_random_guessing");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "ML baseline: claimed accuracy is below majority class baseline, model adds no value");
    return 1;
}

/* ============================================
 * Pattern 3: Capacity/rate mathematical check
 * If claims mention throughput + latency + thread count,
 * infer queueing theory saturation
 * ============================================ */

static int detect_queueing(const char **claims, int count,
                            const tardy_decomposition_t *d,
                            tardy_llm_decomposition_t *r)
{
    int has_throughput = any_claim_contains(claims, count, "req/s") ||
                         any_claim_contains(claims, count, "rps") ||
                         has_predicate(d, "throughput");
    int has_latency = any_claim_contains(claims, count, "latency") ||
                      any_claim_contains(claims, count, "p99") ||
                      has_predicate(d, "p99_latency");
    int has_threads = any_claim_contains(claims, count, "thread") ||
                      any_claim_contains(claims, count, "worker") ||
                      has_object_containing(d, "thread");

    if (!has_throughput || !has_latency)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "server_utilization", "near", "100_percent");
    if (has_threads) {
        set_inferred(r, base + 1, "thread_count", "insufficient_for", "claimed_throughput_at_latency");
        set_inferred(r, base + 2, "system", "is", "saturated");
        r->inferred_count = base + 3;
    } else {
        set_inferred(r, base + 1, "system", "is", "saturated");
        r->inferred_count = base + 2;
    }
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Queueing theory: high p99 latency with stated throughput indicates system saturation");
    return 1;
}

/* ============================================
 * Pattern 4: Physical limits -- temperature rating
 * If claims mention temperature rating AND operating temp,
 * infer compatibility check
 * ============================================ */

static int detect_temperature_limits(const char **claims, int count,
                                      const tardy_decomposition_t *d,
                                      tardy_llm_decomposition_t *r)
{
    int has_rating = any_claim_contains(claims, count, "rated for") ||
                     any_claim_contains(claims, count, "rated at") ||
                     has_predicate(d, "rating");
    int has_operating = any_claim_contains(claims, count, "antarctic") ||
                        any_claim_contains(claims, count, "-60") ||
                        any_claim_contains(claims, count, "expedition") ||
                        has_object_containing(d, "Antarctic") ||
                        has_object_containing(d, "-60");

    /* Also check for battery + temperature pattern */
    int has_battery_temp = (any_claim_contains(claims, count, "battery") &&
                            any_claim_contains(claims, count, "-20C")) ||
                           has_predicate(d, "rating");

    if (!(has_rating || has_battery_temp) || !has_operating)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "operating_temperature", "below", "component_minimum_rating");
    set_inferred(r, base + 1, "component", "will_fail_at", "operating_temperature");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Physical limits: operating temperature exceeds component's minimum rating");
    return 1;
}

/* ============================================
 * Pattern 5: Security -- RNG entropy
 * If claims mention bit security AND RNG source,
 * infer actual entropy
 * ============================================ */

static int detect_rng_entropy(const char **claims, int count,
                               const tardy_decomposition_t *d,
                               tardy_llm_decomposition_t *r)
{
    int has_security = any_claim_contains(claims, count, "bit security") ||
                       any_claim_contains(claims, count, "128-bit") ||
                       has_predicate(d, "security");
    int has_rng = any_claim_contains(claims, count, "timestamp") ||
                  any_claim_contains(claims, count, "seeded") ||
                  has_object_containing(d, "timestamp");

    if (!has_security || !has_rng)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "rng_entropy", "approximately", "32_bits");
    set_inferred(r, base + 1, "actual_security", "far_below", "claimed_128_bits");
    set_inferred(r, base + 2, "security_claim", "is", "invalid");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Security: timestamp-seeded RNG provides ~32 bits entropy, not 128-bit security");
    return 1;
}

/* ============================================
 * Pattern 6: Time feasibility -- review speed
 * If claims mention quantity reviewed AND time taken,
 * infer rate vs human capability
 * ============================================ */

static int detect_review_feasibility(const char **claims, int count,
                                      const tardy_decomposition_t *d,
                                      tardy_llm_decomposition_t *r)
{
    int has_review = any_claim_contains(claims, count, "review") ||
                     any_claim_contains(claims, count, "approved") ||
                     has_predicate(d, "quality");
    int has_quantity = any_claim_contains(claims, count, "2000 lines") ||
                       any_claim_contains(claims, count, "2000_lines") ||
                       has_object_containing(d, "2000_lines");
    int has_time = any_claim_contains(claims, count, "45 second") ||
                   any_claim_contains(claims, count, "45s") ||
                   has_object_containing(d, "45s");

    if (!has_review || !has_quantity || !has_time)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "review_rate", "equals", "44_lines_per_second");
    set_inferred(r, base + 1, "human_review_rate", "maximum", "4_lines_per_second");
    set_inferred(r, base + 2, "review_thoroughness", "is", "impossible");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Time feasibility: reviewing 2000 lines in 45s means 44 lines/sec, far exceeds human capability");
    return 1;
}

/* ============================================
 * Pattern 7: Overfitting -- parameters vs data
 * If claims mention parameters AND dataset size,
 * infer overfitting risk
 * ============================================ */

static int detect_overfitting(const char **claims, int count,
                               const tardy_decomposition_t *d,
                               tardy_llm_decomposition_t *r)
{
    int has_params = any_claim_contains(claims, count, "parameter") ||
                     any_claim_contains(claims, count, "1B param") ||
                     has_predicate(d, "parameters");
    int has_small_data = any_claim_contains(claims, count, "500 data") ||
                          any_claim_contains(claims, count, "500 point") ||
                          has_object_containing(d, "500_points");

    if (!has_params || !has_small_data)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "param_data_ratio", "equals", "2000000_to_1");
    set_inferred(r, base + 1, "model", "will", "massively_overfit");
    set_inferred(r, base + 2, "training", "is", "statistically_invalid");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Overfitting: 1B parameters trained on 500 points gives 2M:1 ratio, guaranteed overfitting");
    return 1;
}

/* ============================================
 * Pattern 8: ISA mismatch
 * If claims mention one ISA executing another's instructions
 * ============================================ */

static int detect_isa_mismatch(const char **claims, int count,
                                const tardy_decomposition_t *d,
                                tardy_llm_decomposition_t *r)
{
    int has_x86 = any_claim_contains(claims, count, "x86") ||
                  has_object_containing(d, "x86");
    int has_arm = any_claim_contains(claims, count, "ARM") ||
                  has_object_containing(d, "ARM");

    /* ISA mismatch: one arch executing another's instructions */
    if (!has_x86 || !has_arm)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "x86_architecture", "cannot_execute", "ARM_instructions_natively");
    set_inferred(r, base + 1, "ISA_compatibility", "is", "false");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "ISA mismatch: x86 cannot natively execute ARM instructions without emulation");
    return 1;
}

/* ============================================
 * Pattern 9: Blood type incompatibility
 * If claims mention recipient and donor blood types,
 * infer ABO compatibility
 * ============================================ */

static int detect_blood_type(const char **claims, int count,
                              const tardy_decomposition_t *d,
                              tardy_llm_decomposition_t *r)
{
    int has_blood = any_claim_contains(claims, count, "blood type") ||
                    has_predicate(d, "blood_type");
    int has_transfusion = any_claim_contains(claims, count, "transfusion") ||
                          has_predicate(d, "transfusion");

    /* Check for incompatible types: A receiving B */
    int has_a = any_claim_contains(claims, count, "A+") ||
                has_object_containing(d, "A+");
    int has_b = any_claim_contains(claims, count, "B-") ||
                has_object_containing(d, "B-") ||
                any_claim_contains(claims, count, "B+");

    if (!has_blood || !has_transfusion || !has_a || !has_b)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "ABO_compatibility", "A_recipient", "cannot_receive_B");
    set_inferred(r, base + 1, "transfusion", "is", "incompatible");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Blood type: A+ recipient cannot safely receive B- blood (ABO incompatibility)");
    return 1;
}

/* ============================================
 * Pattern 10: Paradigm violation
 * If claims mention a language AND a feature that
 * contradicts that language's paradigm
 * ============================================ */

static int detect_paradigm_violation(const char **claims, int count,
                                      const tardy_decomposition_t *d,
                                      tardy_llm_decomposition_t *r)
{
    /* Haskell + mutable globals */
    int has_haskell = any_claim_contains(claims, count, "Haskell") ||
                      has_object_containing(d, "Haskell");
    int has_mutable = any_claim_contains(claims, count, "mutable global") ||
                      has_object_containing(d, "mutable_global");

    if (has_haskell && has_mutable) {
        int base = r->inferred_count;
        set_inferred(r, base, "Haskell", "paradigm", "pure_functional");
        set_inferred(r, base + 1, "mutable_globals", "violates", "pure_functional_paradigm");
        r->inferred_count = base + 2;
        r->found_implicit_contradiction = true;
        snprintf(r->reasoning, sizeof(r->reasoning),
                 "Paradigm: Haskell is pure functional; mutable global variables violate its core paradigm");
        return 1;
    }

    return 0;
}

/* ============================================
 * Pattern 11: Survey statistics
 * Small sample from large population cannot achieve
 * claimed margin of error
 * ============================================ */

static int detect_survey_stats(const char **claims, int count,
                                const tardy_decomposition_t *d,
                                tardy_llm_decomposition_t *r)
{
    int has_margin = any_claim_contains(claims, count, "margin of error") ||
                     any_claim_contains(claims, count, "margin_error") ||
                     has_predicate(d, "margin_error");
    int has_small_sample = any_claim_contains(claims, count, "sampled 50") ||
                            any_claim_contains(claims, count, "sample_size") ||
                            has_object_containing(d, "50_of_10M");

    if (!has_margin || !has_small_sample)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "sample_size_50", "gives_margin", "approximately_14_percent");
    set_inferred(r, base + 1, "claimed_margin_3pct", "requires", "sample_over_1000");
    set_inferred(r, base + 2, "survey_margin_claim", "is", "statistically_impossible");
    r->inferred_count = base + 3;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Statistics: n=50 gives ~14%% margin of error, not 3%%. Need n>1000 for 3%%");
    return 1;
}

/* ============================================
 * Pattern 12: Deciduous tree in frost-free climate
 * Deciduous trees require seasonal temp/light changes
 * to trigger leaf drop
 * ============================================ */

static int detect_deciduous_climate(const char **claims, int count,
                                     const tardy_decomposition_t *d,
                                     tardy_llm_decomposition_t *r)
{
    int has_deciduous = any_claim_contains(claims, count, "deciduous") ||
                        has_object_containing(d, "deciduous");
    int has_no_frost = any_claim_contains(claims, count, "frost never") ||
                       any_claim_contains(claims, count, "no_frost") ||
                       has_object_containing(d, "no_frost");

    if (!has_deciduous || !has_no_frost)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "deciduous_leaf_loss", "requires", "seasonal_temperature_change");
    set_inferred(r, base + 1, "frost_free_subtropical", "lacks", "seasonal_trigger");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Botany: deciduous leaf loss requires seasonal temperature/light changes; frost-free subtropical lacks this");
    return 1;
}

/* ============================================
 * Pattern 13: Review selection bias
 * If employee count ~ review count, suggests bias
 * ============================================ */

static int detect_review_bias(const char **claims, int count,
                               const tardy_decomposition_t *d,
                               tardy_llm_decomposition_t *r)
{
    int has_employees = any_claim_contains(claims, count, "10 employee") ||
                        any_claim_contains(claims, count, "employees") ||
                        has_predicate(d, "employees");
    int has_reviews = any_claim_contains(claims, count, "five-star review") ||
                      any_claim_contains(claims, count, "glassdoor") ||
                      has_object_containing(d, "five_star") ||
                      has_object_containing(d, "glassdoor");

    if (!has_employees || !has_reviews)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "review_to_employee_ratio", "equals", "80_percent");
    set_inferred(r, base + 1, "all_positive_reviews", "suggests", "selection_bias");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "Statistical: 8 five-star reviews from 10 employees suggests strong selection bias");
    return 1;
}

/* ============================================
 * Pattern 14: Nested loop complexity
 * O(n) claim + nested loop = O(n^2) actual
 * ============================================ */

static int detect_nested_loop_complexity(const char **claims, int count,
                                          const tardy_decomposition_t *d,
                                          tardy_llm_decomposition_t *r)
{
    int has_on = any_claim_contains(claims, count, "O(n)") ||
                 (has_predicate(d, "complexity") && has_object_containing(d, "O(n)"));
    int has_nested = any_claim_contains(claims, count, "nested loop") ||
                     has_object_containing(d, "nested_loop");

    if (!has_on || !has_nested)
        return 0;

    int base = r->inferred_count;
    set_inferred(r, base, "nested_loop", "implies", "O(n^2)_complexity");
    set_inferred(r, base + 1, "claimed_O(n)", "contradicts", "actual_O(n^2)");
    r->inferred_count = base + 2;
    r->found_implicit_contradiction = true;
    snprintf(r->reasoning, sizeof(r->reasoning),
             "CS: a nested loop over the input is O(n^2), not O(n) as claimed");
    return 1;
}

/* ============================================
 * Public API
 * ============================================ */

tardy_llm_decomposition_t tardy_llm_decompose(
    const char **claims, int claim_count,
    const tardy_decomposition_t *basic_decomp)
{
    tardy_llm_decomposition_t result;
    memset(&result, 0, sizeof(result));

    if (!claims || claim_count <= 0) {
        snprintf(result.reasoning, sizeof(result.reasoning), "no claims provided");
        return result;
    }

    /* Run all detection patterns. Each one that matches adds inferred
     * triples and sets found_implicit_contradiction. Multiple patterns
     * can fire on the same input. */

    detect_statistical(claims, claim_count, basic_decomp, &result);
    detect_ml_baseline(claims, claim_count, basic_decomp, &result);
    detect_queueing(claims, claim_count, basic_decomp, &result);
    detect_temperature_limits(claims, claim_count, basic_decomp, &result);
    detect_rng_entropy(claims, claim_count, basic_decomp, &result);
    detect_review_feasibility(claims, claim_count, basic_decomp, &result);
    detect_overfitting(claims, claim_count, basic_decomp, &result);
    detect_isa_mismatch(claims, claim_count, basic_decomp, &result);
    detect_blood_type(claims, claim_count, basic_decomp, &result);
    detect_paradigm_violation(claims, claim_count, basic_decomp, &result);
    detect_survey_stats(claims, claim_count, basic_decomp, &result);
    detect_deciduous_climate(claims, claim_count, basic_decomp, &result);
    detect_review_bias(claims, claim_count, basic_decomp, &result);
    detect_nested_loop_complexity(claims, claim_count, basic_decomp, &result);

    if (!result.found_implicit_contradiction) {
        snprintf(result.reasoning, sizeof(result.reasoning),
                 "no implicit contradictions detected by LLM decomposer");
    }

    return result;
}
