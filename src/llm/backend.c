/*
 * Tardygrada — LLM Backend Implementation
 *
 * Stub: returns deterministic responses (for testing).
 * Anthropic: shells out to curl to call Claude API.
 * No HTTP library dependency — just fork/exec curl.
 */

#include "backend.h"
#include "../mcp/json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* ============================================
 * Stub Backend — deterministic test responses
 * ============================================ */

static int stub_ask(const char *prompt, tardy_llm_response_t *resp)
{
    memset(resp, 0, sizeof(tardy_llm_response_t));
    resp->success = true;
    strncpy(resp->model, "stub-v1", sizeof(resp->model));
    resp->temperature = 0.0f;

    /* Known test prompts → deterministic responses */
    if (strstr(prompt, "Dr Who") || strstr(prompt, "Doctor Who")) {
        strncpy(resp->text,
                "Doctor Who was created at BBC Television Centre in London in 1963 by Sydney Newman.",
                TARDY_LLM_MAX_RESPONSE);
    } else if (strstr(prompt, "hello") || strstr(prompt, "Hello")) {
        strncpy(resp->text, "Hello! I am a Tardygrada agent.",
                TARDY_LLM_MAX_RESPONSE);
    } else if (strstr(prompt, "capital") && strstr(prompt, "France")) {
        strncpy(resp->text, "The capital of France is Paris.",
                TARDY_LLM_MAX_RESPONSE);
    } else {
        /* Default: echo the prompt back with a prefix */
        snprintf(resp->text, TARDY_LLM_MAX_RESPONSE,
                 "Response to: %s", prompt);
    }

    resp->text_len = (int)strlen(resp->text);
    resp->tokens_in = (int)strlen(prompt) / 4;
    resp->tokens_out = resp->text_len / 4;

    return 0;
}

/* ============================================
 * Anthropic Backend — Claude API via curl
 *
 * No HTTP library. We fork/exec curl.
 * The only external dependency is curl being installed.
 * ============================================ */

static int anthropic_ask(const tardy_llm_config_t *config,
                          const char *system_prompt,
                          const char *user_prompt,
                          tardy_llm_response_t *resp)
{
    memset(resp, 0, sizeof(tardy_llm_response_t));

    if (!config->api_key[0]) {
        strncpy(resp->error, "ANTHROPIC_API_KEY not set",
                sizeof(resp->error));
        resp->success = false;
        return -1;
    }

    /* Build JSON body */
    char body[TARDY_LLM_MAX_PROMPT * 2];
    int blen = snprintf(body, sizeof(body),
        "{\"model\":\"%s\","
        "\"max_tokens\":%d,"
        "\"temperature\":%.1f,"
        "\"system\":\"%s\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
        config->model[0] ? config->model : "claude-sonnet-4-20250514",
        config->max_tokens > 0 ? config->max_tokens : 1024,
        config->temperature,
        system_prompt ? system_prompt : "You are a precise factual assistant.",
        user_prompt);

    if (blen <= 0) {
        resp->success = false;
        return -1;
    }

    /* Fork + exec curl */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        resp->success = false;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        resp->success = false;
        return -1;
    }

    if (pid == 0) {
        /* Child: exec curl, stdout → pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", 0);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char auth[300];
        snprintf(auth, sizeof(auth), "x-api-key: %s", config->api_key);

        char url[300];
        snprintf(url, sizeof(url), "%s/v1/messages",
                 config->base_url[0] ? config->base_url :
                 "https://api.anthropic.com");

        execlp("curl", "curl", "-s",
               "-X", "POST", url,
               "-H", "Content-Type: application/json",
               "-H", "anthropic-version: 2023-06-01",
               "-H", auth,
               "-d", body,
               (char *)NULL);
        _exit(1);
    }

    /* Parent: read response from pipe */
    close(pipefd[1]);

    char curl_output[TARDY_LLM_MAX_RESPONSE * 2];
    int total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], curl_output + total,
                     sizeof(curl_output) - total - 1)) > 0)
        total += (int)n;
    curl_output[total] = '\0';
    close(pipefd[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (total <= 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        strncpy(resp->error, "curl failed", sizeof(resp->error));
        resp->success = false;
        return -1;
    }

    /* Parse JSON response */
    tardy_json_parser_t parser;
    if (tardy_json_parse(&parser, curl_output, total) < 0) {
        strncpy(resp->error, "JSON parse failed", sizeof(resp->error));
        resp->success = false;
        return -1;
    }

    /* Extract content[0].text */
    int content_tok = tardy_json_find(&parser, 0, "content");
    if (content_tok >= 0) {
        /* content is an array, first element is an object with "text" */
        int first_tok = content_tok + 1;
        int text_tok = tardy_json_find(&parser, first_tok, "text");
        if (text_tok >= 0) {
            resp->text_len = tardy_json_str(&parser, text_tok,
                                             resp->text, TARDY_LLM_MAX_RESPONSE);
            resp->success = true;
        }
    }

    /* Check for error */
    if (!resp->success) {
        int err_tok = tardy_json_find(&parser, 0, "error");
        if (err_tok >= 0) {
            int msg_tok = tardy_json_find(&parser, err_tok, "message");
            if (msg_tok >= 0)
                tardy_json_str(&parser, msg_tok,
                               resp->error, sizeof(resp->error));
        }
        if (!resp->error[0])
            strncpy(resp->error, "unknown API error", sizeof(resp->error));
    }

    strncpy(resp->model, config->model[0] ? config->model : "claude-sonnet-4-20250514",
            sizeof(resp->model));
    resp->temperature = config->temperature;

    /* Extract usage */
    int usage_tok = tardy_json_find(&parser, 0, "usage");
    if (usage_tok >= 0) {
        int in_tok = tardy_json_find(&parser, usage_tok, "input_tokens");
        if (in_tok >= 0) resp->tokens_in = (int)tardy_json_int(&parser, in_tok);
        int out_tok = tardy_json_find(&parser, usage_tok, "output_tokens");
        if (out_tok >= 0) resp->tokens_out = (int)tardy_json_int(&parser, out_tok);
    }

    return resp->success ? 0 : -1;
}

/* ============================================
 * Public API
 * ============================================ */

int tardy_llm_init(tardy_llm_backend_t *backend,
                    const tardy_llm_config_t *config)
{
    if (!backend || !config)
        return -1;
    backend->config = *config;
    backend->initialized = true;
    return 0;
}

int tardy_llm_ask(tardy_llm_backend_t *backend,
                   const char *system_prompt,
                   const char *user_prompt,
                   tardy_llm_response_t *response)
{
    if (!backend || !backend->initialized || !response)
        return -1;

    switch (backend->config.provider) {
    case TARDY_LLM_STUB:
        return stub_ask(user_prompt, response);

    case TARDY_LLM_ANTHROPIC:
        return anthropic_ask(&backend->config, system_prompt,
                              user_prompt, response);

    case TARDY_LLM_OPENAI:
        /* TODO: similar to Anthropic but different API format */
        strncpy(response->error, "OpenAI backend not yet implemented",
                sizeof(response->error));
        return -1;

    case TARDY_LLM_LOCAL:
        /* TODO: unix socket to ollama/llama.cpp */
        strncpy(response->error, "Local backend not yet implemented",
                sizeof(response->error));
        return -1;
    }

    return -1;
}

void tardy_llm_shutdown(tardy_llm_backend_t *backend)
{
    if (backend)
        backend->initialized = false;
}

tardy_llm_config_t tardy_llm_stub_config(void)
{
    tardy_llm_config_t config = {0};
    config.provider = TARDY_LLM_STUB;
    config.temperature = 0.0f;
    config.max_tokens = 1024;
    return config;
}

tardy_llm_config_t tardy_llm_anthropic_config(void)
{
    tardy_llm_config_t config = {0};
    config.provider = TARDY_LLM_ANTHROPIC;
    config.temperature = 0.0f;
    config.max_tokens = 1024;
    strncpy(config.model, "claude-sonnet-4-20250514", sizeof(config.model));

    /* Read API key from environment */
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (key)
        strncpy(config.api_key, key, sizeof(config.api_key) - 1);

    return config;
}
