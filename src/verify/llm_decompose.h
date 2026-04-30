/*
 * Tardygrada -- Lexical Implicit-Relation Decomposer
 *
 * IMPORTANT: this file does NOT call an LLM. It is a deterministic
 * pattern-matching layer (~15 hand-coded substring patterns) that
 * augments the basic decomposer with implicit relationships when the
 * input contains specific cue terms (e.g. "Bonferroni", "ISA mismatch",
 * "blood type").
 *
 * The historical name `tardy_llm_decompose` is misleading; use the new
 * `tardy_lexical_decompose` in new code. The old name is kept as an
 * inline shim for backwards compatibility.
 *
 * A separate, opt-in LLM grounding path exists in src/mcp/server.c and
 * is gated on the environment variable TARDY_LLM_DECOMPOSE=1 -- that
 * one really does call Anthropic. This file is unrelated to it.
 *
 * Example of what this lexical layer does:
 *   Input: "The study has p=0.04. The researchers applied 30 tests."
 *   Pattern decomposer: (study, has_p_value, 0.04), (researchers, applied, 30_tests)
 *   This layer adds:    (bonferroni_threshold, equals, 0.00167),
 *                       (study_p_value, exceeds, bonferroni_threshold)
 *   The OWL reasoner can then see the contradiction.
 */

#ifndef TARDY_LLM_DECOMPOSE_H
#define TARDY_LLM_DECOMPOSE_H

#include "pipeline.h"
#include <stdbool.h>

/* Maximum inferred triples from the lexical decomposition layer */
#define TARDY_LLM_MAX_INFERRED 32
#define TARDY_LEXICAL_MAX_INFERRED TARDY_LLM_MAX_INFERRED

typedef struct {
    tardy_triple_t inferred_triples[TARDY_LLM_MAX_INFERRED];
    int            inferred_count;
    bool           found_implicit_contradiction;
    char           reasoning[512];
} tardy_lexical_decomposition_t;

/* Backwards-compatible alias for the old type name. */
typedef tardy_lexical_decomposition_t tardy_llm_decomposition_t;

/*
 * Lexical implicit-relation decomposition. Scans claims for hand-coded
 * cue patterns and emits inferred triples that surface implicit
 * relationships missed by the basic decomposer. NO LLM CALLS.
 *
 * @param claims       Array of claim strings
 * @param claim_count  Number of claims
 * @param basic_decomp Output from the pattern decomposer (for context)
 * @return             Lexical decomposition result with inferred triples
 */
tardy_lexical_decomposition_t tardy_lexical_decompose(
    const char **claims, int claim_count,
    const tardy_decomposition_t *basic_decomp
);

/* Backwards-compatible shim. Old call sites still compile and behave
 * identically; the misleading name is preserved only to avoid mass
 * source churn. New code should use tardy_lexical_decompose. */
static inline tardy_lexical_decomposition_t tardy_llm_decompose(
    const char **claims, int claim_count,
    const tardy_decomposition_t *basic_decomp
)
{
    return tardy_lexical_decompose(claims, claim_count, basic_decomp);
}

#endif /* TARDY_LLM_DECOMPOSE_H */
