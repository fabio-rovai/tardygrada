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
