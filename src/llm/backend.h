/*
 * Tardygrada — LLM Backend
 *
 * Pluggable interface for LLM conversations.
 * An agent's body IS a conversation with a model.
 * The VM wraps it with verification.
 *
 * Backends:
 *   - Stub: deterministic responses for testing
 *   - HTTP: talks to Anthropic/OpenAI APIs via subprocess curl
 *   - Local: talks to local model via unix socket (ollama, llama.cpp)
 *
 * The backend doesn't matter. What matters is:
 * the response goes through the verification pipeline before
 * it can become a Fact.
 */

#ifndef TARDY_LLM_BACKEND_H
#define TARDY_LLM_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#define TARDY_LLM_MAX_RESPONSE 4096
#define TARDY_LLM_MAX_PROMPT   4096

/* ============================================
 * LLM Response — what comes back from a conversation
 * This is UNTRUSTED. It's a mutable agent.
 * Must go through verification before it can be frozen.
 * ============================================ */

typedef struct {
    char    text[TARDY_LLM_MAX_RESPONSE];
    int     text_len;
    char    model[64];           /* which model responded */
    float   temperature;         /* what temperature was used */
    int     tokens_in;           /* prompt tokens consumed */
    int     tokens_out;          /* response tokens generated */
    bool    success;
    char    error[256];
} tardy_llm_response_t;

/* ============================================
 * LLM Backend — pluggable provider
 * ============================================ */

typedef enum {
    TARDY_LLM_STUB,     /* deterministic test responses */
    TARDY_LLM_ANTHROPIC, /* Anthropic Claude API */
    TARDY_LLM_OPENAI,   /* OpenAI API */
    TARDY_LLM_LOCAL,    /* local model (ollama/llama.cpp) */
} tardy_llm_provider_t;

typedef struct {
    tardy_llm_provider_t provider;
    char                 api_key[256];
    char                 model[64];
    char                 base_url[256];
    float                temperature;
    int                  max_tokens;
} tardy_llm_config_t;

typedef struct {
    tardy_llm_config_t config;
    bool               initialized;
} tardy_llm_backend_t;

/* ============================================
 * Backend API
 * ============================================ */

/* Initialize with config */
int tardy_llm_init(tardy_llm_backend_t *backend,
                    const tardy_llm_config_t *config);

/* Send a prompt, get a response.
 * This is the `ask()` primitive in Tardygrada.
 * The response is UNTRUSTED — must be verified before freeze. */
int tardy_llm_ask(tardy_llm_backend_t *backend,
                   const char *system_prompt,
                   const char *user_prompt,
                   tardy_llm_response_t *response);

/* Cleanup */
void tardy_llm_shutdown(tardy_llm_backend_t *backend);

/* Default stub config — for testing without API keys */
tardy_llm_config_t tardy_llm_stub_config(void);

/* Anthropic config from environment */
tardy_llm_config_t tardy_llm_anthropic_config(void);

#endif /* TARDY_LLM_BACKEND_H */
