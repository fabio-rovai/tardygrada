/*
 * Layer 4 (probabilistic scoring) aggregation tests.
 *
 * A claim is the CONJUNCTION of its triples: it holds only if every triple
 * holds. The arithmetic mean is the wrong aggregator for a conjunction,
 * because it lets one weak triple hide behind many strong ones. These tests
 * pin the required behaviour.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../src/verify/pipeline.h"

/* Layer 4 needs no crypto. Stub the two symbols pipeline.c pulls in for the
 * work-verification layer so this unit test links without monocypher. */
void tardy_sha256(const void *data, size_t len, tardy_hash_t *out)
{
    (void)data; (void)len;
    if (out) memset(out, 0, sizeof(*out));
}

bool tardy_hash_eq(const tardy_hash_t *a, const tardy_hash_t *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

static int failures = 0;

static void check(const char *name, bool cond, const char *detail)
{
    if (cond) {
        printf("  PASS  %s\n", name);
    } else {
        printf("  FAIL  %s  (%s)\n", name, detail);
        failures++;
    }
}

/* Build a grounding result of `n` GROUNDED triples with the given
 * per-triple confidences. */
static void make_grounding(tardy_grounding_t *g, const float *confs, int n)
{
    memset(g, 0, sizeof(*g));
    g->count = n;
    g->grounded = n;
    for (int i = 0; i < n; i++) {
        g->results[i].status = TARDY_KNOWLEDGE_GROUNDED;
        g->results[i].confidence = confs[i];
        g->results[i].evidence_count = 1;
    }
}

int main(void)
{
    tardy_semantics_t sem = TARDY_DEFAULT_SEMANTICS;  /* min_confidence 0.85 */
    tardy_grounding_t g;
    tardy_layer_result_t r;

    printf("Layer 4 aggregation tests (threshold %.2f)\n",
           (double)sem.truth.min_confidence);

    /* 1. THE BUG: one weak conjunct hidden among strong ones.
     *    Arithmetic mean = (9*0.95 + 0.20)/10 = 0.875 -> passes 0.85.
     *    A conjunction with an unsupported term must NOT pass. */
    {
        float confs[10] = {0.95f, 0.95f, 0.95f, 0.95f, 0.95f,
                           0.95f, 0.95f, 0.95f, 0.95f, 0.20f};
        make_grounding(&g, confs, 10);
        r = tardy_verify_probabilistic(&g, &sem);
        check("weak conjunct hidden among strong ones is rejected",
              !r.passed, r.detail);
    }

    /* 2. All terms strong -> passes. */
    {
        float confs[10];
        for (int i = 0; i < 10; i++) confs[i] = 0.95f;
        make_grounding(&g, confs, 10);
        r = tardy_verify_probabilistic(&g, &sem);
        check("uniformly strong evidence passes", r.passed, r.detail);
    }

    /* 3. Uniformly mediocre -> fails (both aggregators agree here). */
    {
        float confs[10];
        for (int i = 0; i < 10; i++) confs[i] = 0.80f;
        make_grounding(&g, confs, 10);
        r = tardy_verify_probabilistic(&g, &sem);
        check("uniformly mediocre evidence fails", !r.passed, r.detail);
    }

    /* 4. Score must never exceed the arithmetic mean: the aggregator has to
     *    be conservative, not optimistic. */
    {
        float confs[4] = {0.99f, 0.99f, 0.99f, 0.70f};
        make_grounding(&g, confs, 4);
        r = tardy_verify_probabilistic(&g, &sem);
        float arith = (0.99f + 0.99f + 0.99f + 0.70f) / 4.0f;
        check("reported score is no higher than the arithmetic mean",
              r.confidence <= arith + 1e-6f, r.detail);
    }

    /* 5. A single grounded triple below the floor fails even when it is the
     *    only evidence and would otherwise average to itself. */
    {
        float confs[1] = {0.30f};
        make_grounding(&g, confs, 1);
        r = tardy_verify_probabilistic(&g, &sem);
        check("lone weak triple fails", !r.passed, r.detail);
    }

    /* 6. UNKNOWN triples are excluded from scoring, not counted as zero. */
    {
        float confs[2] = {0.95f, 0.95f};
        make_grounding(&g, confs, 2);
        g.count = 3;
        g.results[2].status = TARDY_KNOWLEDGE_UNKNOWN;
        g.results[2].confidence = 0.0f;
        g.unknown = 1;
        r = tardy_verify_probabilistic(&g, &sem);
        check("unknown triples do not drag the score to zero",
              r.passed, r.detail);
    }

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
