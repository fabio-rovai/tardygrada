/*
 * Tardygrada — Self-Hosted Ontology Engine
 *
 * The knowledge graph IS agents. No external process needed.
 * Each triple is a @sovereign agent. Grounding = semantic query.
 * Consistency = constitution invariants.
 *
 * Replaces the unix socket bridge with in-process agent lookup.
 * Same verification guarantees, zero latency, zero dependencies.
 */

#ifndef TARDY_ONTOLOGY_SELF_H
#define TARDY_ONTOLOGY_SELF_H

#include <stdint.h>
#include "../vm/vm.h"
#include "../verify/pipeline.h"
#include "datalog.h"
#include "frames.h"

/* ============================================
 * Self-Hosted Ontology — triples as agents
 * ============================================ */

/* Trust tiers — lighter than tardy_trust_t. Tracks where a fact entered
 * the ontology, so query results can surface provenance.
 *
 *   BUNDLED   — tests/wikidata_common.nt, hand-curated
 *   SOVEREIGN — tests/sovereign_ontology.nt, K_PROMOTE-reconfirmed
 *   LEARNED   — tests/learned_ontology.nt, freshly accepted via MCP
 *
 * NONE means "load happened before tier tracking was added," used for
 * the legacy load_ttl path that doesn't pass a tier.
 */
typedef enum {
    TARDY_TIER_NONE = 0,
    TARDY_TIER_BUNDLED = 1,
    TARDY_TIER_SOVEREIGN = 2,
    TARDY_TIER_LEARNED = 3
} tardy_tier_t;

#define TARDY_TIER_KEY_MAX 80
#define TARDY_DATE_MAX 16   /* ISO 8601 date "YYYY-MM-DD" + null + slack */

typedef struct {
    char    key[TARDY_TIER_KEY_MAX];   /* "s|p|o" — matches agent name */
    uint8_t tier;                       /* tardy_tier_t value */
} tardy_tier_entry_t;

/* Per-fact validity interval. Empty since="" means "valid from forever
 * ago"; empty until="" means "valid forever". ISO dates are stored as
 * strings so comparisons are plain strcmp, no calendar math. */
typedef struct {
    char key[TARDY_TIER_KEY_MAX];
    char since[TARDY_DATE_MAX];
    char until[TARDY_DATE_MAX];
} tardy_validity_entry_t;

typedef struct {
    tardy_vm_t              *vm;
    tardy_uuid_t             ontology_agent;  /* parent agent holding all triples */
    int                      triple_count;
    bool                     initialized;
    tardy_dl_program_t       datalog;         /* Datalog inference engine */
    tardy_frame_registry_t   frames;          /* Frame schemas + CRDT merge */
    /* Per-fact tier sidecar. Sized to datalog's fact capacity so we can
     * tag every Datalog fact, including those introduced by submit_fact
     * after startup. Linear scan on lookup — fine at n<<4096. */
    tardy_tier_entry_t       tier_map[TARDY_DL_MAX_FACTS];
    int                      tier_count;
    uint8_t                  current_tier;    /* set by load_ttl_with_tier */
    /* Per-fact validity sidecar. Sized smaller than tier_map (most
     * facts have no time component). */
    tardy_validity_entry_t   validity_map[TARDY_DL_MAX_FACTS];
    int                      validity_count;
} tardy_self_ontology_t;

/* Initialize self-hosted ontology within a VM */
int tardy_self_ontology_init(tardy_self_ontology_t *ont, tardy_vm_t *vm);

/* Load a triple as a @sovereign agent.
 * Format: "subject predicate object" stored as agent name "s|p|o"
 * The value is the full triple text for semantic search. */
int tardy_self_ontology_add(tardy_self_ontology_t *ont,
                             const char *subject,
                             const char *predicate,
                             const char *object);

/* Load triples from a Turtle (.ttl) file.
 * Parses basic N-Triples/Turtle subset. */
int tardy_self_ontology_load_ttl(tardy_self_ontology_t *ont,
                                  const char *path);

/* Load triples and tag every loaded fact with the given tier.
 * Sets ont->current_tier for the duration of the load so each call into
 * tardy_self_ontology_add records the fact's provenance, then clears it. */
int tardy_self_ontology_load_ttl_with_tier(tardy_self_ontology_t *ont,
                                            const char *path,
                                            tardy_tier_t tier);

/* Look up the tier for a (subject, predicate, object) triple.
 * Returns TARDY_TIER_NONE if the fact was never recorded with a tier
 * (e.g. derived facts, or loads via legacy load_ttl). */
tardy_tier_t tardy_self_ontology_get_tier(const tardy_self_ontology_t *ont,
                                           const char *subject,
                                           const char *predicate,
                                           const char *object);

/* Human-readable tier name (for JSON output). */
const char *tardy_tier_name(tardy_tier_t tier);

/* Attach a validity interval to a fact. Empty strings mean unbounded
 * on that side. Quietly no-ops if the fact key isn't already present
 * — callers should call this AFTER tardy_self_ontology_add. */
int tardy_self_ontology_set_validity(tardy_self_ontology_t *ont,
                                      const char *subject,
                                      const char *predicate,
                                      const char *object,
                                      const char *since,
                                      const char *until);

/* Look up validity for a triple. Out args are nullable. Returns 1 if a
 * record was found, 0 otherwise. Empty strings indicate unbounded. */
int tardy_self_ontology_get_validity(const tardy_self_ontology_t *ont,
                                      const char *subject,
                                      const char *predicate,
                                      const char *object,
                                      char *since_out, int since_max,
                                      char *until_out, int until_max);

/* Test if a triple is valid at the given ISO date. Returns 1 if valid
 * (or if no validity record exists — facts with no time annotation are
 * "always valid"), 0 if expressly outside. valid_at "" or NULL means
 * "no constraint". */
int tardy_self_ontology_valid_at(const tardy_self_ontology_t *ont,
                                  const char *subject,
                                  const char *predicate,
                                  const char *object,
                                  const char *valid_at);

/* Ground triples against the self-hosted ontology.
 * Same interface as the bridge — drop-in replacement. */
int tardy_self_ontology_ground(tardy_self_ontology_t *ont,
                                const tardy_triple_t *triples, int count,
                                tardy_grounding_t *out);

/* Check consistency among triples.
 * Detects: same subject+predicate with different objects. */
int tardy_self_ontology_check_consistency(tardy_self_ontology_t *ont,
                                           const tardy_triple_t *triples,
                                           int count,
                                           tardy_consistency_t *out);

/* Full verify: ground + consistency in one call */
int tardy_self_ontology_verify(tardy_self_ontology_t *ont,
                                const tardy_triple_t *triples, int count,
                                tardy_grounding_t *grounding,
                                tardy_consistency_t *consistency);

#endif /* TARDY_ONTOLOGY_SELF_H */
