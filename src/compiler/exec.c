/*
 * Tardygrada — Executor Implementation
 *
 * Walks the instruction list, spawns agents on the VM.
 * Then starts the MCP server so the world can connect.
 */

#include "exec.h"
#include "terraform.h"
#include "../verify/pipeline.h"
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

        case OP_RECEIVE: {
            exec_print("[tardygrada]   receive(\"");
            exec_print(inst->str_val);
            exec_print("\")");
            if (inst->grounded) {
                exec_print(" grounded_in(");
                exec_print(inst->ontology);
                exec_print(")");
            }
            exec_print(" -> pending\n");

            /* Spawn empty mutable agent (will be filled via MCP submit_claim) */
            const char *empty = "";
            tardy_vm_spawn(vm, current_agent, inst->name,
                          TARDY_TYPE_STR, TARDY_TRUST_MUTABLE,
                          empty, 1);
            break;
        }

        case OP_FORK:
            tardy_fork(vm, current_agent, inst->str_val,
                       inst->name[0] ? inst->name : NULL);
            break;

        case OP_FREEZE:
            /* TODO: runtime freeze of mutable agent */
            break;

        case OP_SET_SEMANTICS: {
            exec_print("[tardygrada]   @semantics(");
            exec_print(inst->sem_key);
            exec_print(": ");
            exec_print(inst->sem_value);
            exec_print(")\n");

            /* Apply semantics override to current agent */
            const tardy_semantics_t *current_sem =
                tardy_vm_get_semantics(vm, current_agent);
            tardy_semantics_t updated = *current_sem;

            /* Parse key and set value */
            if (strcmp(inst->sem_key, "truth.min_confidence") == 0) {
                float f = 0.0f;
                const char *s = inst->sem_value;
                int i = 0;
                for (; s[i] && s[i] != '.'; i++) f = f * 10.0f + (s[i] - '0');
                if (s[i] == '.') {
                    i++;
                    float frac = 0.1f;
                    for (; s[i]; i++) { f += (s[i] - '0') * frac; frac *= 0.1f; }
                }
                updated.truth.min_confidence = f;
            } else if (strcmp(inst->sem_key, "truth.min_consensus_agents") == 0) {
                updated.truth.min_consensus_agents = (int)inst->int_val;
                if (updated.truth.min_consensus_agents == 0) {
                    /* Parse from string */
                    int v = 0;
                    const char *s = inst->sem_value;
                    for (int i = 0; s[i]; i++) v = v * 10 + (s[i] - '0');
                    updated.truth.min_consensus_agents = v;
                }
            } else if (strcmp(inst->sem_key, "pipeline.min_passing_layers") == 0) {
                int v = 0;
                const char *s = inst->sem_value;
                for (int i = 0; s[i]; i++) v = v * 10 + (s[i] - '0');
                updated.pipeline.min_passing_layers = v;
            }

            tardy_vm_set_semantics(vm, current_agent, &updated);
            break;
        }

        case OP_COORDINATE: {
            exec_print("[tardygrada]   coordinate {");
            exec_print(inst->coord_agents);
            exec_print("} on(\"");
            exec_print(inst->coord_task);
            exec_print("\")\n");
            /* Coordination sends the task to each named agent's inbox
             * and waits for responses. For now, log and continue. */
            /* TODO: dispatch task to agents, collect results, consensus vote */
            break;
        }

        case OP_HALT:
            exec_print("[tardygrada] program loaded\n");
            return 0;
        }
    }

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
