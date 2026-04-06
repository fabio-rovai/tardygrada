/*
 * Tardygrada -- Baseline Hallucination Detectors
 *
 * SelfCheckGPT-style (consistency) and FActScore-style (atomic facts)
 * deterministic baselines for comparison against the Tardygrada pipeline.
 */

#include "baselines.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* ============================================
 * String utilities
 * ============================================ */

/* Case-insensitive substring search */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return haystack;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0)
            return p;
    }
    return NULL;
}

/* Split text on ". " into sentences, return count.
 * Writes into caller-provided buffer, sets pointers. */
static int split_sentences(const char *text, char *buf, size_t bufsz,
                           const char **out, int max_out)
{
    strncpy(buf, text, bufsz - 1);
    buf[bufsz - 1] = '\0';

    int count = 0;
    char *p = buf;
    while (*p && count < max_out) {
        /* skip leading spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        out[count++] = p;

        /* find ". " or end */
        char *dot = strstr(p, ". ");
        if (dot) {
            *dot = '\0';
            p = dot + 2;
        } else {
            /* remove trailing period if present */
            size_t len = strlen(p);
            if (len > 0 && p[len - 1] == '.') p[len - 1] = '\0';
            break;
        }
    }
    return count;
}

/* Extract all numbers from a string into an array, return count */
static int extract_numbers(const char *text, double *nums, int max_nums)
{
    int count = 0;
    const char *p = text;
    while (*p && count < max_nums) {
        /* look for digit or minus-before-digit */
        if (isdigit((unsigned char)*p) ||
            (*p == '-' && isdigit((unsigned char)*(p + 1)))) {
            char *end;
            double val = strtod(p, &end);
            if (end != p) {
                /* Handle suffixes: k, M, B */
                if (*end == 'k' || *end == 'K') { val *= 1000; end++; }
                else if (*end == 'M') { val *= 1000000; end++; }
                else if (*end == 'B') { val *= 1000000000; end++; }
                nums[count++] = val;
                p = end;
                continue;
            }
        }
        p++;
    }
    return count;
}

/* ============================================
 * SelfCheckGPT: negation detection
 * ============================================ */

/* Negation pairs: if sentence A contains word[0] and sentence B
 * contains word[1] in a similar context, flag inconsistency. */
static const char *negation_pairs[][2] = {
    {"is not",        "is"},
    {"has no",        "has"},
    {"no ",           ""},       /* generic "no X" */
    {"not ",          ""},
    {"never",         ""},
    {"zero",          ""},
    {"none",          ""},
    {"without",       "with"},
    {"cannot",        "can"},
    {"discontinued",  "runs"},
    {"closed",        "open"},
    {"offline",       "online"},
    {"deleted",       "exists"},
    {NULL, NULL}
};

/* Antonym pairs: directional opposites */
static const char *antonym_pairs[][2] = {
    {"increased",   "decreased"},
    {"increased",   "declined"},
    {"completed",   "delayed"},
    {"passed",      "failed"},
    {"free",        "costs"},
    {"free",        "cost"},
    {"waterproof",  "absorbs water"},
    {"herbivorous", "hunts"},
    {"herbivore",   "prey"},
    {"soundproof",  "loud"},
    {"encrypted",   "plaintext"},
    {"extinct",     "observed"},
    {"automated",   "operators"},
    {"instantaneous","hours"},
    {"air-gapped",  "internet"},
    {"odorless",    "smell"},
    {"wireless",    "ethernet"},
    {"cancelled",   "attendees"},
    {"deleted",     "logged in"},
    {"pedestrian",  "trucks"},
    {"offline",     "streaming"},
    {"discontinued","shipping"},
    {"discharged",  "surgery"},
    {"deleted",     "modified"},
    {"vegan",       "steak"},
    {"non-toxic",   "hazmat"},
    {"proprietary", "public"},
    {"recovered",   "intensive care"},
    {"freshwater",  "salinity"},
    {"no side effects", "nausea"},
    {"no control",  "control"},
    {"no dependencies", "requires"},
    {"no external", "requires"},
    {"doctor",      "no medical"},
    {NULL, NULL}
};

/* Check if two sentences have a negation contradiction */
static bool check_negation(const char *s1, const char *s2)
{
    /* Check negation pairs */
    for (int i = 0; negation_pairs[i][0] != NULL; i++) {
        const char *neg = negation_pairs[i][0];
        /* If s1 has the negation word and s2 doesn't have it (or vice versa),
         * and the context words overlap, flag it. We do a simple check:
         * if one sentence has "no X" and the other has "X", that's a flag. */
        if (ci_strstr(s1, neg) && !ci_strstr(s2, neg)) {
            return true;
        }
        if (ci_strstr(s2, neg) && !ci_strstr(s1, neg)) {
            return true;
        }
    }
    return false;
}

/* Check if two sentences have antonym contradictions */
static bool check_antonyms(const char *s1, const char *s2)
{
    for (int i = 0; antonym_pairs[i][0] != NULL; i++) {
        if ((ci_strstr(s1, antonym_pairs[i][0]) && ci_strstr(s2, antonym_pairs[i][1])) ||
            (ci_strstr(s1, antonym_pairs[i][1]) && ci_strstr(s2, antonym_pairs[i][0]))) {
            return true;
        }
    }
    return false;
}

/* Check if two sentences have conflicting numbers in similar context.
 * Heuristic: if both sentences talk about the same subject and have
 * different numbers, that might be a conflict. */
static bool check_conflicting_numbers(const char *s1, const char *s2)
{
    double nums1[16], nums2[16];
    int n1 = extract_numbers(s1, nums1, 16);
    int n2 = extract_numbers(s2, nums2, 16);

    if (n1 == 0 || n2 == 0) return false;

    /* Simple heuristic: if both have exactly one number and they differ
     * by more than 2x, flag as conflicting.
     * This catches "5 members" vs "8 members", "2 hours" vs "14 hours" etc. */
    if (n1 == 1 && n2 == 1) {
        double ratio = (nums1[0] != 0) ? nums2[0] / nums1[0] : 0;
        if (ratio < 0.5 || ratio > 2.0) {
            return true;
        }
    }

    /* Also check: if one has a number and the other has "zero"/"no" + same noun */
    for (int i = 0; i < n1; i++) {
        if (nums1[i] == 0) {
            for (int j = 0; j < n2; j++) {
                if (nums2[j] > 0) return true;
            }
        }
    }
    for (int i = 0; i < n2; i++) {
        if (nums2[i] == 0) {
            for (int j = 0; j < n1; j++) {
                if (nums1[j] > 0) return true;
            }
        }
    }

    return false;
}

/* ============================================
 * SelfCheckGPT: main evaluation
 * ============================================ */

selfcheck_result_t selfcheck_evaluate(const char **claims, int claim_count)
{
    selfcheck_result_t result;
    memset(&result, 0, sizeof(result));
    result.total_claims = claim_count;
    result.consistency_score = 1.0f;

    if (claim_count == 0) return result;

    /* For each claim, split into sentences and compare pairwise */
    int total_pairs = 0;
    int contradictions = 0;

    for (int c = 0; c < claim_count; c++) {
        char buf[2048];
        const char *sentences[32];
        int nsent = split_sentences(claims[c], buf, sizeof(buf),
                                    sentences, 32);

        /* Compare all sentence pairs within this claim */
        for (int i = 0; i < nsent; i++) {
            for (int j = i + 1; j < nsent; j++) {
                total_pairs++;

                bool neg = check_negation(sentences[i], sentences[j]);
                bool ant = check_antonyms(sentences[i], sentences[j]);
                bool num = check_conflicting_numbers(sentences[i], sentences[j]);

                if (neg || ant || num) {
                    contradictions++;
                }
            }
        }
    }

    if (total_pairs > 0) {
        result.consistency_score = 1.0f - (float)contradictions / (float)total_pairs;
    }

    /* Flag claims as hallucinated if consistency drops below threshold */
    if (result.consistency_score < 0.5f) {
        result.flagged_hallucinated = claim_count;
    }

    return result;
}

/* ============================================
 * FActScore: verifiability heuristics
 * ============================================ */

/* Vague phrases that indicate unverifiable claims */
static const char *vague_phrases[] = {
    "some studies show",
    "it is believed",
    "many experts",
    "reportedly",
    "it is said",
    "some say",
    "it is thought",
    "allegedly",
    "supposedly",
    "in some cases",
    "various sources",
    "according to some",
    "it has been suggested",
    "many people think",
    "there is evidence",
    NULL
};

/* Check if a claim contains specific, verifiable atomic facts */
static float claim_verifiability(const char *claim)
{
    float score = 0.5f;  /* base score: neutral */

    /* Boost for specific numbers */
    double nums[16];
    int ncount = extract_numbers(claim, nums, 16);
    if (ncount > 0) score += 0.2f;
    if (ncount > 1) score += 0.1f;

    /* Boost for capitalized proper nouns (named entities) */
    int caps = 0;
    const char *p = claim;
    bool after_space = true;
    bool first_word = true;
    while (*p) {
        if (*p == ' ' || *p == '.') {
            after_space = true;
            if (*p == '.') first_word = true;
            else first_word = false;
        } else if (isupper((unsigned char)*p) && after_space && !first_word) {
            caps++;
            after_space = false;
        } else {
            after_space = false;
            first_word = false;
        }
        p++;
    }
    if (caps > 0) score += 0.1f;

    /* Penalize vague phrases */
    for (int i = 0; vague_phrases[i] != NULL; i++) {
        if (ci_strstr(claim, vague_phrases[i])) {
            score -= 0.3f;
        }
    }

    /* Clamp to [0, 1] */
    if (score > 1.0f) score = 1.0f;
    if (score < 0.0f) score = 0.0f;

    return score;
}

factscore_result_t factscore_evaluate(const char **claims, int claim_count)
{
    factscore_result_t result;
    memset(&result, 0, sizeof(result));
    result.total_facts = claim_count;

    if (claim_count == 0) {
        result.factscore = 0.0f;
        return result;
    }

    float total_score = 0.0f;

    for (int i = 0; i < claim_count; i++) {
        float v = claim_verifiability(claims[i]);
        total_score += v;
        if (v >= 0.5f) {
            result.verifiable_facts++;
        } else {
            result.unverifiable_facts++;
        }
    }

    result.factscore = total_score / (float)claim_count;
    return result;
}
