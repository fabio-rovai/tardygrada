/*
 * Tardygrada -- Baseline Hallucination Detectors
 *
 * NOT real SelfCheckGPT or FActScore. These are lightweight, deterministic
 * lexical heuristics intended only as cheap reference points inside this
 * repo's benchmarks. They share the *family* of techniques (consistency-
 * across-claims, per-claim verifiability signals) but DO NOT reproduce the
 * published methods:
 *   - Real SelfCheckGPT samples N LLM generations and uses NLI/BERTScore.
 *   - Real FActScore decomposes responses into atomic facts and grounds
 *     each one against a knowledge base (e.g. Wikipedia).
 *
 * What this file actually implements:
 *   1. lexical_baseline -- pairwise lexical-contradiction heuristic over a
 *      set of claims (hand-coded negation/antonym pairs).
 *   2. verifiability_signals -- per-claim heuristic looking for numbers,
 *      named entities, citations vs vague hedging.
 *
 * Headline benchmark tables that compare Tardygrada to "SelfCheckGPT" or
 * "FActScore" are NOT comparing against the published systems -- they are
 * comparing against the heuristics here. Treat any such delta as a
 * lexical-baseline delta, not a state-of-the-art delta.
 */

#ifndef BASELINES_H
#define BASELINES_H

#include <stdbool.h>

/* ============================================
 * Lexical-baseline (a.k.a. "consistency-style") detector
 * ============================================
 *
 * Compares claims WITHIN a set for lexical contradictions
 * (negation pairs, conflicting numbers, antonyms). Pairwise within the
 * given set; no LLM calls; no NLI model.
 */

typedef struct {
    int   total_claims;
    int   flagged_hallucinated;
    float consistency_score;  /* 0.0 = all inconsistent, 1.0 = all consistent */
} lexical_baseline_result_t;

/* Check if claims are consistent with each other (pairwise similarity) */
lexical_baseline_result_t lexical_baseline_evaluate(const char **claims, int claim_count);

/* Backwards-compatibility shims -- old names still work but should not be
 * used in new code or in any external comparison label. */
typedef lexical_baseline_result_t selfcheck_result_t;
static inline lexical_baseline_result_t
selfcheck_evaluate(const char **claims, int claim_count)
{
    return lexical_baseline_evaluate(claims, claim_count);
}


/* ============================================
 * FActScore-style baseline
 * ============================================
 *
 * Real FActScore: decompose response into atomic facts, check each
 * fact against a knowledge base (Wikipedia). Score = fraction supported.
 *
 * Our approximation: check each claim individually for verifiability
 * signals (specific numbers, named entities, citations vs vague language).
 * Does NOT check compositional consistency -- that's the whole point.
 */

typedef struct {
    int   total_facts;
    int   verifiable_facts;
    int   unverifiable_facts;
    float factscore;  /* verifiable / total */
} factscore_result_t;

/* Check individual claims for verifiability */
factscore_result_t factscore_evaluate(const char **claims, int claim_count);

#endif /* BASELINES_H */
