/*
 * Tardygrada -- Ontology Inference Engine
 *
 * Three capabilities:
 *
 * 1. Synthetic backbone: structural rules generated at startup.
 *    "If X capitalOf Y, then X located_in Y."
 *    Lets the system reason about facts it hasn't seen.
 *
 * 2. Self-healing: when a gap is found, try to infer the missing
 *    triple from existing ones via the rules.
 *
 * 3. Rule mining: learn new rules from verified claims.
 *    "Claims matching pattern P tend to verify." -> new rule.
 *
 * 4. Computational verification: if a claim contains numbers,
 *    try to verify by running the computation.
 */

#ifndef TARDY_INFERENCE_H
#define TARDY_INFERENCE_H

#include "self.h"

#define TARDY_MAX_RULES 64

/* An inference rule: if (condition) then (conclusion) */
/* Rule confidence is a Beta posterior mean over observations, seeded with the
 * rule's hand-set prior as PRIOR_STRENGTH pseudo-observations. Repetition
 * alone can never manufacture certainty: the value approaches but never
 * reaches TARDY_RULE_CONF_CEIL, and contradicting observations pull it back
 * down. */
#define TARDY_RULE_PRIOR_STRENGTH 10.0f
#define TARDY_RULE_CONF_CEIL      0.99f

typedef struct {
    char if_pred[64];    /* if triple has this predicate... */
    char then_pred[64];  /* ...infer a triple with this predicate */
    int  swap_so;        /* 1 = swap subject/object in conclusion */
    float confidence;    /* posterior confidence of the inferred triple */
    float prior;         /* seeded prior mean; 0 = not yet captured */
    int   support;       /* observations consistent with the rule */
    int   contra;        /* observations contradicting the rule */
} tardy_rule_t;

/* Recompute a rule's confidence from its observation counts. */
void tardy_rule_update(tardy_rule_t *rule, int observed_support,
                       int observed_contra);

typedef struct {
    tardy_rule_t rules[TARDY_MAX_RULES];
    int          count;
} tardy_ruleset_t;

/* Initialize with synthetic backbone rules */
void tardy_inference_init(tardy_ruleset_t *rs);

/* Try to infer missing triples from existing ontology + rules.
 * Returns number of new triples inferred. */
int tardy_inference_heal(tardy_ruleset_t *rs,
                          tardy_self_ontology_t *ont,
                          const tardy_triple_t *query, int query_count,
                          tardy_triple_t *inferred, int max_inferred);

/* Mine a new rule from a verified claim pattern.
 * Called after successful verification. */
int tardy_inference_learn(tardy_ruleset_t *rs,
                           const tardy_triple_t *triples, int count);

/* Verify a computational claim by running it.
 * Returns 1 if verified, 0 if not, -1 if not computational. */
int tardy_inference_compute(const char *claim, int len,
                             float *confidence);

#endif
