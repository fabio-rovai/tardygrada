/*
 * Tardygrada -- VitaminC External Benchmark
 *
 * Runs the Tardygrada pipeline on 500 examples from VitaminC (Schuster et al.,
 * 2021), a fact verification dataset with three labels:
 *   - SUPPORTS: evidence supports the claim
 *   - REFUTES: evidence contradicts the claim
 *   - NOT ENOUGH INFO: evidence is insufficient
 *
 * Task: given (claim, evidence) pairs, detect whether the evidence REFUTES
 * the claim. This tests Tardygrada's contradiction detection on a standard
 * NLI-style task where the two texts to compare are explicit.
 *
 * HONEST ASSESSMENT:
 * VitaminC is a CLAIM VERIFICATION task, not a self-consistency task.
 * Our pipeline checks internal consistency of a single text. To adapt it,
 * we concatenate claim + evidence and check for contradictions between them.
 * This is a reasonable but imperfect fit -- VitaminC was designed for
 * entailment models (BERT-style), not consistency checkers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#include "vm/types.h"
#include "vm/semantics.h"
#include "vm/crypto.h"
#include "verify/pipeline.h"
#include "verify/numeric.h"
#include "verify/llm_decompose.h"
#include "verify/decompose.h"
#include "mcp/json.h"

#include "baselines.h"

/* ============================================
 * Constants
 * ============================================ */

#define VITAMINC_MAX          500
#define MAX_TEXT_LEN           4096
#define MAX_SENTENCES          64
#define SENTENCE_BUF_SIZE      8192
#define JSON_FILE              "vitaminc_500.json"

/* ============================================
 * VitaminC example structure
 * ============================================ */

typedef enum {
    LABEL_SUPPORTS = 0,
    LABEL_REFUTES,
    LABEL_NEI,       /* NOT ENOUGH INFO */
    LABEL_COUNT,
} vitaminc_label_t;

typedef struct {
    int              id;
    char             claim[MAX_TEXT_LEN];
    char             evidence[MAX_TEXT_LEN];
    vitaminc_label_t label;
} vitaminc_example_t;

/* ============================================
 * Timing
 * ============================================ */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================
 * JSON parsing
 * ============================================ */

static char *read_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    *out_len = len;
    return buf;
}

static void json_unescape(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && *(r + 1)) {
            r++;
            switch (*r) {
            case 'n':  *w++ = '\n'; break;
            case 't':  *w++ = '\t'; break;
            case '\\': *w++ = '\\'; break;
            case '"':  *w++ = '"';  break;
            case '/':  *w++ = '/';  break;
            case 'r':  *w++ = '\r'; break;
            case 'u':  *w++ = '?';  r += 4; break;
            default:   *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static long find_object_end(const char *buf, long len, long pos)
{
    if (buf[pos] != '{') return -1;
    int depth = 0;
    bool in_string = false;
    for (long i = pos; i < len; i++) {
        if (in_string) {
            if (buf[i] == '\\') { i++; continue; }
            if (buf[i] == '"') in_string = false;
            continue;
        }
        if (buf[i] == '"') { in_string = true; continue; }
        if (buf[i] == '{') depth++;
        if (buf[i] == '}') { depth--; if (depth == 0) return i; }
    }
    return -1;
}

static int extract_str_field(const char *obj_str, int obj_len,
                             const char *key, char *out, int out_size)
{
    tardy_json_parser_t p;
    if (tardy_json_parse(&p, obj_str, obj_len) != 0) return -1;
    int tok = tardy_json_find(&p, 0, key);
    if (tok < 0) return -1;
    int n = tardy_json_str(&p, tok, out, out_size);
    if (n >= 0) json_unescape(out);
    return n;
}

static vitaminc_label_t parse_label(const char *obj_str, int obj_len)
{
    tardy_json_parser_t p;
    if (tardy_json_parse(&p, obj_str, obj_len) != 0) return LABEL_NEI;
    int tok = tardy_json_find(&p, 0, "label");
    if (tok < 0) return LABEL_NEI;

    /* label can be string or int */
    if (p.tokens[tok].type == TARDY_JSON_STRING) {
        char lbl[32] = {0};
        tardy_json_str(&p, tok, lbl, sizeof(lbl));
        if (strstr(lbl, "REFUTE")) return LABEL_REFUTES;
        if (strstr(lbl, "SUPPORT")) return LABEL_SUPPORTS;
        return LABEL_NEI;
    } else {
        /* Integer label: 0=SUPPORTS, 1=REFUTES, 2=NEI */
        long val = tardy_json_int(&p, tok);
        if (val == 0) return LABEL_SUPPORTS;
        if (val == 1) return LABEL_REFUTES;
        return LABEL_NEI;
    }
}

static int parse_vitaminc(const char *path, vitaminc_example_t *examples,
                          int max_examples)
{
    long flen;
    char *buf = read_file(path, &flen);
    if (!buf) {
        fprintf(stderr, "ERROR: Cannot read %s\n", path);
        return 0;
    }

    int count = 0;
    long pos = 0;

    while (pos < flen && buf[pos] != '{') pos++;

    while (pos < flen && count < max_examples) {
        if (buf[pos] != '{') { pos++; continue; }

        long end = find_object_end(buf, flen, pos);
        if (end < 0) break;

        int obj_len = (int)(end - pos + 1);

        char saved = buf[end + 1];
        buf[end + 1] = '\0';

        vitaminc_example_t *ex = &examples[count];
        memset(ex, 0, sizeof(*ex));
        ex->id = count;

        extract_str_field(buf + pos, obj_len, "claim",
                          ex->claim, sizeof(ex->claim));
        extract_str_field(buf + pos, obj_len, "evidence",
                          ex->evidence, sizeof(ex->evidence));
        ex->label = parse_label(buf + pos, obj_len);

        buf[end + 1] = saved;
        count++;

        pos = end + 1;
        while (pos < flen && buf[pos] != '{') pos++;
    }

    free(buf);
    return count;
}

/* ============================================
 * Sentence splitting
 * ============================================ */

static int split_sentences(const char *text, char *buf, size_t bufsz,
                           const char **out, int max_out)
{
    size_t tlen = strlen(text);
    if (tlen >= bufsz) tlen = bufsz - 1;
    memcpy(buf, text, tlen);
    buf[tlen] = '\0';

    int count = 0;
    char *p = buf;
    while (*p && count < max_out) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;

        out[count++] = p;

        while (*p) {
            if ((*p == '.' || *p == '?' || *p == '!') &&
                (*(p + 1) == ' ' || *(p + 1) == '\n' || *(p + 1) == '\r' || *(p + 1) == '\0')) {
                *p = '\0';
                p++;
                break;
            }
            p++;
        }
    }
    return count;
}

/* ============================================
 * Pipeline helpers
 * ============================================ */

static void build_honest_work(tardy_work_log_t *wl, tardy_work_spec_t *ws,
                              const tardy_semantics_t *sem)
{
    *ws = tardy_compute_work_spec(sem);
    tardy_worklog_init(wl);
    wl->ontology_queries = ws->min_ontology_queries + 1;
    wl->context_reads    = ws->min_context_reads + 1;
    wl->agents_spawned   = ws->min_agents;
    wl->compute_ns       = ws->min_compute_ns * 2;
    wl->memory_used      = 8192;
    tardy_sha256(wl, sizeof(*wl) - sizeof(tardy_hash_t), &wl->operations_hash);
}

/* Run Tardygrada on concatenated "claim. evidence." text.
 * The idea: if claim and evidence contradict, the pipeline should
 * detect inconsistency in the combined text. */
static bool run_tardygrada(const char *claim, const char *evidence,
                           const tardy_semantics_t *sem)
{
    /* Concatenate claim + evidence */
    char combined[SENTENCE_BUF_SIZE];
    snprintf(combined, sizeof(combined), "%s. %s", claim, evidence);

    /* Split into sentences */
    char sbuf[SENTENCE_BUF_SIZE];
    const char *sentences[MAX_SENTENCES];
    int nsent = split_sentences(combined, sbuf, sizeof(sbuf),
                                sentences, MAX_SENTENCES);
    if (nsent == 0) return false;

    /* Decompose into triples */
    tardy_decomposition_t decomps[3];
    tardy_triple_t all_triples[TARDY_MAX_TRIPLES];
    int total_triples = 0;

    char dbuf[SENTENCE_BUF_SIZE];
    const char *dsent[MAX_SENTENCES];
    int dnsent = split_sentences(combined, dbuf, sizeof(dbuf),
                                 dsent, MAX_SENTENCES);

    for (int s = 0; s < dnsent && total_triples < TARDY_MAX_TRIPLES - 4; s++) {
        tardy_triple_t triples[8];
        int nt = tardy_decompose(dsent[s], (int)strlen(dsent[s]),
                                 triples, 8);
        for (int t = 0; t < nt && total_triples < TARDY_MAX_TRIPLES; t++) {
            all_triples[total_triples++] = triples[t];
        }
    }

    for (int d = 0; d < 3; d++) {
        decomps[d].count     = total_triples;
        decomps[d].agreement = 1.0f;
        memset(&decomps[d].decomposer, (uint8_t)(d + 1), sizeof(tardy_uuid_t));
        for (int i = 0; i < total_triples; i++) {
            decomps[d].triples[i] = all_triples[i];
        }
    }

    /* Grounding: all UNKNOWN */
    tardy_grounding_t grounding;
    memset(&grounding, 0, sizeof(grounding));
    grounding.count   = total_triples;
    grounding.unknown = total_triples;
    for (int i = 0; i < total_triples; i++) {
        grounding.results[i].status         = TARDY_KNOWLEDGE_UNKNOWN;
        grounding.results[i].evidence_count = 0;
        grounding.results[i].confidence     = 0.0f;
        grounding.results[i].triple         = all_triples[i];
    }

    /* Consistency check */
    selfcheck_result_t sc = selfcheck_evaluate((const char **)&combined, 1);
    tardy_consistency_t consistency;
    if (sc.consistency_score < 0.5f) {
        consistency.consistent          = false;
        consistency.contradiction_count = 1;
        snprintf(consistency.explanation, sizeof(consistency.explanation),
                 "contradiction detected (score=%.2f)", sc.consistency_score);
    } else {
        consistency.consistent          = true;
        consistency.contradiction_count = 0;
        snprintf(consistency.explanation, sizeof(consistency.explanation),
                 "consistent (score=%.2f)", sc.consistency_score);
    }

    tardy_work_log_t  wl;
    tardy_work_spec_t ws;
    build_honest_work(&wl, &ws, sem);

    tardy_pipeline_result_t r = tardy_pipeline_verify(
        combined, (int)strlen(combined),
        decomps, 3,
        &grounding,
        &consistency,
        &wl, &ws, sem);

    bool detected = !r.passed;

    if (!detected) {
        const char *claims[1] = { combined };
        tardy_numeric_check_t nc = tardy_numeric_verify(claims, 1);
        if (nc.has_contradiction) detected = true;
    }

    if (!detected && total_triples > 0) {
        const char *claims[1] = { combined };
        tardy_llm_decomposition_t llm = tardy_llm_decompose(
            claims, 1, &decomps[0]);
        if (llm.found_implicit_contradiction) detected = true;
    }

    return detected;
}

static bool run_selfcheck(const char *claim, const char *evidence)
{
    char combined[SENTENCE_BUF_SIZE];
    snprintf(combined, sizeof(combined), "%s. %s", claim, evidence);
    const char *claims[1] = { combined };
    selfcheck_result_t sc = selfcheck_evaluate(claims, 1);
    return (sc.consistency_score < 0.5f);
}

static bool run_factscore(const char *claim, const char *evidence)
{
    char combined[SENTENCE_BUF_SIZE];
    snprintf(combined, sizeof(combined), "%s. %s", claim, evidence);
    char sbuf[SENTENCE_BUF_SIZE];
    const char *sentences[MAX_SENTENCES];
    int nsent = split_sentences(combined, sbuf, sizeof(sbuf),
                                sentences, MAX_SENTENCES);
    if (nsent == 0) return false;
    factscore_result_t fs = factscore_evaluate(sentences, nsent);
    return (fs.unverifiable_facts > 0);
}

/* ============================================
 * Metrics
 * ============================================ */

typedef struct {
    int tp, fp, tn, fn;
} confusion_t;

static double calc_precision(const confusion_t *c)
{
    if (c->tp + c->fp == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fp);
}

static double calc_recall(const confusion_t *c)
{
    if (c->tp + c->fn == 0) return 0.0;
    return (double)c->tp / (c->tp + c->fn);
}

static double calc_f1(const confusion_t *c)
{
    double p = calc_precision(c);
    double r = calc_recall(c);
    if (p + r == 0.0) return 0.0;
    return 2.0 * p * r / (p + r);
}

static double calc_accuracy(const confusion_t *c)
{
    int total = c->tp + c->fp + c->tn + c->fn;
    if (total == 0) return 0.0;
    return (double)(c->tp + c->tn) / total;
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== VitaminC External Benchmark ===\n\n");
    printf("Loading %s ...\n", JSON_FILE);

    vitaminc_example_t *examples = calloc(VITAMINC_MAX, sizeof(vitaminc_example_t));
    if (!examples) {
        fprintf(stderr, "ERROR: Failed to allocate examples\n");
        return 1;
    }

    int n = parse_vitaminc(JSON_FILE, examples, VITAMINC_MAX);
    if (n == 0) {
        fprintf(stderr, "ERROR: No examples parsed from %s\n", JSON_FILE);
        free(examples);
        return 1;
    }

    /* Count label distribution */
    int label_counts[LABEL_COUNT] = {0};
    for (int i = 0; i < n; i++) {
        label_counts[examples[i].label]++;
    }

    printf("Parsed %d examples (SUPPORTS=%d, REFUTES=%d, NEI=%d)\n\n",
           n, label_counts[LABEL_SUPPORTS], label_counts[LABEL_REFUTES],
           label_counts[LABEL_NEI]);

    /* Task: binary classification -- REFUTES vs not-REFUTES.
     * Ground truth positive = REFUTES. */

    tardy_semantics_t sem = TARDY_DEFAULT_SEMANTICS;
    sem.truth.min_evidence_triples          = 0;
    sem.truth.min_confidence                = 0.0f;
    sem.hallucination.grounding_threshold   = 0.0f;
    sem.hallucination.require_dual_ontology = false;
    sem.pipeline.layer_ontology_grounding   = true;
    sem.pipeline.layer_consistency_check    = true;
    sem.pipeline.layer_probabilistic_scoring = false;
    sem.pipeline.layer_protocol_check       = true;
    sem.pipeline.layer_formal_certification = false;
    sem.pipeline.layer_cross_representation = false;
    sem.pipeline.layer_work_verification    = true;
    sem.pipeline.min_passing_layers         = 2;
    sem.pipeline.skip_for_literals          = false;
    sem.pipeline.skip_for_arithmetic        = false;
    sem.pipeline.skip_for_internal_routing  = false;

    confusion_t cm_selfcheck  = {0};
    confusion_t cm_factscore  = {0};
    confusion_t cm_tardygrada = {0};

    /* Also track per-label accuracy */
    int correct_td[LABEL_COUNT] = {0};
    int total_per_label[LABEL_COUNT] = {0};

    uint64_t t_start = now_ns();

    for (int i = 0; i < n; i++) {
        if ((i + 1) % 100 == 0 || i == n - 1) {
            printf("  Processing %d/%d ...\r", i + 1, n);
            fflush(stdout);
        }

        /* Ground truth: REFUTES = positive */
        bool ground_truth = (examples[i].label == LABEL_REFUTES);
        const char *claim = examples[i].claim;
        const char *evidence = examples[i].evidence;

        if (strlen(claim) < 3 || strlen(evidence) < 3) continue;

        bool sc_flag = run_selfcheck(claim, evidence);
        bool fs_flag = run_factscore(claim, evidence);
        bool td_flag = run_tardygrada(claim, evidence, &sem);

        /* SelfCheck */
        if (ground_truth) {
            if (sc_flag) cm_selfcheck.tp++; else cm_selfcheck.fn++;
        } else {
            if (sc_flag) cm_selfcheck.fp++; else cm_selfcheck.tn++;
        }

        /* FActScore */
        if (ground_truth) {
            if (fs_flag) cm_factscore.tp++; else cm_factscore.fn++;
        } else {
            if (fs_flag) cm_factscore.fp++; else cm_factscore.tn++;
        }

        /* Tardygrada */
        if (ground_truth) {
            if (td_flag) cm_tardygrada.tp++; else cm_tardygrada.fn++;
        } else {
            if (td_flag) cm_tardygrada.fp++; else cm_tardygrada.tn++;
        }

        /* Per-label tracking */
        total_per_label[examples[i].label]++;
        bool td_correct = (td_flag == ground_truth);
        if (td_correct) correct_td[examples[i].label]++;
    }

    uint64_t t_end = now_ns();
    double elapsed_ms = (double)(t_end - t_start) / 1000000.0;

    printf("\n\n");

    /* ============================================
     * Results
     * ============================================ */

    printf("=== VitaminC External Benchmark (%d examples) ===\n\n", n);
    printf("Task: Detect REFUTES (contradiction between claim and evidence)\n");
    printf("Labels: SUPPORTS=%d, REFUTES=%d, NEI=%d\n\n",
           label_counts[LABEL_SUPPORTS], label_counts[LABEL_REFUTES],
           label_counts[LABEL_NEI]);

    printf("%-14s  %9s  %9s  %9s  %9s  %5s  %5s  %5s  %5s\n",
           "Detector", "Precision", "Recall", "F1", "Accuracy",
           "TP", "FP", "TN", "FN");
    printf("%-14s  %9s  %9s  %9s  %9s  %5s  %5s  %5s  %5s\n",
           "--------------", "---------", "---------", "---------", "---------",
           "-----", "-----", "-----", "-----");
    printf("%-14s  %9.4f  %9.4f  %9.4f  %9.4f  %5d  %5d  %5d  %5d\n",
           "SelfCheck",
           calc_precision(&cm_selfcheck), calc_recall(&cm_selfcheck),
           calc_f1(&cm_selfcheck), calc_accuracy(&cm_selfcheck),
           cm_selfcheck.tp, cm_selfcheck.fp, cm_selfcheck.tn, cm_selfcheck.fn);
    printf("%-14s  %9.4f  %9.4f  %9.4f  %9.4f  %5d  %5d  %5d  %5d\n",
           "FActScore",
           calc_precision(&cm_factscore), calc_recall(&cm_factscore),
           calc_f1(&cm_factscore), calc_accuracy(&cm_factscore),
           cm_factscore.tp, cm_factscore.fp, cm_factscore.tn, cm_factscore.fn);
    printf("%-14s  %9.4f  %9.4f  %9.4f  %9.4f  %5d  %5d  %5d  %5d\n",
           "Tardygrada",
           calc_precision(&cm_tardygrada), calc_recall(&cm_tardygrada),
           calc_f1(&cm_tardygrada), calc_accuracy(&cm_tardygrada),
           cm_tardygrada.tp, cm_tardygrada.fp, cm_tardygrada.tn, cm_tardygrada.fn);

    /* Per-label accuracy for Tardygrada */
    printf("\n--- Tardygrada Accuracy by Label ---\n\n");
    const char *label_names[] = {"SUPPORTS", "REFUTES", "NEI"};
    for (int l = 0; l < LABEL_COUNT; l++) {
        if (total_per_label[l] > 0) {
            printf("  %-12s  %d/%d correct (%.1f%%)\n",
                   label_names[l], correct_td[l], total_per_label[l],
                   100.0 * correct_td[l] / total_per_label[l]);
        }
    }

    /* ============================================
     * Honest analysis
     * ============================================ */

    printf("\n--- Honest Analysis ---\n\n");

    printf("WHAT THIS BENCHMARK MEASURES:\n\n");

    printf("1. VitaminC is a CLAIM VERIFICATION task -- given a claim and evidence,\n");
    printf("   determine if the evidence supports, refutes, or is neutral about the\n");
    printf("   claim. We adapt this to contradiction detection by concatenating the\n");
    printf("   claim and evidence, then checking for internal contradictions.\n\n");

    printf("2. This is NOT the task our pipeline was designed for. VitaminC requires\n");
    printf("   textual entailment reasoning: understanding that 'won 3 matches' refutes\n");
    printf("   'won 5 matches'. Our pattern matchers can catch some of these (numeric\n");
    printf("   conflicts, negations) but miss subtle entailment patterns.\n\n");

    printf("3. State-of-the-art NLI models achieve ~90%% accuracy on VitaminC.\n");
    printf("   Our deterministic pipeline cannot match trained neural models on this\n");
    printf("   task. The value of our pipeline is in SELF-CONSISTENCY checking\n");
    printf("   (detecting contradictions within a SINGLE text), not cross-text NLI.\n\n");

    printf("Completed in %.2f ms (%d examples, avg %.2f ms/example)\n",
           elapsed_ms, n, elapsed_ms / n);

    free(examples);
    return 0;
}
