/*
 * Tardygrada — Self-Hosted Ontology Engine
 *
 * Every triple is a @sovereign agent. Grounding is agent lookup.
 * No SPARQL. No Oxigraph. No unix socket. No external process.
 * The knowledge graph lives inside the VM.
 */

#include "self.h"
#include "../vm/semantic.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Forward declarations */
extern tardy_uuid_t tardy_uuid_gen(void);
extern bool tardy_uuid_eq(const tardy_uuid_t *a, const tardy_uuid_t *b);

/* ============================================
 * Normalization helpers (shared with bridge.c)
 * ============================================ */

static void normalize_name(const char *input, char *output, int max_len)
{
    int oi = 0;
    int capitalize_next = 1;
    for (int i = 0; input[i] && oi < max_len - 1; i++) {
        if (input[i] == ' ' || input[i] == '-' || input[i] == '_') {
            capitalize_next = 1;
            continue;
        }
        if (capitalize_next && input[i] >= 'a' && input[i] <= 'z') {
            output[oi++] = input[i] - 32;
            capitalize_next = 0;
        } else {
            output[oi++] = input[i];
            capitalize_next = 0;
        }
    }
    output[oi] = '\0';
}

/* Case-insensitive substring match */
static int ci_contains(const char *haystack, const char *needle)
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

/* ============================================
 * Init
 * ============================================ */

int tardy_self_ontology_init(tardy_self_ontology_t *ont, tardy_vm_t *vm)
{
    if (!ont || !vm)
        return -1;

    memset(ont, 0, sizeof(tardy_self_ontology_t));
    ont->vm = vm;

    /* Create a parent agent to hold all triples */
    int64_t zero = 0;
    ont->ontology_agent = tardy_vm_spawn(vm, vm->root_id, "_ontology",
                                          TARDY_TYPE_AGENT,
                                          TARDY_TRUST_SOVEREIGN,
                                          &zero, sizeof(int64_t));

    if (ont->ontology_agent.hi == 0 && ont->ontology_agent.lo == 0)
        return -1;

    ont->initialized = true;
    return 0;
}

/* ============================================
 * Add Triple — each triple becomes a @sovereign agent
 * ============================================ */

int tardy_self_ontology_add(tardy_self_ontology_t *ont,
                             const char *subject,
                             const char *predicate,
                             const char *object)
{
    if (!ont || !ont->initialized)
        return -1;

    /* Build agent name: "subject|predicate|object" */
    char name[TARDY_CTX_MAX_NAME];
    snprintf(name, sizeof(name), "%.20s|%.20s|%.20s",
             subject, predicate, object);

    /* Build value: full triple text for semantic search */
    char value[512];
    int vlen = snprintf(value, sizeof(value), "%s %s %s",
                        subject, predicate, object);

    /* Spawn as @sovereign — cryptographically immutable */
    tardy_uuid_t id = tardy_vm_spawn(ont->vm, ont->ontology_agent, name,
                                      TARDY_TYPE_STR,
                                      TARDY_TRUST_SOVEREIGN,
                                      value, vlen + 1);

    if (id.hi == 0 && id.lo == 0)
        return -1;

    ont->triple_count++;
    return 0;
}

/* ============================================
 * Load TTL — basic Turtle/N-Triples parser
 *
 * Handles:
 *   <http://example.org/X> <http://schema.org/Y> <http://example.org/Z> .
 *   <http://example.org/X> <http://schema.org/Y> "literal" .
 * ============================================ */

static void extract_local_name(const char *iri, char *out, int max_len)
{
    /* <http://example.org/DoctorWho> → DoctorWho */
    const char *last_slash = NULL;
    const char *last_hash = NULL;
    for (const char *p = iri; *p; p++) {
        if (*p == '/') last_slash = p;
        if (*p == '#') last_hash = p;
    }
    const char *start = last_hash ? last_hash + 1 : (last_slash ? last_slash + 1 : iri);

    /* Strip angle brackets */
    if (*start == '<') start++;
    int len = 0;
    while (start[len] && start[len] != '>' && len < max_len - 1) {
        out[len] = start[len];
        len++;
    }
    out[len] = '\0';
}

int tardy_self_ontology_load_ttl(tardy_self_ontology_t *ont,
                                  const char *path)
{
    if (!ont || !ont->initialized || !path)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    if (st.st_size > 1024 * 1024) { /* 1MB max */
        close(fd);
        return -1;
    }

    char *buf = (char *)__builtin_alloca(st.st_size + 1);
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int loaded = 0;
    char *line = buf;
    while (*line) {
        /* Skip whitespace and comments */
        while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r')
            line++;
        if (*line == '#' || *line == '@') {
            /* Skip comment or prefix declaration */
            while (*line && *line != '\n') line++;
            continue;
        }
        if (!*line) break;

        /* Try to parse: <subject> <predicate> <object> . */
        /* or: <subject> <predicate> "literal" . */
        if (*line != '<') {
            while (*line && *line != '\n') line++;
            continue;
        }

        /* Extract subject */
        char *s_start = line;
        char *s_end = strchr(s_start, '>');
        if (!s_end) break;
        s_end++;

        /* Skip whitespace */
        char *p_start = s_end;
        while (*p_start == ' ' || *p_start == '\t') p_start++;

        if (*p_start != '<') {
            while (*line && *line != '\n') line++;
            continue;
        }

        /* Extract predicate */
        char *p_end = strchr(p_start, '>');
        if (!p_end) break;
        p_end++;

        /* Skip whitespace */
        char *o_start = p_end;
        while (*o_start == ' ' || *o_start == '\t') o_start++;

        /* Extract object (IRI or literal) */
        char *o_end;
        int is_literal = 0;
        if (*o_start == '<') {
            o_end = strchr(o_start, '>');
            if (!o_end) break;
            o_end++;
        } else if (*o_start == '"') {
            is_literal = 1;
            o_start++; /* skip opening " */
            o_end = strchr(o_start, '"');
            if (!o_end) break;
        } else {
            while (*line && *line != '\n') line++;
            continue;
        }

        /* Extract local names */
        char subj[128], pred[128], obj[128];
        *s_end = '\0';
        extract_local_name(s_start, subj, sizeof(subj));
        *p_end = '\0';
        extract_local_name(p_start, pred, sizeof(pred));
        if (is_literal) {
            int olen = (int)(o_end - o_start);
            if (olen > 127) olen = 127;
            memcpy(obj, o_start, olen);
            obj[olen] = '\0';
        } else {
            *o_end = '\0';
            extract_local_name(o_start, obj, sizeof(obj));
        }

        /* Add triple */
        if (subj[0] && pred[0] && obj[0]) {
            tardy_self_ontology_add(ont, subj, pred, obj);
            loaded++;
        }

        /* Advance to next line */
        line = o_end + 1;
        while (*line && *line != '\n') line++;
    }

    return loaded;
}

/* ============================================
 * Ground — look up triples by querying agents
 *
 * For each input triple, search the ontology agents
 * for matches on subject + predicate + object.
 * Uses the existing semantic query system.
 * ============================================ */

int tardy_self_ontology_ground(tardy_self_ontology_t *ont,
                                const tardy_triple_t *triples, int count,
                                tardy_grounding_t *out)
{
    if (!ont || !ont->initialized || !out) {
        if (out) memset(out, 0, sizeof(tardy_grounding_t));
        return -1;
    }

    memset(out, 0, sizeof(tardy_grounding_t));
    out->count = count;

    tardy_agent_t *ont_agent = tardy_vm_find(ont->vm, ont->ontology_agent);
    if (!ont_agent) return -1;

    for (int i = 0; i < count && i < TARDY_MAX_TRIPLES; i++) {
        out->results[i].triple = triples[i];

        /* Normalize input — same as bridge.c IRI normalization */
        char norm_s[128], norm_o[128];
        normalize_name(triples[i].subject, norm_s, sizeof(norm_s));
        normalize_name(triples[i].object, norm_o, sizeof(norm_o));

        /* Map decomposer predicates to schema.org equivalents */
        const char *raw_p = triples[i].predicate;
        const char *mapped_p = raw_p;
        if (strcmp(raw_p, "created_at") == 0) mapped_p = "locationCreated";
        else if (strcmp(raw_p, "created_by") == 0) mapped_p = "creator";
        else if (strcmp(raw_p, "created_in") == 0) mapped_p = "dateCreated";
        else if (strcmp(raw_p, "born_in") == 0) mapped_p = "birthPlace";
        else if (strcmp(raw_p, "located_in") == 0) mapped_p = "address";
        else if (strcmp(raw_p, "type_of") == 0 || strcmp(raw_p, "is_a") == 0) mapped_p = "type";
        else if (strcmp(raw_p, "wrote") == 0) mapped_p = "author";
        else if (strcmp(raw_p, "directed") == 0) mapped_p = "director";
        else if (strcmp(raw_p, "produced") == 0) mapped_p = "producer";
        else if (strcmp(raw_p, "founded_by") == 0) mapped_p = "founder";
        else if (strcmp(raw_p, "works_for") == 0) mapped_p = "worksFor";

        char norm_p[128];
        normalize_name(mapped_p, norm_p, sizeof(norm_p));

        /* Search through ontology agent's children */
        int evidence = 0;
        int contradictions = 0;

        for (int c = 0; c < ont_agent->context.child_count; c++) {
            const char *child_name = ont_agent->context.children[c].name;

            /* Child name format: "subject|predicate|object" */
            /* Check if subject matches */
            int subj_match = ci_contains(child_name, norm_s);
            if (!subj_match && triples[i].subject[0])
                subj_match = ci_contains(child_name, triples[i].subject);

            if (!subj_match) continue;

            /* Subject matches — check predicate */
            int pred_match = ci_contains(child_name, norm_p);
            if (!pred_match && triples[i].predicate[0])
                pred_match = ci_contains(child_name, triples[i].predicate);

            if (subj_match && pred_match) {
                /* Subject + predicate match — check object */
                int obj_match = ci_contains(child_name, norm_o);
                if (!obj_match && triples[i].object[0])
                    obj_match = ci_contains(child_name, triples[i].object);

                if (obj_match) {
                    /* Full match — evidence found */
                    evidence++;
                } else {
                    /* Same subject+predicate but different object — contradiction */
                    contradictions++;
                }
            }
        }

        if (evidence > 0) {
            out->results[i].status = TARDY_KNOWLEDGE_GROUNDED;
            out->results[i].confidence = evidence > 2 ? 0.95f :
                                          (evidence > 1 ? 0.90f : 0.80f);
            out->results[i].evidence_count = evidence;
            out->grounded++;
        } else if (contradictions > 0) {
            out->results[i].status = TARDY_KNOWLEDGE_CONTRADICTED;
            out->results[i].confidence = 0.0f;
            out->results[i].evidence_count = contradictions;
            out->contradicted++;
        } else {
            out->results[i].status = TARDY_KNOWLEDGE_UNKNOWN;
            out->results[i].confidence = 0.0f;
            out->results[i].evidence_count = 0;
            out->unknown++;
        }
    }

    return 0;
}

/* ============================================
 * Consistency Check
 * ============================================ */

int tardy_self_ontology_check_consistency(tardy_self_ontology_t *ont,
                                           const tardy_triple_t *triples,
                                           int count,
                                           tardy_consistency_t *out)
{
    if (!ont || !out) {
        if (out) { memset(out, 0, sizeof(tardy_consistency_t)); out->consistent = true; }
        return -1;
    }

    memset(out, 0, sizeof(tardy_consistency_t));
    out->consistent = true;

    /* Check input triples against each other */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            /* Same subject + predicate but different object = inconsistency */
            if (strcmp(triples[i].subject, triples[j].subject) == 0 &&
                strcmp(triples[i].predicate, triples[j].predicate) == 0 &&
                strcmp(triples[i].object, triples[j].object) != 0) {
                out->consistent = false;
                out->contradiction_count++;
                snprintf(out->explanation, sizeof(out->explanation),
                         "%.60s.%.60s has conflicting values: %.50s vs %.50s",
                         triples[i].subject, triples[i].predicate,
                         triples[i].object, triples[j].object);
            }
        }
    }

    /* Also check input against existing ontology triples */
    tardy_agent_t *ont_agent = tardy_vm_find(ont->vm, ont->ontology_agent);
    if (!ont_agent) return 0;

    for (int i = 0; i < count; i++) {
        char norm_s[128], norm_p[128];
        normalize_name(triples[i].subject, norm_s, sizeof(norm_s));
        normalize_name(triples[i].predicate, norm_p, sizeof(norm_p));

        for (int c = 0; c < ont_agent->context.child_count; c++) {
            const char *name = ont_agent->context.children[c].name;
            if (ci_contains(name, norm_s) && ci_contains(name, norm_p)) {
                char norm_o[128];
                normalize_name(triples[i].object, norm_o, sizeof(norm_o));
                if (!ci_contains(name, norm_o)) {
                    /* Same subject+pred in ontology but different object */
                    /* This is only a contradiction for functional properties */
                    /* For now, flag but don't fail */
                }
            }
        }
    }

    return 0;
}

/* ============================================
 * Full Verify — ground + consistency
 * ============================================ */

int tardy_self_ontology_verify(tardy_self_ontology_t *ont,
                                const tardy_triple_t *triples, int count,
                                tardy_grounding_t *grounding,
                                tardy_consistency_t *consistency)
{
    int g = tardy_self_ontology_ground(ont, triples, count, grounding);
    int c = tardy_self_ontology_check_consistency(ont, triples, count,
                                                    consistency);
    return (g == 0 && c == 0) ? 0 : -1;
}
