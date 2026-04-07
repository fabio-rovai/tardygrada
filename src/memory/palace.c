/*
 * Tardygrada — Hierarchical Temporal Memory Palace
 * Implementation: pure C arrays, substring matching, binary persistence.
 * No SQLite, no ChromaDB, no semantic search. Just structured facts.
 */

#include "palace.h"
#include "../vm/util.h"
#include "../vm/crypto.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

/* ============================================
 * Timestamp helper — epoch seconds
 * ============================================ */

static uint64_t now_ts(void)
{
    return (uint64_t)time(NULL);
}

/* ============================================
 * Case-insensitive substring search
 * ============================================ */

static int ci_contains(const char *haystack, const char *needle)
{
    if (!needle || !needle[0]) return 1;
    if (!haystack || !haystack[0]) return 0;

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;

    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* ============================================
 * Compute integrity hash for a fact
 * ============================================ */

static void compute_fact_hash(tardy_memory_fact_t *f)
{
    /* Hash subject + predicate + object concatenated */
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "%s|%s|%s",
                       f->subject, f->predicate, f->object);
    tardy_sha256(buf, (size_t)len, &f->hash);
}

/* ============================================
 * Find or create wing
 * ============================================ */

static tardy_wing_t *find_wing(tardy_palace_t *p, const char *name)
{
    for (int i = 0; i < p->wing_count; i++) {
        if (strcmp(p->wings[i].name, name) == 0)
            return &p->wings[i];
    }
    return NULL;
}

static tardy_wing_t *get_or_create_wing(tardy_palace_t *p, const char *name)
{
    tardy_wing_t *w = find_wing(p, name);
    if (w) return w;

    if (p->wing_count >= TARDY_PALACE_MAX_WINGS) return NULL;

    w = &p->wings[p->wing_count++];
    memset(w, 0, sizeof(*w));
    strncpy(w->name, name, sizeof(w->name) - 1);
    return w;
}

/* ============================================
 * Find or create room within a wing
 * ============================================ */

static tardy_room_t *find_room(tardy_wing_t *w, const char *name)
{
    for (int i = 0; i < w->room_count; i++) {
        if (strcmp(w->rooms[i].name, name) == 0)
            return &w->rooms[i];
    }
    return NULL;
}

static tardy_room_t *get_or_create_room(tardy_wing_t *w, const char *name)
{
    tardy_room_t *r = find_room(w, name);
    if (r) return r;

    if (w->room_count >= TARDY_PALACE_ROOMS_PER_WING) return NULL;

    r = &w->rooms[w->room_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    return r;
}

/* ============================================
 * NLP: parse a natural language sentence into SPO
 * Simple heuristic: look for patterns like
 *   "The <subject> has/is/was <object>"
 *   "<subject> <predicate> <object>"
 * ============================================ */

void tardy_palace_parse_sentence(const char *sentence,
    char *subject, int subject_size,
    char *predicate, int predicate_size,
    char *object, int object_size)
{
    /* Default: entire sentence as object, "state" as predicate */
    subject[0] = '\0';
    strncpy(predicate, "state", predicate_size - 1);
    predicate[predicate_size - 1] = '\0';
    strncpy(object, sentence, object_size - 1);
    object[object_size - 1] = '\0';

    /* Skip leading "The " */
    const char *p = sentence;
    if (strncasecmp(p, "the ", 4) == 0) p += 4;

    /* Look for common verbs: has, is, was, are, were, have, had */
    static const char *verbs[] = {
        " has ", " is ", " was ", " are ", " were ",
        " have ", " had ", " will be ", " equals ",
        NULL
    };

    for (int v = 0; verbs[v]; v++) {
        const char *found = NULL;
        /* Case-insensitive search for verb */
        size_t vlen = strlen(verbs[v]);
        size_t plen = strlen(p);
        for (size_t i = 0; i + vlen <= plen; i++) {
            int match = 1;
            for (size_t j = 0; j < vlen; j++) {
                if (tolower((unsigned char)p[i + j]) !=
                    tolower((unsigned char)verbs[v][j])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                found = p + i;
                break;
            }
        }

        if (found) {
            /* Subject = text before verb */
            int slen = (int)(found - p);
            if (slen >= subject_size) slen = subject_size - 1;
            memcpy(subject, p, slen);
            subject[slen] = '\0';

            /* Predicate = the verb (trimmed) */
            const char *vstart = verbs[v];
            while (*vstart == ' ') vstart++;
            int pvlen = (int)strlen(vstart);
            while (pvlen > 0 && vstart[pvlen - 1] == ' ') pvlen--;
            if (pvlen >= predicate_size) pvlen = predicate_size - 1;
            memcpy(predicate, vstart, pvlen);
            predicate[pvlen] = '\0';

            /* Object = text after verb */
            const char *ostart = found + vlen;
            strncpy(object, ostart, object_size - 1);
            object[object_size - 1] = '\0';
            return;
        }
    }
}

/* ============================================
 * Auto-room: derive room name from keywords
 * ============================================ */

void tardy_palace_auto_room(const char *predicate, const char *subject,
    char *room, int room_size)
{
    /* Keyword -> room mapping */
    static const struct { const char *keyword; const char *room_name; } map[] = {
        { "budget",     "budget" },
        { "cost",       "budget" },
        { "price",      "budget" },
        { "money",      "budget" },
        { "fund",       "budget" },
        { "spend",      "budget" },
        { "team",       "team" },
        { "member",     "team" },
        { "staff",      "team" },
        { "people",     "team" },
        { "employee",   "team" },
        { "headcount",  "team" },
        { "deadline",   "timeline" },
        { "timeline",   "timeline" },
        { "date",       "timeline" },
        { "schedule",   "timeline" },
        { "milestone",  "timeline" },
        { "due",        "timeline" },
        { "deliver",    "timeline" },
        { "complet",    "timeline" },
        { "delay",      "timeline" },
        { "scope",      "scope" },
        { "require",    "scope" },
        { "feature",    "scope" },
        { "spec",       "scope" },
        { "risk",       "risk" },
        { "issue",      "risk" },
        { "problem",    "risk" },
        { "status",     "status" },
        { "progress",   "status" },
        { "phase",      "status" },
        { NULL, NULL }
    };

    /* Check subject and predicate against keywords */
    for (int i = 0; map[i].keyword; i++) {
        if (ci_contains(subject, map[i].keyword) ||
            ci_contains(predicate, map[i].keyword)) {
            strncpy(room, map[i].room_name, room_size - 1);
            room[room_size - 1] = '\0';
            return;
        }
    }

    /* Fallback: use first word of subject, lowercased */
    int j = 0;
    for (int i = 0; subject[i] && subject[i] != ' ' && j < room_size - 1; i++) {
        room[j++] = (char)tolower((unsigned char)subject[i]);
    }
    if (j == 0) {
        strncpy(room, "general", room_size - 1);
        room[room_size - 1] = '\0';
    } else {
        room[j] = '\0';
    }
}

/* ============================================
 * Init
 * ============================================ */

void tardy_palace_init(tardy_palace_t *p)
{
    memset(p, 0, sizeof(*p));
    strncpy(p->db_path, TARDY_PALACE_DEFAULT_PATH, sizeof(p->db_path) - 1);
}

/* ============================================
 * Remember — store a fact with temporal handling
 * ============================================ */

int tardy_palace_remember(tardy_palace_t *p,
    const char *wing_name, const char *room_name,
    const char *subject, const char *predicate, const char *object,
    float confidence, tardy_uuid_t source)
{
    if (!wing_name || !wing_name[0]) return -1;
    if (!subject || !subject[0]) return -1;

    /* Get/create wing */
    tardy_wing_t *w = get_or_create_wing(p, wing_name);
    if (!w) return -1;

    /* Auto-detect room if not specified */
    char auto_room[64];
    if (!room_name || !room_name[0]) {
        tardy_palace_auto_room(predicate, subject, auto_room, sizeof(auto_room));
        room_name = auto_room;
    }

    /* Get/create room */
    tardy_room_t *r = get_or_create_room(w, room_name);
    if (!r) return -1;

    /* Temporal superseding: find existing current fact with same subject+predicate */
    uint64_t ts = now_ts();
    for (int i = 0; i < r->fact_count; i++) {
        tardy_memory_fact_t *f = &r->facts[i];
        if (f->valid_to == 0 &&
            strcmp(f->subject, subject) == 0 &&
            strcmp(f->predicate, predicate) == 0) {
            /* Same subject+predicate, different object = temporal update */
            if (strcmp(f->object, object) != 0) {
                f->valid_to = ts;  /* supersede, don't delete */
            } else {
                /* Exact duplicate — just update confidence */
                f->confidence = confidence;
                return 0;
            }
        }
    }

    /* Add new fact */
    if (r->fact_count >= TARDY_PALACE_FACTS_PER_ROOM) return -1;
    if (p->total_facts >= TARDY_PALACE_MAX_FACTS) return -1;

    tardy_memory_fact_t *nf = &r->facts[r->fact_count++];
    memset(nf, 0, sizeof(*nf));
    strncpy(nf->subject, subject, sizeof(nf->subject) - 1);
    strncpy(nf->predicate, predicate, sizeof(nf->predicate) - 1);
    strncpy(nf->object, object, sizeof(nf->object) - 1);
    nf->valid_from = ts;
    nf->valid_to = 0;
    nf->confidence = confidence;
    nf->source_agent = source;
    compute_fact_hash(nf);

    p->total_facts++;
    return 0;
}

/* ============================================
 * Recall — query facts
 * ============================================ */

static int fact_matches_query(const tardy_memory_fact_t *f, const char *query)
{
    if (!query || !query[0]) return 1;
    return ci_contains(f->subject, query) ||
           ci_contains(f->predicate, query) ||
           ci_contains(f->object, query);
}

int tardy_palace_recall(tardy_palace_t *p,
    const char *wing_name, const char *room_name,
    const char *query,
    tardy_memory_fact_t *out, int max_results)
{
    int count = 0;

    /* Find wing */
    tardy_wing_t *w = find_wing(p, wing_name);
    if (!w) return 0;

    /* Iterate rooms */
    for (int ri = 0; ri < w->room_count && count < max_results; ri++) {
        tardy_room_t *r = &w->rooms[ri];

        /* Filter by room if specified */
        if (room_name && room_name[0] && strcmp(r->name, room_name) != 0)
            continue;

        /* Current facts first (valid_to == 0), then historical */
        /* Pass 1: current facts */
        for (int fi = r->fact_count - 1; fi >= 0 && count < max_results; fi--) {
            if (r->facts[fi].valid_to == 0 &&
                fact_matches_query(&r->facts[fi], query)) {
                out[count++] = r->facts[fi];
            }
        }
        /* Pass 2: superseded facts */
        for (int fi = r->fact_count - 1; fi >= 0 && count < max_results; fi--) {
            if (r->facts[fi].valid_to != 0 &&
                fact_matches_query(&r->facts[fi], query)) {
                out[count++] = r->facts[fi];
            }
        }
    }

    return count;
}

/* ============================================
 * Recall at timestamp
 * ============================================ */

int tardy_palace_recall_at(tardy_palace_t *p,
    const char *wing_name, uint64_t timestamp,
    tardy_memory_fact_t *out, int max_results)
{
    int count = 0;

    int w_start = 0, w_end = p->wing_count;
    if (wing_name && wing_name[0]) {
        tardy_wing_t *w = find_wing(p, wing_name);
        if (!w) return 0;
        w_start = (int)(w - p->wings);
        w_end = w_start + 1;
    }

    for (int wi = w_start; wi < w_end && count < max_results; wi++) {
        tardy_wing_t *w = &p->wings[wi];
        for (int ri = 0; ri < w->room_count && count < max_results; ri++) {
            tardy_room_t *r = &w->rooms[ri];
            for (int fi = 0; fi < r->fact_count && count < max_results; fi++) {
                tardy_memory_fact_t *f = &r->facts[fi];
                if (f->valid_from <= timestamp &&
                    (f->valid_to == 0 || f->valid_to > timestamp)) {
                    out[count++] = *f;
                }
            }
        }
    }

    return count;
}

/* ============================================
 * Consistency check
 * ============================================ */

int tardy_palace_check(tardy_palace_t *p,
    const char *subject, const char *predicate, const char *object,
    tardy_memory_fact_t *conflict_out)
{
    for (int wi = 0; wi < p->wing_count; wi++) {
        tardy_wing_t *w = &p->wings[wi];
        for (int ri = 0; ri < w->room_count; ri++) {
            tardy_room_t *r = &w->rooms[ri];
            for (int fi = 0; fi < r->fact_count; fi++) {
                tardy_memory_fact_t *f = &r->facts[fi];
                /* Only check current facts */
                if (f->valid_to != 0) continue;

                /* Same subject+predicate, different object = contradiction */
                if (strcmp(f->subject, subject) == 0 &&
                    strcmp(f->predicate, predicate) == 0 &&
                    strcmp(f->object, object) != 0) {
                    if (conflict_out)
                        *conflict_out = *f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ============================================
 * Count
 * ============================================ */

int tardy_palace_count(tardy_palace_t *p, const char *wing_name,
                       const char *room_name)
{
    if (!wing_name || !wing_name[0])
        return p->total_facts;

    tardy_wing_t *w = find_wing(p, wing_name);
    if (!w) return 0;

    if (!room_name || !room_name[0]) {
        int total = 0;
        for (int ri = 0; ri < w->room_count; ri++)
            total += w->rooms[ri].fact_count;
        return total;
    }

    tardy_room_t *r = find_room(w, room_name);
    return r ? r->fact_count : 0;
}

/* ============================================
 * Persistence — binary save/load
 *
 * Format:
 *   [8] magic
 *   [4] version
 *   [4] wing_count
 *   [4] total_facts
 *   For each wing:
 *     [64] name
 *     [4]  room_count
 *     For each room:
 *       [64] name
 *       [4]  fact_count
 *       For each fact:
 *         tardy_memory_fact_t (fixed size, written raw)
 * ============================================ */

void tardy_palace_save(tardy_palace_t *p, const char *path)
{
    const char *fpath = path ? path : p->db_path;
    int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        tardy_eprint("[palace] save failed: cannot open ");
        tardy_eprint(fpath);
        tardy_eprint("\n");
        return;
    }

    /* Header */
    uint64_t magic = TARDY_PALACE_MAGIC;
    uint32_t version = TARDY_PALACE_VERSION;
    uint32_t wing_count = (uint32_t)p->wing_count;
    uint32_t total_facts = (uint32_t)p->total_facts;

    ssize_t w;
    w = write(fd, &magic, 8);        (void)w;
    w = write(fd, &version, 4);      (void)w;
    w = write(fd, &wing_count, 4);   (void)w;
    w = write(fd, &total_facts, 4);  (void)w;

    /* Wings */
    for (int wi = 0; wi < p->wing_count; wi++) {
        tardy_wing_t *wing = &p->wings[wi];
        w = write(fd, wing->name, 64);  (void)w;

        uint32_t room_count = (uint32_t)wing->room_count;
        w = write(fd, &room_count, 4);  (void)w;

        for (int ri = 0; ri < wing->room_count; ri++) {
            tardy_room_t *room = &wing->rooms[ri];
            w = write(fd, room->name, 64);  (void)w;

            uint32_t fact_count = (uint32_t)room->fact_count;
            w = write(fd, &fact_count, 4);  (void)w;

            for (int fi = 0; fi < room->fact_count; fi++) {
                w = write(fd, &room->facts[fi], sizeof(tardy_memory_fact_t));
                (void)w;
            }
        }
    }

    close(fd);

    char msg[512];
    int len = snprintf(msg, sizeof(msg),
        "[palace] saved %d facts to %s\n", p->total_facts, fpath);
    tardy_write(STDERR_FILENO, msg, len);
}

int tardy_palace_load(tardy_palace_t *p, const char *path)
{
    const char *fpath = path ? path : p->db_path;
    int fd = open(fpath, O_RDONLY);
    if (fd < 0) return -1;

    /* Header */
    uint64_t magic = 0;
    uint32_t version = 0;
    uint32_t wing_count = 0;
    uint32_t total_facts = 0;

    ssize_t r;
    r = read(fd, &magic, 8);
    if (r != 8 || magic != TARDY_PALACE_MAGIC) {
        close(fd);
        return -1;
    }
    r = read(fd, &version, 4);
    if (r != 4 || version != TARDY_PALACE_VERSION) {
        close(fd);
        return -1;
    }
    r = read(fd, &wing_count, 4);   (void)r;
    r = read(fd, &total_facts, 4);  (void)r;

    if (wing_count > TARDY_PALACE_MAX_WINGS) {
        close(fd);
        return -1;
    }

    /* Reset palace */
    memset(p->wings, 0, sizeof(p->wings));
    p->wing_count = (int)wing_count;
    p->total_facts = (int)total_facts;

    for (uint32_t wi = 0; wi < wing_count; wi++) {
        tardy_wing_t *wing = &p->wings[wi];
        r = read(fd, wing->name, 64);  (void)r;

        uint32_t room_count = 0;
        r = read(fd, &room_count, 4);  (void)r;
        if (room_count > TARDY_PALACE_ROOMS_PER_WING) {
            close(fd);
            return -1;
        }
        wing->room_count = (int)room_count;

        for (uint32_t ri = 0; ri < room_count; ri++) {
            tardy_room_t *room = &wing->rooms[ri];
            r = read(fd, room->name, 64);  (void)r;

            uint32_t fact_count = 0;
            r = read(fd, &fact_count, 4);  (void)r;
            if (fact_count > TARDY_PALACE_FACTS_PER_ROOM) {
                close(fd);
                return -1;
            }
            room->fact_count = (int)fact_count;

            for (uint32_t fi = 0; fi < fact_count; fi++) {
                r = read(fd, &room->facts[fi], sizeof(tardy_memory_fact_t));
                (void)r;
            }
        }
    }

    close(fd);

    char msg[512];
    int len = snprintf(msg, sizeof(msg),
        "[palace] loaded %d facts from %s\n", p->total_facts, fpath);
    tardy_write(STDERR_FILENO, msg, len);

    return 0;
}
