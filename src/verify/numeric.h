/*
 * Tardygrada -- Numeric Contradiction Detector
 *
 * Lightweight numeric extractor and consistency checker.
 * Catches contradictions that require NUMERIC REASONING:
 *   - Rate/capacity violations (queueing theory saturation)
 *   - Accuracy below majority baseline
 *   - Temperature/physical mismatches
 *   - Statistical violations (Bonferroni)
 *   - Security bit mismatches
 *   - Impossible throughput/review speeds
 *
 * Pure C. No LLM. Deterministic.
 */

#ifndef TARDY_NUMERIC_H
#define TARDY_NUMERIC_H

#include <stdbool.h>

#define TARDY_NUMERIC_MAX_VALUES 16
#define TARDY_NUMERIC_MAX_LABEL  64
#define TARDY_NUMERIC_MAX_EXPL   256

typedef struct {
    double values[TARDY_NUMERIC_MAX_VALUES];
    char   labels[TARDY_NUMERIC_MAX_VALUES][TARDY_NUMERIC_MAX_LABEL];
    int    count;
    bool   has_contradiction;
    char   explanation[TARDY_NUMERIC_MAX_EXPL];
} tardy_numeric_check_t;

/* Extract numbers from claim text */
tardy_numeric_check_t tardy_numeric_extract(const char *claim, int claim_len);

/* Check numeric consistency between multiple claims */
tardy_numeric_check_t tardy_numeric_verify(const char **claims, int claim_count);

#endif /* TARDY_NUMERIC_H */
