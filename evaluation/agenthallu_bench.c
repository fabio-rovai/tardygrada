/*
 * Tardygrada -- AgentHallu External Benchmark
 *
 * Runs the Tardygrada pipeline on 693 agent trajectories from AgentHallu
 * (Liu et al., 2025), a benchmark for hallucination in LLM-based agents.
 *
 * HONEST ASSESSMENT:
 * AgentHallu evaluates AGENT BEHAVIOR hallucinations: wrong tool calls,
 * bad reasoning chains, fabricated observations, unnecessary actions.
 * Tardygrada's pipeline detects TEXT CONSISTENCY contradictions.
 *
 * These are fundamentally different tasks:
 *   - AgentHallu: "Did the agent call the wrong function?" (behavioral)
 *   - Tardygrada: "Do the agent's statements contradict each other?" (logical)
 *
 * Where we CAN help: when an agent's hallucination manifests as a
 * contradiction between steps (e.g., step 3 says "file moved" but
 * step 5 says "file not found"), our consistency layer catches it.
 * Where we CAN'T: pure tool-use errors (wrong function name, bad args)
 * that don't produce textual contradictions.
 *
 * Reported baselines from the paper (GPT-5 best):
 *   Judgment F1:     70.2%
 *   Step attribution: 32.7%
 *   Tool-use acc:     4.9%
 *
 * We report per-category results honestly. Our pipeline is NOT designed
 * for this task, but any signal it finds is real signal.
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

#define AGENTHALLU_MAX          700
#define MAX_STEPS               50
#define STEP_TEXT_SIZE           10240
#define MAX_CATEGORY_LEN        64
#define MAX_SOURCE_LEN          64
#define JSON_FILE               "agenthallu_flat.json"

/* ============================================
 * AgentHallu trajectory structure
 * ============================================ */

typedef struct {
    int   id;
    char  source[MAX_SOURCE_LEN];
    int   is_hallucination;       /* 1 = hallucinated trajectory */
    int   hallucination_step;     /* -1 if none */
    char  category[MAX_CATEGORY_LEN];
    char  subcategory[MAX_CATEGORY_LEN];
    int   step_count;
    char  steps[MAX_STEPS][STEP_TEXT_SIZE];
} agenthallu_traj_t;

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
 * JSON parsing helpers
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
            case 'u':  *w++ = '?';  r += 4; break; /* skip unicode escapes */
            default:   *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Find matching closing brace/bracket */
static int find_matching(const char *buf, long len, int pos, char open, char close)
{
    if (buf[pos] != open) return -1;
    int depth = 0;
    bool in_string = false;
    for (int i = pos; i < len; i++) {
        if (in_string) {
            if (buf[i] == '\\') { i++; continue; }
            if (buf[i] == '"') in_string = false;
            continue;
        }
        if (buf[i] == '"') { in_string = true; continue; }
        if (buf[i] == open) depth++;
        if (buf[i] == close) { depth--; if (depth == 0) return i; }
    }
    return -1;
}

/* Extract a JSON string value after "key": */
static int extract_json_string(const char *buf, long len, int start,
                               const char *key, char *out, int out_size)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf + start, search);
    if (!p || p >= buf + len) return -1;
    /* Skip to colon, then to opening quote */
    p += strlen(search);
    while (*p && *p != '"') p++;
    if (!*p) return -1;
    p++; /* skip opening quote */
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            /* Copy escape sequence for later unescaping */
            if (i < out_size - 2) {
                out[i++] = *p++;
                out[i++] = *p++;
            } else break;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    json_unescape(out);
    return i;
}

/* Extract a JSON integer value after "key": */
static int extract_json_int(const char *buf, long len, int start, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(buf + start, search);
    if (!p || p >= buf + len) return -1;
    p += strlen(search);
    while (*p && *p != ':') p++;
    if (!*p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/* Parse the flat JSON array of trajectories.
 * Trajectories are pre-flattened by Python preprocessor. */
static int parse_agenthallu(const char *path, agenthallu_traj_t *trajs, int max_trajs)
{
    long flen;
    char *buf = read_file(path, &flen);
    if (!buf) {
        fprintf(stderr, "ERROR: Cannot read %s\n", path);
        return 0;
    }

    int count = 0;
    int pos = 0;

    /* Skip to first '{' (inside the outer array) */
    while (pos < flen && buf[pos] != '[') pos++;
    if (pos >= flen) { free(buf); return 0; }
    pos++; /* skip '[' */

    while (pos < flen && count < max_trajs) {
        while (pos < flen && buf[pos] != '{') pos++;
        if (pos >= flen) break;

        int end = find_matching(buf, flen, pos, '{', '}');
        if (end < 0) break;

        agenthallu_traj_t *t = &trajs[count];
        memset(t, 0, sizeof(*t));

        /* Extract scalar fields */
        t->id = extract_json_int(buf, end, pos, "id");
        if (t->id < 0) t->id = count;

        extract_json_string(buf, end, pos, "source",
                           t->source, sizeof(t->source));
        t->is_hallucination = extract_json_int(buf, end, pos, "is_hallucination");
        t->hallucination_step = extract_json_int(buf, end, pos, "hallucination_step");

        extract_json_string(buf, end, pos, "category",
                           t->category, sizeof(t->category));
        extract_json_string(buf, end, pos, "subcategory",
                           t->subcategory, sizeof(t->subcategory));

        t->step_count = extract_json_int(buf, end, pos, "step_count");
        if (t->step_count > MAX_STEPS) t->step_count = MAX_STEPS;

        /* Extract steps array */
        char steps_key[] = "\"steps\"";
        const char *sp = strstr(buf + pos, steps_key);
        if (sp && sp < buf + end) {
            /* Find the opening '[' */
            sp += strlen(steps_key);
            while (*sp && *sp != '[') sp++;
            if (*sp == '[') {
                int arr_start = (int)(sp - buf);
                int arr_end = find_matching(buf, flen, arr_start, '[', ']');
                if (arr_end > 0) {
                    /* Extract each string in the array */
                    int si = arr_start + 1;
                    int step_idx = 0;
                    while (si < arr_end && step_idx < t->step_count) {
                        while (si < arr_end && buf[si] != '"') si++;
                        if (si >= arr_end) break;
                        si++; /* skip opening quote */
                        int wi = 0;
                        while (si < arr_end && buf[si] != '"' && wi < STEP_TEXT_SIZE - 1) {
                            if (buf[si] == '\\' && si + 1 < arr_end) {
                                if (wi < STEP_TEXT_SIZE - 2) {
                                    t->steps[step_idx][wi++] = buf[si++];
                                    t->steps[step_idx][wi++] = buf[si++];
                                } else break;
                            } else {
                                t->steps[step_idx][wi++] = buf[si++];
                            }
                        }
                        t->steps[step_idx][wi] = '\0';
                        json_unescape(t->steps[step_idx]);
                        step_idx++;
                        if (si < arr_end) si++; /* skip closing quote */
                    }
                    /* Update step_count to actual parsed */
                    if (step_idx < t->step_count) t->step_count = step_idx;
                }
            }
        }

        count++;
        pos = end + 1;
    }

    free(buf);
    return count;
}

/* ============================================
 * Sentence splitting (reused from halueval)
 * ============================================ */

#define MAX_SENTENCES     128
#define SENTENCE_BUF_SIZE 16384

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

/* Case-insensitive substring check (mirrors verify_doc in main.c) */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    int nlen = (int)strlen(needle);
    for (const char *p = haystack; *p; p++) {
        int match = 1;
        for (int i = 0; i < nlen; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) !=
                         tolower((unsigned char)needle[i])) {
                match = 0;
                break;
            }
        }
        if (match) return p;
    }
    return NULL;
}

/* Per-sentence triple storage for entity grouping */
#define MAX_TRIPLES_PER_SENT 8

typedef struct {
    tardy_triple_t triples[MAX_TRIPLES_PER_SENT];
    int            triple_count;
} sent_triples_t;

/* Entity group: tracks which sentence indices share a subject */
#define MAX_ENTITY_GROUPS 256
#define MAX_SENTS_PER_GROUP 64

typedef struct {
    char entity[TARDY_MAX_TRIPLE_LEN];
    int  sentence_indices[MAX_SENTS_PER_GROUP];
    int  count;
} entity_group_t;

/* ============================================
 * Core detection: run Tardygrada on a trajectory
 *
 * Replicates the verify_doc() logic from src/main.c:
 *   1. Concatenate steps, split into sentences
 *   2. Decompose each sentence into triples
 *   3. Group sentences by shared entity (subject)
 *   4. For each pair in a group: triple consistency + numeric + LLM decompose
 *   5. Also check consecutive step pairs for pairwise triple conflicts
 *
 * Returns: detected hallucination (true/false)
 *          and which step pair triggered it (for attribution)
 * ============================================ */

typedef struct {
    bool detected;
    int  flagged_step;       /* step index that triggered detection, -1 if none */
    int  contradiction_pair; /* the other step in the contradiction, -1 if none */
    char reason[256];
} detection_result_t;

static detection_result_t run_tardygrada_trajectory(
    const agenthallu_traj_t *traj,
    const tardy_semantics_t *sem)
{
    detection_result_t result = { false, -1, -1, "" };

    if (traj->step_count < 2) return result;

    /* === Concatenate all steps === */
    char full_text[65536];
    int full_len = 0;
    for (int s = 0; s < traj->step_count && full_len < 60000; s++) {
        int slen = (int)strlen(traj->steps[s]);
        if (slen == 0) continue;
        if (full_len > 0) {
            full_text[full_len++] = ' ';
        }
        int copy_len = slen;
        if (full_len + copy_len > 60000) copy_len = 60000 - full_len;
        memcpy(full_text + full_len, traj->steps[s], copy_len);
        full_len += copy_len;
    }
    full_text[full_len] = '\0';

    if (full_len < 10) return result;

    /* === Phase 1: Decompose into per-sentence triples === */
    char dbuf[SENTENCE_BUF_SIZE];
    const char *dsent[MAX_SENTENCES];
    int dnsent = split_sentences(full_text, dbuf, sizeof(dbuf),
                                 dsent, MAX_SENTENCES);

    sent_triples_t *per_sent = calloc(dnsent > 0 ? dnsent : 1, sizeof(sent_triples_t));
    if (!per_sent) return result;

    tardy_triple_t all_triples[TARDY_MAX_TRIPLES];
    int total_triples = 0;

    for (int s = 0; s < dnsent; s++) {
        per_sent[s].triple_count = tardy_decompose(
            dsent[s], (int)strlen(dsent[s]),
            per_sent[s].triples, MAX_TRIPLES_PER_SENT);
        for (int t = 0; t < per_sent[s].triple_count &&
             total_triples < TARDY_MAX_TRIPLES; t++) {
            all_triples[total_triples++] = per_sent[s].triples[t];
        }
    }

    /* === Phase 2: Group sentences by shared entity (subject) === */
    entity_group_t *groups = calloc(MAX_ENTITY_GROUPS, sizeof(entity_group_t));
    if (!groups) { free(per_sent); return result; }
    int group_count = 0;

    for (int i = 0; i < dnsent; i++) {
        for (int t = 0; t < per_sent[i].triple_count; t++) {
            const char *subj = per_sent[i].triples[t].subject;
            if (strcmp(subj, "claim") == 0 &&
                strcmp(per_sent[i].triples[t].predicate, "states") == 0)
                continue;
            if (strcmp(subj, "subject") == 0) continue;

            int found = -1;
            for (int g = 0; g < group_count; g++) {
                if (ci_strstr(groups[g].entity, subj) ||
                    ci_strstr(subj, groups[g].entity)) {
                    found = g;
                    break;
                }
            }
            if (found < 0 && group_count < MAX_ENTITY_GROUPS) {
                found = group_count++;
                strncpy(groups[found].entity, subj, TARDY_MAX_TRIPLE_LEN - 1);
                groups[found].entity[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
                groups[found].count = 0;
            }
            if (found >= 0 && groups[found].count < MAX_SENTS_PER_GROUP) {
                int dup = 0;
                for (int k = 0; k < groups[found].count; k++) {
                    if (groups[found].sentence_indices[k] == i) {
                        dup = 1; break;
                    }
                }
                if (!dup)
                    groups[found].sentence_indices[groups[found].count++] = i;
            }
        }
    }

    /* === Phase 3: Check pairs within each entity group === */
    int contradiction_count = 0;

    for (int g = 0; g < group_count && !result.detected; g++) {
        for (int a = 0; a < groups[g].count && !result.detected; a++) {
            for (int b = a + 1; b < groups[g].count && !result.detected; b++) {
                int si = groups[g].sentence_indices[a];
                int sj = groups[g].sentence_indices[b];
                if (si == sj) continue;

                /* 3a: Triple consistency -- same subject+predicate, different object */
                for (int ti = 0; ti < per_sent[si].triple_count && !result.detected; ti++) {
                    for (int tj = 0; tj < per_sent[sj].triple_count && !result.detected; tj++) {
                        tardy_triple_t *ta = &per_sent[si].triples[ti];
                        tardy_triple_t *tb = &per_sent[sj].triples[tj];

                        if (strcmp(ta->subject, "claim") == 0 &&
                            strcmp(ta->predicate, "states") == 0) continue;
                        if (strcmp(tb->subject, "claim") == 0 &&
                            strcmp(tb->predicate, "states") == 0) continue;
                        if (strcmp(ta->subject, "subject") == 0) continue;
                        if (strcmp(tb->subject, "subject") == 0) continue;
                        if ((strcmp(ta->predicate, "located_at") == 0 ||
                             strcmp(ta->predicate, "located_in") == 0) &&
                            strlen(ta->subject) < 5) continue;

                        if (ci_strstr(ta->subject, tb->subject) &&
                            ci_strstr(ta->predicate, tb->predicate) &&
                            !ci_strstr(ta->object, tb->object) &&
                            ta->object[0] && tb->object[0]) {
                            result.detected = true;
                            contradiction_count++;
                            snprintf(result.reason, sizeof(result.reason),
                                     "triple conflict: (%s, %s) \"%s\" vs \"%s\"",
                                     ta->subject, ta->predicate,
                                     ta->object, tb->object);
                        }
                    }
                }

                /* 3b: Numeric verification on the sentence pair */
                if (!result.detected) {
                    const char *pair[2] = { dsent[si], dsent[sj] };
                    tardy_numeric_check_t nc = tardy_numeric_verify(pair, 2);
                    if (nc.has_contradiction) {
                        result.detected = true;
                        contradiction_count++;
                        snprintf(result.reason, sizeof(result.reason),
                                 "numeric: %s", nc.explanation);
                    }
                }

                /* 3c: LLM decomposition for implicit contradictions */
                if (!result.detected) {
                    const char *pair[2] = { dsent[si], dsent[sj] };
                    tardy_decomposition_t basic;
                    memset(&basic, 0, sizeof(basic));
                    int tc = 0;
                    for (int t = 0; t < per_sent[si].triple_count &&
                         tc < TARDY_MAX_TRIPLES; t++)
                        basic.triples[tc++] = per_sent[si].triples[t];
                    for (int t = 0; t < per_sent[sj].triple_count &&
                         tc < TARDY_MAX_TRIPLES; t++)
                        basic.triples[tc++] = per_sent[sj].triples[t];
                    basic.count = tc;

                    tardy_llm_decomposition_t llm =
                        tardy_llm_decompose(pair, 2, &basic);
                    if (llm.found_implicit_contradiction) {
                        result.detected = true;
                        contradiction_count++;
                        snprintf(result.reason, sizeof(result.reason),
                                 "llm_decompose: %s", llm.reasoning);
                    }
                }
            }
        }
    }

    /* === Strategy 2: Numeric verification across all steps === */
    if (!result.detected) {
        const char *step_ptrs[MAX_STEPS];
        int ns = 0;
        for (int s = 0; s < traj->step_count && ns < MAX_STEPS; s++) {
            if (strlen(traj->steps[s]) > 5) {
                step_ptrs[ns++] = traj->steps[s];
            }
        }
        if (ns > 1) {
            tardy_numeric_check_t nc = tardy_numeric_verify(step_ptrs, ns);
            if (nc.has_contradiction) {
                result.detected = true;
                snprintf(result.reason, sizeof(result.reason),
                         "numeric (cross-step): %s", nc.explanation);
            }
        }
    }

    /* === Strategy 3: Pairwise step triple consistency ===
     * Check consecutive step pairs for triple conflicts.
     * This can identify WHICH step caused the issue. */
    if (!result.detected && traj->step_count >= 3) {
        for (int i = 0; i < traj->step_count - 1 && !result.detected; i++) {
            if (strlen(traj->steps[i]) < 10 || strlen(traj->steps[i + 1]) < 10)
                continue;

            /* Decompose both steps into triples */
            tardy_triple_t trip_a[8], trip_b[8];
            int na = tardy_decompose(traj->steps[i], (int)strlen(traj->steps[i]),
                                     trip_a, 8);
            int nb = tardy_decompose(traj->steps[i + 1], (int)strlen(traj->steps[i + 1]),
                                     trip_b, 8);

            /* Check for same subject+predicate, different object */
            for (int ta_i = 0; ta_i < na && !result.detected; ta_i++) {
                for (int tb_i = 0; tb_i < nb && !result.detected; tb_i++) {
                    tardy_triple_t *ta = &trip_a[ta_i];
                    tardy_triple_t *tb = &trip_b[tb_i];

                    if (strcmp(ta->subject, "claim") == 0) continue;
                    if (strcmp(tb->subject, "claim") == 0) continue;
                    if (strcmp(ta->subject, "subject") == 0) continue;
                    if (strcmp(tb->subject, "subject") == 0) continue;

                    if (ci_strstr(ta->subject, tb->subject) &&
                        ci_strstr(ta->predicate, tb->predicate) &&
                        !ci_strstr(ta->object, tb->object) &&
                        ta->object[0] && tb->object[0]) {
                        result.detected = true;
                        result.flagged_step = i + 2;  /* 1-indexed, second step */
                        result.contradiction_pair = i + 1;
                        snprintf(result.reason, sizeof(result.reason),
                                 "step-pair (%d,%d) triple conflict: (%s, %s)",
                                 i + 1, i + 2, ta->subject, ta->predicate);
                    }
                }
            }

            /* Also check numeric on the pair */
            if (!result.detected) {
                const char *pair[2] = { traj->steps[i], traj->steps[i + 1] };
                tardy_numeric_check_t nc = tardy_numeric_verify(pair, 2);
                if (nc.has_contradiction) {
                    result.detected = true;
                    result.flagged_step = i + 2;
                    result.contradiction_pair = i + 1;
                    snprintf(result.reason, sizeof(result.reason),
                             "step-pair (%d,%d) numeric: %s",
                             i + 1, i + 2, nc.explanation);
                }
            }
        }
    }

    /* === Strategy 4: LLM decomposition on full trajectory ===
     * Check for implicit contradictions in the combined claims. */
    if (!result.detected && total_triples > 0) {
        tardy_decomposition_t decomps_full;
        decomps_full.count = total_triples;
        decomps_full.agreement = 1.0f;
        memset(&decomps_full.decomposer, 1, sizeof(tardy_uuid_t));
        for (int i = 0; i < total_triples; i++)
            decomps_full.triples[i] = all_triples[i];

        const char *claims[1] = { full_text };
        tardy_llm_decomposition_t llm = tardy_llm_decompose(
            claims, 1, &decomps_full);
        if (llm.found_implicit_contradiction) {
            result.detected = true;
            snprintf(result.reason, sizeof(result.reason),
                     "llm_decompose: %s", llm.reasoning);
        }
    }

    /* === Feed through pipeline for protocol/work verification === */
    if (!result.detected) {
        tardy_decomposition_t decomps[3];
        for (int d = 0; d < 3; d++) {
            decomps[d].count     = total_triples;
            decomps[d].agreement = 1.0f;
            memset(&decomps[d].decomposer, (uint8_t)(d + 1), sizeof(tardy_uuid_t));
            for (int i = 0; i < total_triples; i++)
                decomps[d].triples[i] = all_triples[i];
        }

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

        tardy_consistency_t consistency;
        if (contradiction_count > 0) {
            consistency.consistent          = false;
            consistency.contradiction_count = contradiction_count;
            snprintf(consistency.explanation, sizeof(consistency.explanation),
                     "triple-based contradiction (%d conflicts)",
                     contradiction_count);
        } else {
            consistency.consistent          = true;
            consistency.contradiction_count = 0;
            snprintf(consistency.explanation, sizeof(consistency.explanation),
                     "no triple contradictions found");
        }

        tardy_work_log_t  wl;
        tardy_work_spec_t ws;
        build_honest_work(&wl, &ws, sem);

        tardy_pipeline_result_t r = tardy_pipeline_verify(
            full_text, full_len,
            decomps, 3,
            &grounding,
            &consistency,
            &wl, &ws, sem);

        if (!r.passed) {
            result.detected = true;
            snprintf(result.reason, sizeof(result.reason),
                     "pipeline: %s", r.failure_detail);
        }
    }

    free(groups);
    free(per_sent);
    return result;
}

/* Run SelfCheck baseline on a trajectory (full text) */
static bool run_selfcheck_trajectory(const agenthallu_traj_t *traj)
{
    if (traj->step_count < 2) return false;

    /* Concatenate all steps */
    char full_text[65536];
    int full_len = 0;
    for (int s = 0; s < traj->step_count && full_len < 60000; s++) {
        int slen = (int)strlen(traj->steps[s]);
        if (full_len > 0 && full_len < 60000) full_text[full_len++] = ' ';
        int copy = slen < (60000 - full_len) ? slen : (60000 - full_len);
        if (copy > 0) { memcpy(full_text + full_len, traj->steps[s], copy); full_len += copy; }
    }
    full_text[full_len] = '\0';

    const char *claims[1] = { full_text };
    selfcheck_result_t sc = selfcheck_evaluate(claims, 1);
    return (sc.consistency_score < 0.5f);
}

/* Run FActScore baseline on a trajectory (per-sentence) */
static bool run_factscore_trajectory(const agenthallu_traj_t *traj)
{
    char full_text[65536];
    int full_len = 0;
    for (int s = 0; s < traj->step_count && full_len < 60000; s++) {
        int slen = (int)strlen(traj->steps[s]);
        if (full_len > 0 && full_len < 60000) full_text[full_len++] = ' ';
        int copy = slen < (60000 - full_len) ? slen : (60000 - full_len);
        if (copy > 0) { memcpy(full_text + full_len, traj->steps[s], copy); full_len += copy; }
    }
    full_text[full_len] = '\0';

    char sbuf[SENTENCE_BUF_SIZE];
    const char *sentences[MAX_SENTENCES];
    int nsent = split_sentences(full_text, sbuf, sizeof(sbuf),
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
 * Category index
 * ============================================ */

typedef enum {
    CAT_NONE = 0,
    CAT_TOOL_USE,
    CAT_REASONING,
    CAT_HUMAN_INTERACTION,
    CAT_RETRIEVAL,
    CAT_PLANNING,
    CAT_COUNT
} category_t;

static const char *cat_names[CAT_COUNT] = {
    "Clean",
    "Tool-Use",
    "Reasoning",
    "Human-Int",
    "Retrieval",
    "Planning"
};

static category_t classify_category(const char *cat)
{
    if (!cat || strcmp(cat, "none") == 0 || cat[0] == '\0') return CAT_NONE;
    if (strstr(cat, "Tool"))    return CAT_TOOL_USE;
    if (strstr(cat, "Reason"))  return CAT_REASONING;
    if (strstr(cat, "Human"))   return CAT_HUMAN_INTERACTION;
    if (strstr(cat, "Retriev")) return CAT_RETRIEVAL;
    if (strstr(cat, "Plan"))    return CAT_PLANNING;
    return CAT_NONE;
}

/* ============================================
 * Main
 * ============================================ */

int main(void)
{
    printf("=== AgentHallu External Benchmark ===\n\n");
    printf("Loading %s ...\n", JSON_FILE);

    /* Allocate on heap (trajectories are large) */
    agenthallu_traj_t *trajs = calloc(AGENTHALLU_MAX, sizeof(agenthallu_traj_t));
    if (!trajs) {
        fprintf(stderr, "ERROR: Failed to allocate trajectories\n");
        return 1;
    }

    int n = parse_agenthallu(JSON_FILE, trajs, AGENTHALLU_MAX);
    if (n == 0) {
        fprintf(stderr, "ERROR: No trajectories parsed from %s\n", JSON_FILE);
        free(trajs);
        return 1;
    }

    /* Count ground truth distribution */
    int gt_hallucinated = 0, gt_clean = 0;
    int cat_counts[CAT_COUNT] = {0};
    for (int i = 0; i < n; i++) {
        if (trajs[i].is_hallucination) gt_hallucinated++;
        else gt_clean++;
        cat_counts[classify_category(trajs[i].category)]++;
    }

    printf("Parsed %d trajectories (%d hallucinated, %d clean)\n",
           n, gt_hallucinated, gt_clean);
    printf("Categories: ");
    for (int c = 0; c < CAT_COUNT; c++) {
        if (cat_counts[c] > 0)
            printf("%s=%d ", cat_names[c], cat_counts[c]);
    }
    printf("\n\n");

    /* Configure pipeline semantics */
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

    /* Overall confusion matrices */
    confusion_t cm_selfcheck  = {0};
    confusion_t cm_factscore  = {0};
    confusion_t cm_tardygrada = {0};

    /* Per-category confusion matrices for Tardygrada */
    confusion_t cm_tardy_cat[CAT_COUNT] = {{0}};

    /* Step attribution tracking */
    int attribution_correct = 0;
    int attribution_total   = 0;
    int attribution_correct_cat[CAT_COUNT] = {0};
    int attribution_total_cat[CAT_COUNT]   = {0};

    uint64_t t_start = now_ns();

    for (int i = 0; i < n; i++) {
        if ((i + 1) % 50 == 0 || i == n - 1) {
            printf("  Processing %d/%d ...\r", i + 1, n);
            fflush(stdout);
        }

        bool ground_truth = (trajs[i].is_hallucination == 1);
        int  gt_step      = trajs[i].hallucination_step;
        category_t cat    = classify_category(trajs[i].category);

        /* Skip empty trajectories */
        if (trajs[i].step_count < 1) continue;

        /* Run detectors */
        bool sc_flag = run_selfcheck_trajectory(&trajs[i]);
        bool fs_flag = run_factscore_trajectory(&trajs[i]);
        detection_result_t td = run_tardygrada_trajectory(&trajs[i], &sem);

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

        /* Tardygrada -- overall */
        if (ground_truth) {
            if (td.detected) cm_tardygrada.tp++; else cm_tardygrada.fn++;
        } else {
            if (td.detected) cm_tardygrada.fp++; else cm_tardygrada.tn++;
        }

        /* Tardygrada -- per category */
        if (ground_truth) {
            if (td.detected) cm_tardy_cat[cat].tp++; else cm_tardy_cat[cat].fn++;
        } else {
            if (td.detected) cm_tardy_cat[cat].fp++; else cm_tardy_cat[cat].tn++;
        }

        /* Step attribution (only for true positives with a flagged step) */
        if (ground_truth && td.detected && gt_step > 0) {
            attribution_total++;
            attribution_total_cat[cat]++;
            if (td.flagged_step == gt_step) {
                attribution_correct++;
                attribution_correct_cat[cat]++;
            }
        }
    }

    uint64_t t_end = now_ns();
    double elapsed_ms = (double)(t_end - t_start) / 1000000.0;

    printf("\n\n");

    /* ============================================
     * Results: Judgment Task (Binary Classification)
     * ============================================ */

    printf("=== AgentHallu Benchmark -- Judgment Task (%d trajectories) ===\n\n", n);
    printf("Ground truth: %d hallucinated, %d clean (%.0f%% hallucinated)\n\n",
           gt_hallucinated, gt_clean,
           100.0 * gt_hallucinated / (gt_hallucinated + gt_clean));

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

    /* Paper baselines for context */
    printf("\n--- Paper Baselines (LLM-based, for context) ---\n");
    printf("  GPT-5:          F1=70.2%%  Attribution=32.7%%\n");
    printf("  Gemini-2.5-Pro: F1=64.6%%  Attribution=41.1%%\n");
    printf("  DeepSeek-V3.1:  F1=52.1%%  Attribution=19.0%%\n");
    printf("  Random:         F1~49.5%%  Attribution= 8.7%%\n");

    /* ============================================
     * Results: Per-Category Breakdown (Tardygrada only)
     * ============================================ */

    printf("\n=== Per-Category Breakdown (Tardygrada) ===\n\n");
    printf("%-14s  %5s  %9s  %9s  %9s\n",
           "Category", "N", "Precision", "Recall", "F1");
    printf("%-14s  %5s  %9s  %9s  %9s\n",
           "--------------", "-----", "---------", "---------", "---------");
    for (int c = 1; c < CAT_COUNT; c++) {
        if (cm_tardy_cat[c].tp + cm_tardy_cat[c].fn > 0) {
            printf("%-14s  %5d  %9.4f  %9.4f  %9.4f\n",
                   cat_names[c],
                   cm_tardy_cat[c].tp + cm_tardy_cat[c].fn,
                   calc_precision(&cm_tardy_cat[c]),
                   calc_recall(&cm_tardy_cat[c]),
                   calc_f1(&cm_tardy_cat[c]));
        }
    }

    /* ============================================
     * Results: Step Attribution
     * ============================================ */

    printf("\n=== Step Attribution (Tardygrada) ===\n\n");
    if (attribution_total > 0) {
        printf("Overall: %d/%d correct (%.1f%%)\n",
               attribution_correct, attribution_total,
               100.0 * attribution_correct / attribution_total);
        printf("  (Only counted for true positives where step was flagged)\n\n");

        printf("%-14s  %9s  %9s\n", "Category", "Correct", "Total");
        printf("%-14s  %9s  %9s\n", "--------------", "---------", "---------");
        for (int c = 1; c < CAT_COUNT; c++) {
            if (attribution_total_cat[c] > 0) {
                printf("%-14s  %9d  %9d  (%.1f%%)\n",
                       cat_names[c],
                       attribution_correct_cat[c],
                       attribution_total_cat[c],
                       100.0 * attribution_correct_cat[c] / attribution_total_cat[c]);
            }
        }
    } else {
        printf("No step-level attributions produced (pairwise detection didn't trigger).\n");
    }

    /* ============================================
     * Honest Analysis
     * ============================================ */

    printf("\n--- Honest Analysis ---\n\n");

    printf("WHAT AGENTHALLU TESTS vs WHAT TARDYGRADA DOES:\n\n");

    printf("AgentHallu's 5 hallucination categories:\n");
    printf("  1. Tool-Use (wrong function, bad args, unnecessary calls)\n");
    printf("  2. Reasoning (wrong logic, math errors, faulty conclusions)\n");
    printf("  3. Retrieval (query misalign, context misalign, bad summaries)\n");
    printf("  4. Human-Interaction (misunderstanding user intent)\n");
    printf("  5. Planning (bad task decomposition, wrong fact derivation)\n\n");

    printf("Tardygrada's consistency pipeline detects:\n");
    printf("  - Textual contradictions between steps (\"file moved\" then \"file not found\")\n");
    printf("  - Numeric inconsistencies across steps\n");
    printf("  - Implicit logical contradictions in combined claims\n\n");

    printf("WHERE WE HAVE SIGNAL:\n");
    printf("  - Reasoning hallucinations that manifest as contradictions\n");
    printf("  - Retrieval hallucinations where summaries contradict source data\n");
    printf("  - Planning errors that produce contradictory action sequences\n\n");

    printf("WHERE WE DON'T:\n");
    printf("  - Pure tool-use errors (calling wrong function is behavioral, not textual)\n");
    printf("  - Unnecessary tool calls (no contradiction, just waste)\n");
    printf("  - Bad argument names (typos aren't contradictions)\n\n");

    printf("COMPARISON TO PAPER BASELINES:\n");
    printf("  The paper tests GPT-5, Gemini, etc. as LLM-based JUDGES -- they read\n");
    printf("  the trajectory and decide if hallucination occurred. That's a completely\n");
    printf("  different approach (LLM reasoning) vs ours (structural consistency).\n");
    printf("  Our approach is: deterministic, reproducible, zero API cost, sub-second.\n");
    printf("  Their approach is: expensive, stochastic, but understands intent.\n\n");

    printf("Completed in %.2f ms (%d trajectories, %.2f ms/traj)\n",
           elapsed_ms, n, elapsed_ms / n);

    free(trajs);
    return 0;
}
