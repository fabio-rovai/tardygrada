/*
 * Tardygrada -- LLM-Assisted Decomposition Simulator
 *
 * For claims where the pattern-matching decomposer extracts triples that
 * don't reveal contradictions, this layer simulates what an LLM would do:
 * infer IMPLICIT relationships from combinations of claims.
 *
 * Example:
 *   Input: "The study has p=0.04. The researchers applied 30 tests."
 *   Pattern decomposer: (study, has_p_value, 0.04), (researchers, applied, 30_tests)
 *   LLM decomposer adds: (bonferroni_threshold, equals, 0.00167),
 *                         (study_p_value, exceeds, bonferroni_threshold)
 *   NOW the OWL reasoner sees the contradiction.
 *
 * In a real deployment, TARDY_LLM_DECOMPOSE=1 calls an actual LLM.
 * For the benchmark, this deterministic simulator is more reproducible.
 */

#ifndef TARDY_LLM_DECOMPOSE_H
#define TARDY_LLM_DECOMPOSE_H

#include "pipeline.h"
#include <stdbool.h>

/* Maximum inferred triples from LLM decomposition */
#define TARDY_LLM_MAX_INFERRED 32

typedef struct {
    tardy_triple_t inferred_triples[TARDY_LLM_MAX_INFERRED];
    int            inferred_count;
    bool           found_implicit_contradiction;
    char           reasoning[512];
} tardy_llm_decomposition_t;

/*
 * Simulate LLM decomposition -- extracts implicit relationships from
 * combinations of claims that the pattern decomposer missed.
 *
 * @param claims       Array of claim strings
 * @param claim_count  Number of claims
 * @param basic_decomp Output from the pattern decomposer (for context)
 * @return             LLM decomposition result with inferred triples
 */
tardy_llm_decomposition_t tardy_llm_decompose(
    const char **claims, int claim_count,
    const tardy_decomposition_t *basic_decomp
);

#endif /* TARDY_LLM_DECOMPOSE_H */
