/*
 * Tardygrada -- Baseline Hallucination Detectors
 *
 * Implements deterministic approximations of:
 *   1. SelfCheckGPT (consistency-based: pairwise claim comparison)
 *   2. FActScore   (atomic fact verification: per-claim verifiability)
 *
 * These are GENEROUS implementations -- real SelfCheckGPT needs N LLM
 * calls, and real FActScore needs a knowledge base. We give them every
 * advantage and they STILL miss compositional contradictions.
 */

#ifndef BASELINES_H
#define BASELINES_H

#include <stdbool.h>

/* ============================================
 * SelfCheckGPT-style baseline
 * ============================================
 *
 * Real SelfCheckGPT: generate N samples of a response, check
 * consistency across samples. Claims appearing in few samples
 * are likely hallucinated.
 *
 * Our approximation: compare claims WITHIN a set for lexical
 * contradictions (negation pairs, conflicting numbers, antonyms).
 * This is generous -- we compare pairwise within the given set
 * rather than requiring multiple LLM generations.
 */

typedef struct {
    int   total_claims;
    int   flagged_hallucinated;
    float consistency_score;  /* 0.0 = all inconsistent, 1.0 = all consistent */
} selfcheck_result_t;

/* Check if claims are consistent with each other (pairwise similarity) */
selfcheck_result_t selfcheck_evaluate(const char **claims, int claim_count);


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
