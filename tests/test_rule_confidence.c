/*
 * Rule-confidence update tests.
 *
 * The old update was `confidence *= 1.05` capped at 1.0. Seeing the same
 * pattern fifteen times drove a rule to absolute certainty, and no amount of
 * contradicting evidence could ever lower it. For a system whose pitch is
 * "cannot be fooled by confident repetition", that was the wrong maths.
 */

#include <stdio.h>
#include <string.h>

#include "../src/ontology/inference.h"
#include "../src/vm/vm.h"

/* The rule-confidence update needs no VM. Stub the one symbol the healing
 * path pulls in so this unit test links standalone. */
tardy_agent_t *tardy_vm_find(tardy_vm_t *vm, tardy_uuid_t id)
{
    (void)vm; (void)id;
    return NULL;
}

static int failures = 0;

static void check(const char *name, bool cond, float got)
{
    if (cond) {
        printf("  PASS  %s (%.4f)\n", name, (double)got);
    } else {
        printf("  FAIL  %s (got %.4f)\n", name, (double)got);
        failures++;
    }
}

int main(void)
{
    printf("Rule confidence update tests\n");

    /* 1. No observations: the seeded prior is returned unchanged. */
    {
        tardy_rule_t rule;
        memset(&rule, 0, sizeof(rule));
        rule.confidence = 0.95f;
        tardy_rule_update(&rule, 0, 0);
        check("seeded prior survives an empty update",
              rule.confidence > 0.949f && rule.confidence < 0.951f,
              rule.confidence);
    }

    /* 2. THE BUG: repetition alone must never reach certainty. */
    {
        tardy_rule_t rule;
        memset(&rule, 0, sizeof(rule));
        rule.confidence = 0.60f;   /* a newly mined rule */
        for (int i = 0; i < 1000; i++) tardy_rule_update(&rule, 1, 0);
        check("1000 repetitions stay below the ceiling",
              rule.confidence <= TARDY_RULE_CONF_CEIL, rule.confidence);
        check("1000 repetitions never reach 1.0",
              rule.confidence < 1.0f, rule.confidence);
    }

    /* 3. Support raises confidence, with diminishing returns. */
    {
        tardy_rule_t rule;
        memset(&rule, 0, sizeof(rule));
        rule.confidence = 0.60f;
        tardy_rule_update(&rule, 1, 0);
        float after_one = rule.confidence;
        tardy_rule_update(&rule, 1, 0);
        float after_two = rule.confidence;
        check("support raises confidence", after_one > 0.60f, after_one);
        check("second observation moves it less than the first",
              (after_two - after_one) < (after_one - 0.60f), after_two);
    }

    /* 4. Contradicting evidence lowers confidence. The old code had no path
     *    for this at all. */
    {
        tardy_rule_t rule;
        memset(&rule, 0, sizeof(rule));
        rule.confidence = 0.90f;
        for (int i = 0; i < 5; i++) tardy_rule_update(&rule, 1, 0);
        float supported = rule.confidence;
        for (int i = 0; i < 20; i++) tardy_rule_update(&rule, 0, 1);
        check("contradicting observations lower confidence",
              rule.confidence < supported, rule.confidence);
        check("sustained contradiction drops it below the prior",
              rule.confidence < 0.90f, rule.confidence);
    }

    /* 5. Confidence stays in [0, 1] under contradiction pressure. */
    {
        tardy_rule_t rule;
        memset(&rule, 0, sizeof(rule));
        rule.confidence = 0.80f;
        for (int i = 0; i < 10000; i++) tardy_rule_update(&rule, 0, 1);
        check("stays non-negative under heavy contradiction",
              rule.confidence >= 0.0f && rule.confidence <= 1.0f,
              rule.confidence);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
