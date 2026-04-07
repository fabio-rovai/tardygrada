/*
 * Tardygrada — LLM Grounding Client
 *
 * When a triple is UNKNOWN after checking the Datalog KB and ontology bridge,
 * ask an external LLM via Unix socket. Cache the answer as a Datalog fact.
 *
 * The LLM is external — runs as a process listening on a socket.
 * Tardygrada connects, sends JSON, gets JSON. Zero library dependencies.
 *
 * Enabled by TARDY_LLM_GROUND=1. Default off (stays deterministic).
 */

#ifndef TARDY_LLM_GROUND_H
#define TARDY_LLM_GROUND_H

#include <stdbool.h>

#define TARDY_LLM_SOCKET "/tmp/tardygrada-llm.sock"

typedef struct {
    int  fd;
    bool connected;
    char buf[8192];
} tardy_llm_conn_t;

typedef struct {
    bool  grounded;        /* true if LLM says the claim is factual */
    float confidence;      /* 0.0-1.0 */
    char  explanation[256]; /* why */
} tardy_llm_ground_result_t;

/* Check if LLM grounding is enabled (TARDY_LLM_GROUND=1) */
bool tardy_llm_ground_enabled(void);

/* Connect to LLM grounding service. Returns 0 on success, -1 on failure. */
int tardy_llm_connect(tardy_llm_conn_t *conn);

/* Disconnect from LLM grounding service. */
void tardy_llm_disconnect(tardy_llm_conn_t *conn);

/* Ground a single triple via LLM.
 * Returns result with grounded=false and confidence=0 on connection failure. */
tardy_llm_ground_result_t tardy_llm_ground_triple(
    tardy_llm_conn_t *conn,
    const char *subject, const char *predicate, const char *object);

/* Ground a claim (full sentence) via LLM.
 * Returns result with grounded=false and confidence=0 on connection failure. */
tardy_llm_ground_result_t tardy_llm_ground_claim(
    tardy_llm_conn_t *conn,
    const char *claim);

#endif /* TARDY_LLM_GROUND_H */
