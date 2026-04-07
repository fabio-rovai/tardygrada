/*
 * Tardygrada — Hierarchical Temporal Memory Palace
 * A structured fact store with temporal validity, superseding, and
 * contradiction detection. Pure C, zero deps, flat binary persistence.
 *
 * Inspired by MemPalace architecture: wing > room > fact hierarchy.
 */

#ifndef TARDY_PALACE_H
#define TARDY_PALACE_H

#include "../vm/types.h"
#include "../vm/crypto.h"

#include <stdint.h>
#include <stdbool.h>

/* ============================================
 * Limits
 * ============================================ */

#define TARDY_PALACE_MAX_WINGS   16
#define TARDY_PALACE_MAX_ROOMS   512
#define TARDY_PALACE_MAX_FACTS   8192

#define TARDY_PALACE_FACTS_PER_ROOM  (TARDY_PALACE_MAX_FACTS / TARDY_PALACE_MAX_ROOMS)
#define TARDY_PALACE_ROOMS_PER_WING  (TARDY_PALACE_MAX_ROOMS / TARDY_PALACE_MAX_WINGS)

/* Persistence */
#define TARDY_PALACE_MAGIC       0x54415244594D454DULL  /* "TARDYMEM" */
#define TARDY_PALACE_VERSION     1
#define TARDY_PALACE_DEFAULT_PATH "/tmp/tardygrada-palace.dat"

/* ============================================
 * Fact — a single temporal assertion
 * ============================================ */

typedef struct {
    char           subject[128];
    char           predicate[64];
    char           object[256];
    uint64_t       valid_from;      /* timestamp when fact became true */
    uint64_t       valid_to;        /* 0 = still valid, else when superseded */
    float          confidence;
    tardy_hash_t   hash;            /* integrity hash of subject+predicate+object */
    tardy_uuid_t   source_agent;    /* who asserted this */
} tardy_memory_fact_t;

/* ============================================
 * Room — a topic within a wing
 * ============================================ */

typedef struct {
    char                 name[64];   /* topic e.g. "budget", "timeline", "team" */
    tardy_memory_fact_t  facts[TARDY_PALACE_FACTS_PER_ROOM];
    int                  fact_count;
} tardy_room_t;

/* ============================================
 * Wing — a project/domain
 * ============================================ */

typedef struct {
    char           name[64];         /* domain e.g. "project-alpha" */
    tardy_room_t   rooms[TARDY_PALACE_ROOMS_PER_WING];
    int            room_count;
} tardy_wing_t;

/* ============================================
 * Palace — the top-level memory structure
 * ============================================ */

typedef struct {
    tardy_wing_t   wings[TARDY_PALACE_MAX_WINGS];
    int            wing_count;
    int            total_facts;
    char           db_path[256];
} tardy_palace_t;

/* ============================================
 * Init / Shutdown
 * ============================================ */

/* Initialize an empty palace */
void tardy_palace_init(tardy_palace_t *p);

/* Save palace to binary file. Uses p->db_path if path is NULL. */
void tardy_palace_save(tardy_palace_t *p, const char *path);

/* Load palace from binary file. Returns 0 on success, -1 on failure. */
int tardy_palace_load(tardy_palace_t *p, const char *path);

/* ============================================
 * Remember — store a fact
 * ============================================ */

/* Store a fact. Auto-creates wing/room if needed.
 * Handles temporal superseding: if same subject+predicate exists with
 * different object, old fact gets valid_to = now.
 * room may be NULL for auto-detection from predicate/subject keywords.
 * Returns 0 on success, -1 on failure (capacity). */
int tardy_palace_remember(tardy_palace_t *p,
    const char *wing, const char *room,
    const char *subject, const char *predicate, const char *object,
    float confidence, tardy_uuid_t source);

/* ============================================
 * Recall — query facts
 * ============================================ */

/* Recall facts matching query (substring match). Returns count written.
 * query NULL = all facts in that wing/room.
 * room NULL = all rooms in wing. */
int tardy_palace_recall(tardy_palace_t *p,
    const char *wing, const char *room,
    const char *query,
    tardy_memory_fact_t *out, int max_results);

/* Recall facts that were valid at a specific timestamp.
 * wing NULL = search all wings. */
int tardy_palace_recall_at(tardy_palace_t *p,
    const char *wing, uint64_t timestamp,
    tardy_memory_fact_t *out, int max_results);

/* ============================================
 * Consistency checking
 * ============================================ */

/* Check if a new fact contradicts existing current facts.
 * Returns: 0 = consistent, 1 = contradiction found (fills conflict_out).
 * Contradiction = same subject, same predicate, different object, still valid. */
int tardy_palace_check(tardy_palace_t *p,
    const char *subject, const char *predicate, const char *object,
    tardy_memory_fact_t *conflict_out);

/* ============================================
 * Stats
 * ============================================ */

/* Count facts in a wing/room. room NULL = all rooms. wing NULL = entire palace. */
int tardy_palace_count(tardy_palace_t *p, const char *wing, const char *room);

/* ============================================
 * NLP helpers (simple keyword extraction)
 * ============================================ */

/* Extract a subject from a natural language sentence.
 * Fills subject and predicate buffers from the sentence text. */
void tardy_palace_parse_sentence(const char *sentence,
    char *subject, int subject_size,
    char *predicate, int predicate_size,
    char *object, int object_size);

/* Derive a room name from predicate/subject keywords */
void tardy_palace_auto_room(const char *predicate, const char *subject,
    char *room, int room_size);

#endif /* TARDY_PALACE_H */
