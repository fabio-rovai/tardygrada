/*
 * Tardygrada — Rule-Based Text Decomposer
 *
 * Breaks free text into (subject, predicate, object) triples
 * using pattern matching on common English sentence structures.
 * No LLM needed. Deterministic. Verifiable.
 */

#include "decompose.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Defined in vm/context.c */
extern tardy_uuid_t tardy_uuid_gen(void);

/* ============================================
 * Helpers
 * ============================================ */

/* Trim leading and trailing whitespace in-place into dst.
 * Returns length of trimmed string. */
static int trim_copy(char *dst, int dst_size, const char *src, int src_len)
{
    int start = 0;
    int end = src_len - 1;

    while (start <= end && isspace((unsigned char)src[start]))
        start++;
    while (end >= start && isspace((unsigned char)src[end]))
        end--;

    int len = end - start + 1;
    if (len <= 0) {
        dst[0] = '\0';
        return 0;
    }
    if (len >= dst_size)
        len = dst_size - 1;

    memcpy(dst, src + start, (size_t)len);
    dst[len] = '\0';
    return len;
}

/* Case-insensitive strstr */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0])
        return haystack;
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
        if (match)
            return p;
    }
    return NULL;
}

/* Try to extract a triple from a sentence using a keyword pattern.
 * Pattern: subject <keyword> object
 * Returns 1 if matched, 0 otherwise. */
static int try_pattern(const char *sent, int sent_len,
                        const char *keyword, const char *predicate,
                        tardy_triple_t *triple)
{
    const char *pos = ci_strstr(sent, keyword);
    if (!pos)
        return 0;

    int subj_len = (int)(pos - sent);
    int kw_len = (int)strlen(keyword);
    int obj_start = (int)(pos - sent) + kw_len;
    int obj_len = sent_len - obj_start;

    if (subj_len <= 0 || obj_len <= 0)
        return 0;

    trim_copy(triple->subject, TARDY_MAX_TRIPLE_LEN, sent, subj_len);
    trim_copy(triple->object, TARDY_MAX_TRIPLE_LEN,
              sent + obj_start, obj_len);
    strncpy(triple->predicate, predicate, TARDY_MAX_TRIPLE_LEN - 1);
    triple->predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';

    /* Skip empty results */
    if (triple->subject[0] == '\0' || triple->object[0] == '\0')
        return 0;

    return 1;
}

/* ============================================
 * Sentence Splitter
 * ============================================ */

/* Split mode determines which delimiters to use */
typedef enum {
    SPLIT_DOT_ONLY = 0,     /* '.' only */
    SPLIT_DOT_COMMA_AND,    /* '.', ',', 'and' */
    SPLIT_DOT_SEMI_COLON    /* '.', ';', ':' */
} split_mode_t;

/* Check if character is a sentence delimiter for the given mode */
static int is_delim(const char *text, int pos, int len, split_mode_t mode)
{
    char c = text[pos];

    /* All modes split on '.' */
    if (c == '.' || c == '!' || c == '?')
        return 1;

    if (mode == SPLIT_DOT_COMMA_AND) {
        if (c == ',')
            return 1;
        /* Check for " and " as a delimiter */
        if (pos + 4 < len &&
            text[pos] == ' ' &&
            tolower((unsigned char)text[pos + 1]) == 'a' &&
            tolower((unsigned char)text[pos + 2]) == 'n' &&
            tolower((unsigned char)text[pos + 3]) == 'd' &&
            text[pos + 4] == ' ')
            return 1;
    }

    if (mode == SPLIT_DOT_SEMI_COLON) {
        if (c == ';' || c == ':')
            return 1;
    }

    return 0;
}

/* Get delimiter length (most are 1, " and " is 5) */
static int delim_len(const char *text, int pos, int len, split_mode_t mode)
{
    if (mode == SPLIT_DOT_COMMA_AND && pos + 4 < len &&
        text[pos] == ' ' &&
        tolower((unsigned char)text[pos + 1]) == 'a' &&
        tolower((unsigned char)text[pos + 2]) == 'n' &&
        tolower((unsigned char)text[pos + 3]) == 'd' &&
        text[pos + 4] == ' ')
        return 5;
    return 1;
}

/* ============================================
 * Pattern Table
 * ============================================ */

typedef struct {
    const char *keyword;
    const char *predicate;
} pattern_t;

/* Ordered from most specific to least specific */
static const pattern_t patterns[] = {
    { " was created at ",  "created_at"  },
    { " was created by ",  "created_by"  },
    { " was created in ",  "created_in"  },
    { " is the ",          NULL          },  /* special: "X is the Y of Z" */
    { " is ",              "is"          },
    { " has ",             "has"         },
    { " are ",             "are"         },
    { " was ",             "was"         },
    { " in ",              "located_in"  },
};

#define PATTERN_COUNT ((int)(sizeof(patterns) / sizeof(patterns[0])))

/* Handle "X is the Y of Z" → (Z, has_Y, X) */
static int try_is_the_of(const char *sent, int sent_len,
                           tardy_triple_t *triple)
{
    const char *is_the = ci_strstr(sent, " is the ");
    if (!is_the)
        return 0;

    int subj_len = (int)(is_the - sent);
    const char *after_is_the = is_the + 8; /* len(" is the ") */
    const char *of_pos = ci_strstr(after_is_the, " of ");
    if (!of_pos)
        return 0;

    /* Y is between "is the" and "of" */
    int y_len = (int)(of_pos - after_is_the);
    const char *z_start = of_pos + 4; /* len(" of ") */
    int z_len = sent_len - (int)(z_start - sent);

    if (subj_len <= 0 || y_len <= 0 || z_len <= 0)
        return 0;

    char y_buf[TARDY_MAX_TRIPLE_LEN];
    trim_copy(y_buf, TARDY_MAX_TRIPLE_LEN, after_is_the, y_len);

    /* Build predicate: "has_Y" */
    char pred[TARDY_MAX_TRIPLE_LEN];
    snprintf(pred, TARDY_MAX_TRIPLE_LEN, "has_%s", y_buf);

    /* Triple: (Z, has_Y, X) */
    trim_copy(triple->subject, TARDY_MAX_TRIPLE_LEN, z_start, z_len);
    strncpy(triple->predicate, pred, TARDY_MAX_TRIPLE_LEN - 1);
    triple->predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
    trim_copy(triple->object, TARDY_MAX_TRIPLE_LEN, sent, subj_len);

    if (triple->subject[0] == '\0' || triple->object[0] == '\0')
        return 0;

    return 1;
}

/* Handle "The Y of X is Z" → (X, has_Y, Z) */
static int try_the_of_is(const char *sent, int sent_len,
                           tardy_triple_t *triple)
{
    /* Must start with "The " (case-insensitive) */
    if (sent_len < 4)
        return 0;
    if (tolower((unsigned char)sent[0]) != 't' ||
        tolower((unsigned char)sent[1]) != 'h' ||
        tolower((unsigned char)sent[2]) != 'e' ||
        sent[3] != ' ')
        return 0;

    const char *of_pos = ci_strstr(sent + 4, " of ");
    if (!of_pos)
        return 0;

    const char *is_pos = ci_strstr(of_pos + 4, " is ");
    if (!is_pos)
        return 0;

    /* Y: between "The " and " of " */
    int y_len = (int)(of_pos - (sent + 4));
    /* X: between " of " and " is " */
    const char *x_start = of_pos + 4;
    int x_len = (int)(is_pos - x_start);
    /* Z: after " is " */
    const char *z_start = is_pos + 4;
    int z_len = sent_len - (int)(z_start - sent);

    if (y_len <= 0 || x_len <= 0 || z_len <= 0)
        return 0;

    char y_buf[TARDY_MAX_TRIPLE_LEN];
    trim_copy(y_buf, TARDY_MAX_TRIPLE_LEN, sent + 4, y_len);

    char pred[TARDY_MAX_TRIPLE_LEN];
    snprintf(pred, TARDY_MAX_TRIPLE_LEN, "has_%s", y_buf);

    trim_copy(triple->subject, TARDY_MAX_TRIPLE_LEN, x_start, x_len);
    strncpy(triple->predicate, pred, TARDY_MAX_TRIPLE_LEN - 1);
    triple->predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
    trim_copy(triple->object, TARDY_MAX_TRIPLE_LEN, z_start, z_len);

    if (triple->subject[0] == '\0' || triple->object[0] == '\0')
        return 0;

    return 1;
}

/* ============================================
 * Core Decomposition
 * ============================================ */

static int decompose_sentence(const char *sent, int sent_len,
                               tardy_triple_t *triples, int max_triples)
{
    if (sent_len <= 0 || max_triples <= 0)
        return 0;

    int count = 0;

    /* Try "The Y of X is Z" first (most specific structural pattern) */
    if (count < max_triples && try_the_of_is(sent, sent_len, &triples[count]))
        count++;

    /* Try "X is the Y of Z" */
    if (count < max_triples && try_is_the_of(sent, sent_len, &triples[count]))
        count++;

    /* If we got a structural match, return it */
    if (count > 0)
        return count;

    /* Try remaining patterns */
    for (int p = 0; p < PATTERN_COUNT && count < max_triples; p++) {
        if (patterns[p].predicate == NULL)
            continue; /* skip special patterns handled above */
        if (try_pattern(sent, sent_len, patterns[p].keyword,
                        patterns[p].predicate, &triples[count]))
            count++;
    }

    return count;
}

/* ============================================
 * Public API
 * ============================================ */

int tardy_decompose(const char *text, int len,
                     tardy_triple_t *triples, int max_triples)
{
    if (!text || len <= 0 || !triples || max_triples <= 0)
        return 0;

    int count = 0;
    int sent_start = 0;

    /* Split on sentence boundaries and decompose each */
    for (int i = 0; i <= len; i++) {
        int at_end = (i == len);
        int at_delim = (!at_end && (text[i] == '.' ||
                                     text[i] == '!' ||
                                     text[i] == '?'));

        if (at_delim || at_end) {
            int sent_len = i - sent_start;
            if (sent_len > 0 && count < max_triples) {
                /* Trim the sentence */
                char sent[1024];
                int slen = trim_copy(sent, (int)sizeof(sent),
                                     text + sent_start, sent_len);
                if (slen > 0) {
                    int n = decompose_sentence(sent, slen,
                                                triples + count,
                                                max_triples - count);
                    count += n;
                }
            }
            sent_start = i + 1;
        }
    }

    /* Fallback: if no triples extracted, use whole text as one triple */
    if (count == 0 && max_triples > 0) {
        strncpy(triples[0].subject, "claim", TARDY_MAX_TRIPLE_LEN - 1);
        triples[0].subject[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
        strncpy(triples[0].predicate, "states", TARDY_MAX_TRIPLE_LEN - 1);
        triples[0].predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
        int copylen = len < TARDY_MAX_TRIPLE_LEN - 1 ?
                      len : TARDY_MAX_TRIPLE_LEN - 1;
        memcpy(triples[0].object, text, (size_t)copylen);
        triples[0].object[copylen] = '\0';
        count = 1;
    }

    return count;
}

/* Decompose with a specific split mode */
static int decompose_with_mode(const char *text, int len,
                                 tardy_triple_t *triples, int max_triples,
                                 split_mode_t mode)
{
    if (!text || len <= 0 || !triples || max_triples <= 0)
        return 0;

    /* For DOT_ONLY mode, just use the standard decompose */
    if (mode == SPLIT_DOT_ONLY)
        return tardy_decompose(text, len, triples, max_triples);

    int count = 0;
    int sent_start = 0;

    for (int i = 0; i <= len; i++) {
        int at_end = (i == len);
        int at_delim = (!at_end && is_delim(text, i, len, mode));

        if (at_delim || at_end) {
            int dl = at_delim ? delim_len(text, i, len, mode) : 0;
            int sent_len = i - sent_start;
            if (sent_len > 0 && count < max_triples) {
                char sent[1024];
                int slen = trim_copy(sent, (int)sizeof(sent),
                                     text + sent_start, sent_len);
                if (slen > 0) {
                    int n = decompose_sentence(sent, slen,
                                                triples + count,
                                                max_triples - count);
                    count += n;
                }
            }
            sent_start = i + (dl > 0 ? dl : 1);
            if (dl > 1)
                i += dl - 1; /* skip multi-char delimiters */
        }
    }

    /* Fallback */
    if (count == 0 && max_triples > 0) {
        strncpy(triples[0].subject, "claim", TARDY_MAX_TRIPLE_LEN - 1);
        triples[0].subject[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
        strncpy(triples[0].predicate, "states", TARDY_MAX_TRIPLE_LEN - 1);
        triples[0].predicate[TARDY_MAX_TRIPLE_LEN - 1] = '\0';
        int copylen = len < TARDY_MAX_TRIPLE_LEN - 1 ?
                      len : TARDY_MAX_TRIPLE_LEN - 1;
        memcpy(triples[0].object, text, (size_t)copylen);
        triples[0].object[copylen] = '\0';
        count = 1;
    }

    return count;
}

int tardy_decompose_multi(const char *text, int len,
                           tardy_decomposition_t *decomps, int count)
{
    if (!text || len <= 0 || !decomps || count <= 0)
        return 0;

    /* Pass 1: split on '.' only */
    if (count >= 1) {
        memset(&decomps[0], 0, sizeof(tardy_decomposition_t));
        decomps[0].count = decompose_with_mode(
            text, len, decomps[0].triples, TARDY_MAX_TRIPLES,
            SPLIT_DOT_ONLY);
        decomps[0].decomposer = tardy_uuid_gen();
    }

    /* Pass 2: split on '.', ',', 'and' */
    if (count >= 2) {
        memset(&decomps[1], 0, sizeof(tardy_decomposition_t));
        decomps[1].count = decompose_with_mode(
            text, len, decomps[1].triples, TARDY_MAX_TRIPLES,
            SPLIT_DOT_COMMA_AND);
        decomps[1].decomposer = tardy_uuid_gen();
    }

    /* Pass 3: split on '.', ';', ':' */
    if (count >= 3) {
        memset(&decomps[2], 0, sizeof(tardy_decomposition_t));
        decomps[2].count = decompose_with_mode(
            text, len, decomps[2].triples, TARDY_MAX_TRIPLES,
            SPLIT_DOT_SEMI_COLON);
        decomps[2].decomposer = tardy_uuid_gen();
    }

    /* Compute agreement: fraction of triples that appear in all passes */
    int actual = count < 3 ? count : 3;
    for (int i = 0; i < actual; i++) {
        if (actual <= 1) {
            decomps[i].agreement = 1.0f;
            continue;
        }
        int matching = 0;
        for (int t = 0; t < decomps[i].count; t++) {
            int found_in_all = 1;
            for (int j = 0; j < actual; j++) {
                if (j == i) continue;
                int found = 0;
                for (int k = 0; k < decomps[j].count; k++) {
                    if (strcmp(decomps[i].triples[t].subject,
                               decomps[j].triples[k].subject) == 0 &&
                        strcmp(decomps[i].triples[t].predicate,
                               decomps[j].triples[k].predicate) == 0 &&
                        strcmp(decomps[i].triples[t].object,
                               decomps[j].triples[k].object) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) { found_in_all = 0; break; }
            }
            if (found_in_all) matching++;
        }
        decomps[i].agreement = decomps[i].count > 0 ?
            (float)matching / (float)decomps[i].count : 0.0f;
    }

    return actual;
}
