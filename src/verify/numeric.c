/*
 * Tardygrada -- Numeric Contradiction Detector
 *
 * Extracts numbers from claim text and checks for numeric
 * inconsistencies that OWL reasoners cannot formalize.
 */

#include "numeric.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* ============================================
 * Helpers
 * ============================================ */

/* Case-insensitive substring search */
static int nc_contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return 0;
    int hlen = (int)strlen(haystack);
    int nlen = (int)strlen(needle);
    if (nlen > hlen) return 0;
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* Extract a double from a string starting at pos.
 * Handles: 123, 12.5, 1000, -20, 1B, 1M, 1K etc.
 * Returns the number and advances *pos past it. */
static double extract_number(const char *text, int len, int *pos)
{
    int i = *pos;
    /* Skip to start of number (optional minus sign) */
    while (i < len && !isdigit((unsigned char)text[i]) && text[i] != '-')
        i++;
    if (i >= len) return 0.0;
    /* Check minus is followed by digit */
    if (text[i] == '-' && (i + 1 >= len || !isdigit((unsigned char)text[i + 1])))
        return 0.0;

    int start = i;
    if (text[i] == '-') i++;
    while (i < len && (isdigit((unsigned char)text[i]) || text[i] == '.'))
        i++;

    char buf[64];
    int blen = i - start;
    if (blen >= 64) blen = 63;
    memcpy(buf, text + start, (size_t)blen);
    buf[blen] = '\0';

    double val = atof(buf);

    /* Check for multiplier suffix */
    if (i < len) {
        char c = text[i];
        if (c == 'B' || c == 'b') { val *= 1e9; i++; }
        else if (c == 'M' || c == 'm') {
            /* Distinguish "M" (million) from "ms", "m/s", "meters" etc. */
            if (i + 1 < len && (text[i + 1] == 's' || text[i + 1] == '/'))
                { /* don't multiply, it's ms or m/s */ }
            else if (c == 'M') { val *= 1e6; i++; }
        }
        else if (c == 'K' || c == 'k') {
            if (i + 1 < len && text[i + 1] == 'W') { /* kW, keep as is */ }
            else if (i + 1 < len && text[i + 1] == 'H') { /* kHz */ }
            else { val *= 1e3; i++; }
        }
        /* Check for word multipliers after a space: "10 million", "5 billion" */
        else if (c == ' ' && i + 4 < len) {
            if (nc_contains(text + i + 1, "million") &&
                (i + 8 >= len || !isalpha((unsigned char)text[i + 8])))
                { val *= 1e6; i += 8; }
            else if (nc_contains(text + i + 1, "billion") &&
                     (i + 8 >= len || !isalpha((unsigned char)text[i + 8])))
                { val *= 1e9; i += 8; }
            else if (nc_contains(text + i + 1, "thousand") &&
                     (i + 9 >= len || !isalpha((unsigned char)text[i + 9])))
                { val *= 1e3; i += 9; }
        }
    }

    *pos = i;
    return val;
}

/* ============================================
 * Number Extraction
 * ============================================ */

tardy_numeric_check_t tardy_numeric_extract(const char *claim, int claim_len)
{
    tardy_numeric_check_t result;
    memset(&result, 0, sizeof(result));

    if (!claim || claim_len <= 0) return result;

    int pos = 0;
    while (pos < claim_len && result.count < TARDY_NUMERIC_MAX_VALUES) {
        /* Find next digit or minus-digit */
        while (pos < claim_len &&
               !isdigit((unsigned char)claim[pos]) &&
               !(claim[pos] == '-' && pos + 1 < claim_len &&
                 isdigit((unsigned char)claim[pos + 1])))
            pos++;

        if (pos >= claim_len) break;

        int num_start = pos;
        double val = extract_number(claim, claim_len, &pos);

        /* Build a label from surrounding context (up to 30 chars before + 20 after) */
        int label_start = num_start - 30;
        if (label_start < 0) label_start = 0;
        /* Go back to start of word */
        while (label_start > 0 && claim[label_start - 1] != ' ' &&
               claim[label_start - 1] != '.')
            label_start--;

        int label_end = pos + 20;
        if (label_end > claim_len) label_end = claim_len;
        /* Extend to end of word */
        while (label_end < claim_len && claim[label_end] != ' ' &&
               claim[label_end] != '.' && claim[label_end] != ',')
            label_end++;

        int llen = label_end - label_start;
        if (llen >= TARDY_NUMERIC_MAX_LABEL) llen = TARDY_NUMERIC_MAX_LABEL - 1;
        if (llen > 0) {
            memcpy(result.labels[result.count], claim + label_start, (size_t)llen);
            result.labels[result.count][llen] = '\0';
        }

        result.values[result.count] = val;
        result.count++;

        if (pos <= num_start) pos++; /* prevent infinite loop */
    }

    return result;
}

/* ============================================
 * Pattern Checkers
 *
 * Each function checks for a specific class of
 * numeric contradiction. Returns 1 if found.
 * ============================================ */

/* Check 1: Rate/capacity saturation (queueing theory)
 * Pattern: high throughput + high latency = saturation
 * E.g. "1000 req/s with 950ms p99 latency" */
static int check_rate_saturation(const char *text, tardy_numeric_check_t *r)
{
    /* Look for req/s or rps combined with latency in ms */
    if (!(nc_contains(text, "req/s") || nc_contains(text, "rps") ||
          nc_contains(text, "request")))
        return 0;
    if (!(nc_contains(text, "latency") || nc_contains(text, "p99") ||
          nc_contains(text, "p95") || nc_contains(text, "response time")))
        return 0;

    /* Extract rate and latency */
    double rate = 0, latency_ms = 0;
    int threads = 0;
    for (int i = 0; i < r->count; i++) {
        if (nc_contains(r->labels[i], "req") ||
            nc_contains(r->labels[i], "rps") ||
            nc_contains(r->labels[i], "handle"))
            rate = r->values[i];
        else if (nc_contains(r->labels[i], "latency") ||
                 nc_contains(r->labels[i], "p99") ||
                 nc_contains(r->labels[i], "p95") ||
                 nc_contains(r->labels[i], "ms"))
            latency_ms = r->values[i];
        else if (nc_contains(r->labels[i], "thread") ||
                 nc_contains(r->labels[i], "worker"))
            threads = (int)r->values[i];
    }

    if (rate <= 0 || latency_ms <= 0) return 0;

    /* Little's law: concurrent_requests = rate * latency
     * If concurrent >> available threads, system is saturated.
     * With 950ms latency at 1000 rps: 950 concurrent requests.
     * Even with 1024 threads, utilization is 950/1024 = 93%
     * which means queueing delays dominate. */
    double concurrent = rate * (latency_ms / 1000.0);
    int capacity = threads > 0 ? threads : 1;

    /* Utilization > 90% with high p99 latency indicates saturation */
    if (concurrent > 0.85 * capacity && latency_ms > 500) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "queueing saturation: %.0f rps * %.0fms = %.0f concurrent "
                 "(%.0f%% of %d threads), p99 > 500ms indicates saturation",
                 rate, latency_ms, concurrent,
                 100.0 * concurrent / capacity, capacity);
        return 1;
    }
    return 0;
}

/* Check 2: Accuracy below majority baseline
 * Pattern: accuracy X% but dataset is Y% one class, and X < Y */
static int check_baseline_accuracy(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "accuracy") || nc_contains(text, "achieves")))
        return 0;
    if (!(nc_contains(text, "majority") || nc_contains(text, "class") ||
          nc_contains(text, "baseline") || nc_contains(text, "dataset")))
        return 0;

    double accuracy = 0, baseline = 0;
    for (int i = 0; i < r->count; i++) {
        if (nc_contains(r->labels[i], "accura") ||
            nc_contains(r->labels[i], "achieves"))
            accuracy = r->values[i];
        else if (nc_contains(r->labels[i], "majority") ||
                 nc_contains(r->labels[i], "class") ||
                 nc_contains(r->labels[i], "baseline") ||
                 nc_contains(r->labels[i], "dataset"))
            baseline = r->values[i];
    }

    /* Both should be percentages (0-100 range) */
    if (accuracy <= 0 || baseline <= 0) return 0;
    if (accuracy > 100 || baseline > 100) return 0;

    if (accuracy < baseline) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "accuracy %.0f%% is below majority class baseline %.0f%% "
                 "(model performs worse than always predicting the majority class)",
                 accuracy, baseline);
        return 1;
    }
    return 0;
}

/* Check 3: Statistical test correction (Bonferroni)
 * Pattern: p-value X with N tests, where X > 0.05/N */
static int check_bonferroni(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "p=") || nc_contains(text, "p <") ||
          nc_contains(text, "p-value") || nc_contains(text, "p =")))
        return 0;
    if (!(nc_contains(text, "test") || nc_contains(text, "comparison") ||
          nc_contains(text, "simultaneous")))
        return 0;

    double p_value = 0;
    int n_tests = 0;
    for (int i = 0; i < r->count; i++) {
        double v = r->values[i];
        /* p-values are small numbers, typically < 1 */
        if (v > 0 && v < 1 &&
            (nc_contains(r->labels[i], "p=") ||
             nc_contains(r->labels[i], "p ") ||
             nc_contains(r->labels[i], "p<")))
            p_value = v;
        else if (v > 1 &&
                 (nc_contains(r->labels[i], "test") ||
                  nc_contains(r->labels[i], "simultaneous") ||
                  nc_contains(r->labels[i], "applied")))
            n_tests = (int)v;
    }

    if (p_value <= 0 || n_tests <= 1) return 0;

    double corrected_threshold = 0.05 / n_tests;
    if (p_value > corrected_threshold) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "p=%.3f fails Bonferroni correction with %d tests "
                 "(corrected threshold = %.4f, p > threshold)",
                 p_value, n_tests, corrected_threshold);
        return 1;
    }
    return 0;
}

/* Check 4: Temperature mismatch
 * Pattern: component rated for X°C, used at Y°C where Y < X (for cold)
 * or Y > X (for heat) */
static int check_temperature_mismatch(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "rated") || nc_contains(text, "operating") ||
          nc_contains(text, "temperature")))
        return 0;
    if (!(nc_contains(text, "C") || nc_contains(text, "celsius") ||
          nc_contains(text, "°")))
        return 0;

    /* Find negative temperatures -- these are the interesting ones */
    double rated_temp = 999, used_temp = 999;
    for (int i = 0; i < r->count; i++) {
        double v = r->values[i];
        /* Look for temperature values (negative or realistic range) */
        if (v >= -200 && v <= 200) {
            if (nc_contains(r->labels[i], "rated") ||
                nc_contains(r->labels[i], "battery") ||
                nc_contains(r->labels[i], "operating"))
                rated_temp = v;
            else if (nc_contains(r->labels[i], "market") ||
                     nc_contains(r->labels[i], "expedit") ||
                     nc_contains(r->labels[i], "device") ||
                     nc_contains(r->labels[i], "environment") ||
                     nc_contains(r->labels[i], "antarc"))
                used_temp = v;
        }
    }

    /* If we didn't find labeled temps, try matching by pattern:
     * first negative = rated, second negative = used */
    if (rated_temp > 200 || used_temp > 200) {
        double neg_temps[4];
        int neg_count = 0;
        for (int i = 0; i < r->count && neg_count < 4; i++) {
            if (r->values[i] < 0 && r->values[i] > -200)
                neg_temps[neg_count++] = r->values[i];
        }
        if (neg_count >= 2) {
            rated_temp = neg_temps[0];  /* first mentioned = rated */
            used_temp = neg_temps[1];   /* second mentioned = actual use */
        }
    }

    if (rated_temp > 200 || used_temp > 200) return 0;

    /* For cold: if used_temp < rated_temp, device will fail */
    if (used_temp < rated_temp) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "temperature mismatch: component rated for %.0f°C "
                 "but used at %.0f°C (%.0f° below rated minimum)",
                 rated_temp, used_temp, rated_temp - used_temp);
        return 1;
    }
    return 0;
}

/* Check 5: Security bit mismatch
 * Pattern: claims N-bit security but entropy source has fewer bits */
static int check_security_bits(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "bit") || nc_contains(text, "security")))
        return 0;
    if (!(nc_contains(text, "seed") || nc_contains(text, "rng") ||
          nc_contains(text, "random") || nc_contains(text, "timestamp") ||
          nc_contains(text, "entropy")))
        return 0;

    double claimed_bits = 0;
    int has_timestamp_seed = nc_contains(text, "timestamp");
    int has_weak_rng = nc_contains(text, "seed");

    for (int i = 0; i < r->count; i++) {
        if (nc_contains(r->labels[i], "bit") ||
            nc_contains(r->labels[i], "security"))
            claimed_bits = r->values[i];
    }

    if (claimed_bits <= 0) return 0;

    /* Timestamp has ~32 bits of entropy (seconds since epoch in a
     * reasonable window is ~2^30; milliseconds ~2^40) */
    if (has_timestamp_seed && claimed_bits > 40) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "security mismatch: claims %.0f-bit security but RNG "
                 "seeded from timestamp (~32 bits of actual entropy)",
                 claimed_bits);
        return 1;
    }

    /* Generic weak seeding */
    if (has_weak_rng && claimed_bits > 64) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "security mismatch: claims %.0f-bit security but "
                 "uses potentially weak seeding",
                 claimed_bits);
        return 1;
    }

    return 0;
}

/* Check 6: Impossible review/throughput speed
 * Pattern: N items reviewed/processed in T seconds where N/T > human_max */
static int check_impossible_speed(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "review") || nc_contains(text, "examined") ||
          nc_contains(text, "inspect") || nc_contains(text, "audit")))
        return 0;
    if (!(nc_contains(text, "second") || nc_contains(text, "minute") ||
          nc_contains(text, "hour")))
        return 0;

    double items = 0, time_seconds = 0;
    for (int i = 0; i < r->count; i++) {
        double v = r->values[i];
        if (nc_contains(r->labels[i], "line") ||
            nc_contains(r->labels[i], "change") ||
            nc_contains(r->labels[i], "page") ||
            nc_contains(r->labels[i], "request") ||
            nc_contains(r->labels[i], "transact"))
            items = v;
        else if (nc_contains(r->labels[i], "second"))
            time_seconds = v;
        else if (nc_contains(r->labels[i], "minute"))
            time_seconds = v * 60;
        else if (nc_contains(r->labels[i], "hour"))
            time_seconds = v * 3600;
    }

    if (items <= 0 || time_seconds <= 0) return 0;

    double rate = items / time_seconds;

    /* A human can review ~200-400 lines of code per hour (~5-7/minute).
     * Anything above 10 lines/second is physically impossible. */
    if (nc_contains(text, "line") && rate > 10) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "impossible review speed: %.0f lines in %.0f seconds = "
                 "%.1f lines/sec (human max ~5-7 lines/min)",
                 items, time_seconds, rate);
        return 1;
    }

    /* For transactions/records, anything above 1/second for manual audit */
    if ((nc_contains(text, "transact") || nc_contains(text, "audit") ||
         nc_contains(text, "examined")) && rate > 1 && items > 100) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "impossible audit speed: %.0f items in %.0f seconds = "
                 "%.1f items/sec",
                 items, time_seconds, rate);
        return 1;
    }

    return 0;
}

/* Check 7: Massive overfitting (params >> data)
 * Pattern: N parameters trained on M data points where N >> M */
static int check_overfitting(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "param") || nc_contains(text, "neural") ||
          nc_contains(text, "model") || nc_contains(text, "network")))
        return 0;
    if (!(nc_contains(text, "train") || nc_contains(text, "data") ||
          nc_contains(text, "sample") || nc_contains(text, "point") ||
          nc_contains(text, "epoch")))
        return 0;

    double params = 0, data_points = 0;
    for (int i = 0; i < r->count; i++) {
        double v = r->values[i];
        if (nc_contains(r->labels[i], "param") ||
            nc_contains(r->labels[i], "network") ||
            nc_contains(r->labels[i], "neural"))
            params = v;
        else if (nc_contains(r->labels[i], "data") ||
                 nc_contains(r->labels[i], "point") ||
                 nc_contains(r->labels[i], "sample") ||
                 nc_contains(r->labels[i], "train"))
            data_points = v;
    }

    if (params <= 0 || data_points <= 0) return 0;

    /* Rule of thumb: if params/data > 100, massive overfitting risk.
     * 1B params on 500 data points = ratio of 2M. */
    double ratio = params / data_points;
    if (ratio > 100) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "massive overfitting: %.0g parameters on %.0f data points "
                 "(ratio %.0f:1, need at least 10:1 data-to-param)",
                 params, data_points, ratio);
        return 1;
    }
    return 0;
}

/* Check 8: Sample size too small for claimed margin of error
 * Pattern: margin of error M% with sample size N from population P
 * Minimum sample for 3% margin at 95% CI is ~1067 */
static int check_sample_size(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "margin") || nc_contains(text, "error") ||
          nc_contains(text, "survey")))
        return 0;
    if (!(nc_contains(text, "sample") || nc_contains(text, "people") ||
          nc_contains(text, "respondent")))
        return 0;

    double margin = 0, sample_size = 0;
    for (int i = 0; i < r->count; i++) {
        double v = r->values[i];
        if (v > 0 && v < 20 &&
            (nc_contains(r->labels[i], "margin") ||
             nc_contains(r->labels[i], "error") ||
             nc_contains(r->labels[i], "%")))
            margin = v;
        else if (v >= 10 && v <= 100000 &&
                 (nc_contains(r->labels[i], "sample") ||
                  nc_contains(r->labels[i], "people") ||
                  nc_contains(r->labels[i], "respondent") ||
                  nc_contains(r->labels[i], "surveyed")))
            sample_size = v;
    }

    if (margin <= 0 || sample_size <= 0) return 0;

    /* Minimum sample size for margin M at 95% CI:
     * n = (1.96^2 * 0.25) / (M/100)^2 = 0.9604 / (M/100)^2
     * For 3%: n = 0.9604 / 0.0009 = 1067 */
    double m_frac = margin / 100.0;
    double min_sample = 0.9604 / (m_frac * m_frac);

    if (sample_size < min_sample * 0.5) {  /* generous: half of minimum */
        snprintf(r->explanation, sizeof(r->explanation),
                 "sample too small: %.0f%% margin of error requires "
                 "~%.0f samples, but only %.0f were sampled",
                 margin, min_sample, sample_size);
        return 1;
    }
    return 0;
}

/* Check 9: Review ratio (employees vs reviews suggesting bias)
 * Pattern: N employees with M positive reviews where M/N > 0.7 */
static int check_review_ratio(const char *text, tardy_numeric_check_t *r)
{
    if (!(nc_contains(text, "employee") || nc_contains(text, "staff") ||
          nc_contains(text, "team")))
        return 0;
    if (!(nc_contains(text, "review") || nc_contains(text, "rating") ||
          nc_contains(text, "glassdoor") || nc_contains(text, "star")))
        return 0;

    double employees = 0, reviews = 0;
    /* Two-pass: first find the employee count, then find reviews.
     * Labels can overlap (wider context), so use positional priority:
     * employee count comes first in text, review count comes second. */
    for (int i = 0; i < r->count; i++) {
        if (employees == 0 &&
            (nc_contains(r->labels[i], "employee") ||
             nc_contains(r->labels[i], "staff") ||
             nc_contains(r->labels[i], "startup") ||
             nc_contains(r->labels[i], "team"))) {
            employees = r->values[i];
        }
    }
    for (int i = 0; i < r->count; i++) {
        if (r->values[i] != employees &&
            (nc_contains(r->labels[i], "review") ||
             nc_contains(r->labels[i], "star") ||
             nc_contains(r->labels[i], "five") ||
             nc_contains(r->labels[i], "rating"))) {
            reviews = r->values[i];
            break;
        }
    }

    if (employees <= 0 || reviews <= 0) return 0;

    /* If reviews/employees > 0.7, suspicious (selection bias).
     * 8 five-star reviews from 10 employees = 80% participation
     * with 100% positive = strong selection bias signal */
    double ratio = reviews / employees;
    if (ratio > 0.6 && employees <= 50) {
        snprintf(r->explanation, sizeof(r->explanation),
                 "selection bias: %.0f reviews from %.0f employees "
                 "(%.0f%% participation with all positive suggests bias)",
                 reviews, employees, ratio * 100);
        return 1;
    }
    return 0;
}

/* ============================================
 * Main Verification Entry Point
 * ============================================ */

tardy_numeric_check_t tardy_numeric_verify(const char **claims, int claim_count)
{
    tardy_numeric_check_t result;
    memset(&result, 0, sizeof(result));

    if (!claims || claim_count <= 0) return result;

    /* Concatenate all claims into one text for analysis */
    char combined[4096];
    int clen = 0;
    for (int i = 0; i < claim_count && clen < 4000; i++) {
        if (!claims[i]) continue;
        int slen = (int)strlen(claims[i]);
        if (clen + slen + 2 > 4000) break;
        if (clen > 0) { combined[clen++] = ' '; }
        memcpy(combined + clen, claims[i], (size_t)slen);
        clen += slen;
    }
    combined[clen] = '\0';

    /* Extract numbers first */
    result = tardy_numeric_extract(combined, clen);

    /* Run all pattern checkers — first match wins */
    if (check_rate_saturation(combined, &result))     { result.has_contradiction = true; return result; }
    if (check_baseline_accuracy(combined, &result))   { result.has_contradiction = true; return result; }
    if (check_bonferroni(combined, &result))           { result.has_contradiction = true; return result; }
    if (check_temperature_mismatch(combined, &result)) { result.has_contradiction = true; return result; }
    if (check_security_bits(combined, &result))        { result.has_contradiction = true; return result; }
    if (check_impossible_speed(combined, &result))     { result.has_contradiction = true; return result; }
    if (check_overfitting(combined, &result))          { result.has_contradiction = true; return result; }
    if (check_sample_size(combined, &result))          { result.has_contradiction = true; return result; }
    if (check_review_ratio(combined, &result))         { result.has_contradiction = true; return result; }

    return result;
}
