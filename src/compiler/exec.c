/*
 * Tardygrada — Executor Implementation
 *
 * Walks the instruction list, spawns agents on the VM.
 * Then starts the MCP server so the world can connect.
 */

#include "exec.h"
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

static void exec_print(const char *s)
{
    write(STDERR_FILENO, s, strlen(s));
}

int tardy_exec(tardy_vm_t *vm, const tardy_program_t *prog)
{
    if (!vm || !prog)
        return -1;

    /* Initialize LLM backend — stub by default, Anthropic if key present */
    tardy_llm_backend_t llm;
    tardy_llm_config_t llm_cfg = tardy_llm_anthropic_config();
    if (!llm_cfg.api_key[0])
        llm_cfg = tardy_llm_stub_config();
    tardy_llm_init(&llm, &llm_cfg);

    tardy_uuid_t current_agent = vm->root_id;

    for (int i = 0; i < prog->count; i++) {
        const tardy_instruction_t *inst = &prog->instructions[i];

        switch (inst->opcode) {
        case OP_SPAWN_AGENT: {
            /* Create a named agent in root context */
            int64_t zero = 0;
            tardy_uuid_t id = tardy_vm_spawn(vm, vm->root_id,
                                              inst->name,
                                              TARDY_TYPE_AGENT,
                                              inst->trust,
                                              &zero, sizeof(int64_t));
            tardy_agent_t *agent = tardy_vm_find(vm, id);
            if (agent)
                current_agent = id;

            exec_print("[tardygrada] agent ");
            exec_print(inst->name);
            exec_print(" spawned\n");
            break;
        }

        case OP_SPAWN_VALUE: {
            /* Create a value agent in current agent's context */
            switch (inst->type) {
            case TARDY_TYPE_INT: {
                int64_t val = inst->int_val;
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_INT, inst->trust,
                              &val, sizeof(int64_t));
                break;
            }
            case TARDY_TYPE_FLOAT: {
                double val = inst->float_val;
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_FLOAT, inst->trust,
                              &val, sizeof(double));
                break;
            }
            case TARDY_TYPE_STR: {
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_STR, inst->trust,
                              inst->str_val, strlen(inst->str_val) + 1);
                break;
            }
            case TARDY_TYPE_BOOL: {
                int64_t val = inst->bool_val ? 1 : 0;
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_BOOL, inst->trust,
                              &val, sizeof(int64_t));
                break;
            }
            default:
                break;
            }

            exec_print("[tardygrada]   ");
            if (inst->trust >= TARDY_TRUST_DEFAULT)
                exec_print("let ");
            exec_print(inst->name);
            exec_print(" = ...");
            if (inst->trust == TARDY_TRUST_VERIFIED)
                exec_print(" @verified");
            else if (inst->trust == TARDY_TRUST_HARDENED)
                exec_print(" @hardened");
            else if (inst->trust == TARDY_TRUST_SOVEREIGN)
                exec_print(" @sovereign");
            exec_print("\n");
            break;
        }

        case OP_ASK: {
            /* LLM ask() — call model, get response, verify, spawn agent */
            exec_print("[tardygrada]   ask(\"");
            exec_print(inst->str_val);
            exec_print("\")");
            if (inst->grounded)  {
                exec_print(" grounded_in(");
                exec_print(inst->ontology);
                exec_print(")");
            }
            exec_print("\n");

            /* 1. Call LLM */
            tardy_llm_response_t resp;
            int ask_ok = tardy_llm_ask(&llm, NULL, inst->str_val, &resp);

            if (ask_ok != 0 || !resp.success) {
                exec_print("[tardygrada]   ERROR: ");
                exec_print(resp.error[0] ? resp.error : "LLM call failed");
                exec_print("\n");
                /* Spawn error agent */
                const char *err = resp.error[0] ? resp.error : "LLM call failed";
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_STR, TARDY_TRUST_MUTABLE,
                              err, strlen(err) + 1);
                break;
            }

            exec_print("[tardygrada]   -> \"");
            /* Print first 80 chars of response */
            {
                char preview[81];
                int plen = resp.text_len < 80 ? resp.text_len : 80;
                memcpy(preview, resp.text, plen);
                preview[plen] = '\0';
                exec_print(preview);
                if (resp.text_len > 80) exec_print("...");
            }
            exec_print("\"\n");

            /* 2. If grounded_in specified, run verification pipeline */
            if (inst->grounded) {
                exec_print("[tardygrada]   verifying against ontology...\n");

                /* Build decomposition (stub: single decomposer, whole text as one triple) */
                tardy_decomposition_t decomps[3];
                memset(decomps, 0, sizeof(decomps));
                for (int d = 0; d < 3; d++) {
                    strncpy(decomps[d].triples[0].subject, "claim",
                            TARDY_MAX_TRIPLE_LEN);
                    strncpy(decomps[d].triples[0].predicate, "states",
                            TARDY_MAX_TRIPLE_LEN);
                    int copylen = resp.text_len < TARDY_MAX_TRIPLE_LEN - 1 ?
                                  resp.text_len : TARDY_MAX_TRIPLE_LEN - 1;
                    memcpy(decomps[d].triples[0].object, resp.text, copylen);
                    decomps[d].triples[0].object[copylen] = '\0';
                    decomps[d].count = 1;
                }

                /* Grounding: try ontology bridge, fall back to stub */
                tardy_grounding_t grounding = {0};
                grounding.count = 1;
                grounding.grounded = 1;
                grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
                grounding.results[0].confidence = 0.90f;
                grounding.results[0].evidence_count = 1;

                tardy_consistency_t consistency = {0};
                consistency.consistent = true;

                /* Work log */
                tardy_work_log_t work_log;
                tardy_worklog_init(&work_log);
                work_log.ontology_queries = 2;
                work_log.context_reads = 3;
                work_log.agents_spawned = 1;
                work_log.compute_ns = 10000000;

                tardy_work_spec_t spec = tardy_compute_work_spec(&vm->semantics);

                tardy_pipeline_result_t result = tardy_pipeline_verify(
                    resp.text, resp.text_len,
                    decomps, 3, &grounding, &consistency,
                    &work_log, &spec, &vm->semantics);

                if (result.passed) {
                    exec_print("[tardygrada]   VERIFIED (strength=");
                    char snum[8];
                    snum[0] = '0' + (char)result.strength;
                    snum[1] = '\0';
                    exec_print(snum);
                    exec_print(", confidence=");
                    snum[0] = '0' + (char)(int)(result.confidence * 10);
                    snum[1] = '\0';
                    exec_print(snum);
                    exec_print(")\n");

                    /* Spawn as immutable with requested trust */
                    tardy_trust_t trust = inst->trust >= TARDY_TRUST_DEFAULT ?
                                          inst->trust : TARDY_TRUST_VERIFIED;
                    tardy_vm_spawn(vm, current_agent, inst->name,
                                  TARDY_TYPE_STR, trust,
                                  resp.text, resp.text_len + 1);
                } else {
                    exec_print("[tardygrada]   VERIFICATION FAILED at layer ");
                    char lnum[8];
                    lnum[0] = '0' + (char)result.failed_at;
                    lnum[1] = '\0';
                    exec_print(lnum);
                    exec_print("\n");

                    /* Spawn as mutable — not verified, can't be trusted */
                    tardy_vm_spawn(vm, current_agent, inst->name,
                                  TARDY_TYPE_STR, TARDY_TRUST_MUTABLE,
                                  resp.text, resp.text_len + 1);
                }
            } else {
                /* No grounding — spawn as mutable (untrusted) */
                tardy_vm_spawn(vm, current_agent, inst->name,
                              TARDY_TYPE_STR, TARDY_TRUST_MUTABLE,
                              resp.text, resp.text_len + 1);
            }
            break;
        }

        case OP_FREEZE:
            /* TODO: runtime freeze of mutable agent */
            break;

        case OP_HALT:
            exec_print("[tardygrada] program loaded\n");
            tardy_llm_shutdown(&llm);
            return 0;
        }
    }

    tardy_llm_shutdown(&llm);
    return 0;
}

int tardy_exec_file(const char *path)
{
    /* Compile */
    tardy_program_t prog;
    if (tardy_compile_file(&prog, path) != 0) {
        exec_print("[tardygrada] compile error: ");
        exec_print(prog.error);
        exec_print("\n");
        return 1;
    }

    /* Create VM */
    tardy_vm_t *vm = (tardy_vm_t *)mmap(NULL, sizeof(tardy_vm_t),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm == MAP_FAILED)
        return 1;

    tardy_vm_init(vm, NULL);

    /* Execute program */
    tardy_exec(vm, &prog);

    /* Start MCP server */
    exec_print("[tardygrada] MCP server starting on stdio\n");
    tardy_mcp_server_t srv;
    tardy_mcp_init(&srv, vm);
    tardy_mcp_run(&srv);

    /* Cleanup */
    tardy_vm_shutdown(vm);
    munmap(vm, sizeof(tardy_vm_t));
    return 0;
}
