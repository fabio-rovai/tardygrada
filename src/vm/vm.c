/*
 * Tardygrada VM — Implementation
 * The root agent. The incorruptible judge. Pure C.
 */

#include "vm.h"
#include <string.h>
#include <time.h>

/* Forward declarations from context.c */
extern tardy_uuid_t tardy_uuid_gen(void);
extern bool tardy_uuid_eq(const tardy_uuid_t *a, const tardy_uuid_t *b);

static tardy_timestamp_t now_ns_vm(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const tardy_uuid_t ZERO_UUID = {0, 0};

/* ============================================
 * VM Init / Shutdown
 * ============================================ */

int tardy_vm_init(tardy_vm_t *vm, const tardy_semantics_t *semantics)
{
    if (!vm)
        return -1;

    memset(vm, 0, sizeof(tardy_vm_t));

    /* Apply semantics (or defaults) */
    if (semantics)
        vm->semantics = *semantics;
    else
        vm->semantics = TARDY_DEFAULT_SEMANTICS;

    /* Generate root identity */
    vm->root_id = tardy_uuid_gen();
    tardy_keygen(&vm->root_key);

    /* Create root agent — the VM itself is agent 0 */
    tardy_agent_t *root = &vm->agents[0];
    root->id        = vm->root_id;
    root->state     = TARDY_STATE_LIVE;
    root->type_tag  = TARDY_TYPE_AGENT;
    root->trust     = TARDY_TRUST_SOVEREIGN;
    root->ref_count = 1; /* self-reference, never GC'd */
    root->last_accessed = now_ns_vm();
    vm->agent_count = 1;

    vm->boot_time = now_ns_vm();
    vm->running   = true;

    return 0;
}

void tardy_vm_shutdown(tardy_vm_t *vm)
{
    if (!vm)
        return;

    vm->running = false;

    /* Free all agent memory (skip root at index 0) */
    for (int i = 1; i < vm->agent_count; i++) {
        tardy_agent_t *a = &vm->agents[i];
        if (a->state == TARDY_STATE_LIVE)
            tardy_mem_free(&a->memory);
        if (a->mutations)
            /* TODO: proper free for mutation log */
            ;
    }
}

/* ============================================
 * Agent Lookup
 * ============================================ */

tardy_agent_t *tardy_vm_find(tardy_vm_t *vm, tardy_uuid_t id)
{
    for (int i = 0; i < vm->agent_count; i++) {
        if (tardy_uuid_eq(&vm->agents[i].id, &id) &&
            vm->agents[i].state != TARDY_STATE_DEAD)
            return &vm->agents[i];
    }
    return NULL;
}

tardy_agent_t *tardy_vm_find_by_name(tardy_vm_t *vm,
                                      tardy_uuid_t parent_id,
                                      const char *name)
{
    tardy_agent_t *parent = tardy_vm_find(vm, parent_id);
    if (!parent)
        return NULL;

    for (int i = 0; i < parent->context.child_count; i++) {
        if (strncmp(parent->context.children[i].name, name,
                    TARDY_CTX_MAX_NAME) == 0) {
            return tardy_vm_find(vm, parent->context.children[i].agent_id);
        }
    }
    return NULL;
}

/* ============================================
 * Spawn — create a new agent in parent's context
 *
 * This is `let x: int = 5` in Tardygrada.
 * A new agent is born. It holds the value.
 * It lives in the parent's context.
 * ============================================ */

tardy_uuid_t tardy_vm_spawn(tardy_vm_t *vm,
                             tardy_uuid_t parent_id,
                             const char *name,
                             tardy_type_t type,
                             tardy_trust_t trust,
                             const void *data, size_t len)
{
    if (!vm || !vm->running)
        return ZERO_UUID;

    if (vm->agent_count >= TARDY_MAX_AGENTS)
        return ZERO_UUID;

    /* Find parent */
    tardy_agent_t *parent = tardy_vm_find(vm, parent_id);
    if (!parent)
        return ZERO_UUID;

    /* Check parent has room for children */
    if (parent->context.child_count >= TARDY_CTX_MAX_CHILDREN)
        return ZERO_UUID;

    /* Determine replica count from semantics */
    int replicas = 0;
    if (trust == TARDY_TRUST_HARDENED)
        replicas = vm->semantics.immutability.hardened_replica_count;
    else if (trust == TARDY_TRUST_SOVEREIGN)
        replicas = vm->semantics.immutability.sovereign_replica_count;

    /* Allocate the new agent */
    int idx = vm->agent_count;
    tardy_agent_t *agent = &vm->agents[idx];
    memset(agent, 0, sizeof(tardy_agent_t));

    agent->id        = tardy_uuid_gen();
    agent->state     = TARDY_STATE_LIVE;
    agent->type_tag  = type;
    agent->trust     = trust;
    agent->data_size = len;
    agent->ref_count = 1;
    agent->last_accessed = now_ns_vm();

    /* Allocate and initialize memory */
    agent->memory = tardy_mem_alloc(len, trust, replicas);
    tardy_mem_init(&agent->memory, data, len, &vm->root_key);

    /* Set provenance */
    agent->provenance.created_by = parent_id;
    agent->provenance.created_at = now_ns_vm();
    agent->provenance.reason     = "spawn";
    if (agent->memory.has_hash)
        agent->provenance.birth_hash = agent->memory.birth_hash;

    /* Register in parent's context */
    tardy_named_child_t *child =
        &parent->context.children[parent->context.child_count];
    strncpy(child->name, name, TARDY_CTX_MAX_NAME - 1);
    child->name[TARDY_CTX_MAX_NAME - 1] = '\0';
    child->agent_id = agent->id;
    parent->context.child_count++;

    vm->agent_count++;
    return agent->id;
}

/* ============================================
 * Kill — destroy a mutable agent
 * Immutable agents CANNOT be killed.
 * ============================================ */

int tardy_vm_kill(tardy_vm_t *vm, tardy_uuid_t agent_id)
{
    tardy_agent_t *agent = tardy_vm_find(vm, agent_id);
    if (!agent)
        return -1;

    /* Cannot kill immutable agents */
    if (agent->trust >= TARDY_TRUST_DEFAULT)
        return -1;

    /* Create tombstone */
    if (vm->tombstone_count < TARDY_MAX_TOMBSTONES) {
        tardy_tombstone_t *tomb = &vm->tombstones[vm->tombstone_count++];
        tomb->id         = agent->id;
        tomb->was_type   = agent->type_tag;
        tomb->birth_hash = agent->provenance.birth_hash;
        tomb->died_at    = now_ns_vm();
        /* Compute final hash */
        if (agent->state == TARDY_STATE_LIVE) {
            int64_t val;
            tardy_page_read(&agent->memory.primary, &val, sizeof(int64_t));
            tardy_sha256(&val, sizeof(int64_t), &tomb->final_hash);
        }
    }

    /* Free memory */
    tardy_mem_free(&agent->memory);
    agent->state = TARDY_STATE_DEAD;

    return 0;
}

/* ============================================
 * Read — read an agent's value by name
 * ============================================ */

tardy_read_status_t tardy_vm_read(tardy_vm_t *vm,
                                   tardy_uuid_t parent_id,
                                   const char *name,
                                   void *out, size_t len)
{
    tardy_agent_t *agent = tardy_vm_find_by_name(vm, parent_id, name);
    if (!agent)
        return TARDY_READ_HASH_MISMATCH;

    agent->last_accessed = now_ns_vm();

    /* If static, return the cached value directly */
    if (agent->state == TARDY_STATE_STATIC) {
        if (agent->type_tag == TARDY_TYPE_STR) {
            size_t copy = agent->data_size;
            if (copy > len) copy = len;
            memcpy(out, agent->static_str, copy);
        } else {
            memcpy(out, &agent->static_value,
                   len < sizeof(int64_t) ? len : sizeof(int64_t));
        }
        return TARDY_READ_OK;
    }

    /* Live/Temp: read using the original data size for correct hash verification */
    size_t read_size = agent->data_size > 0 ? agent->data_size : len;
    if (read_size > len) read_size = len;
    return tardy_mem_read(&agent->memory, out, read_size);
}

/* ============================================
 * Mutate — change a mutable agent's value
 * ============================================ */

int tardy_vm_mutate(tardy_vm_t *vm,
                     tardy_uuid_t parent_id,
                     const char *name,
                     const void *data, size_t len)
{
    tardy_agent_t *agent = tardy_vm_find_by_name(vm, parent_id, name);
    if (!agent)
        return -1;

    if (agent->trust != TARDY_TRUST_MUTABLE)
        return -1;

    /* TODO: record mutation in provenance log */

    agent->last_accessed = now_ns_vm();
    agent->data_size = len;
    return tardy_mem_mutate(&agent->memory, data, len);
}

/* ============================================
 * Freeze — promote mutable to immutable
 * The critical operation: a pending value becomes a Fact.
 * ============================================ */

tardy_uuid_t tardy_vm_freeze(tardy_vm_t *vm,
                              tardy_uuid_t agent_id,
                              tardy_trust_t new_trust)
{
    tardy_agent_t *agent = tardy_vm_find(vm, agent_id);
    if (!agent)
        return ZERO_UUID;

    if (agent->trust != TARDY_TRUST_MUTABLE)
        return ZERO_UUID; /* already immutable */

    if (new_trust < TARDY_TRUST_DEFAULT)
        return ZERO_UUID; /* can't freeze to mutable */

    /* Read current value (use data_size for proper length) */
    size_t dsize = agent->data_size > 0 ? agent->data_size : sizeof(int64_t);
    char current_buf[4096];
    if (dsize > sizeof(current_buf)) dsize = sizeof(current_buf);
    tardy_page_read(&agent->memory.primary, current_buf, dsize);

    /* Free old mutable memory */
    tardy_mem_free(&agent->memory);

    /* Determine replica count */
    int replicas = 0;
    if (new_trust == TARDY_TRUST_HARDENED)
        replicas = vm->semantics.immutability.hardened_replica_count;
    else if (new_trust == TARDY_TRUST_SOVEREIGN)
        replicas = vm->semantics.immutability.sovereign_replica_count;

    /* Reallocate with new trust level */
    agent->memory = tardy_mem_alloc(dsize, new_trust, replicas);
    tardy_mem_init(&agent->memory, current_buf, dsize, &vm->root_key);

    agent->trust = new_trust;
    agent->provenance.reason = "frozen";

    return agent->id;
}

/* ============================================
 * GC — Garbage Collection
 * Demote idle agents. Collect dead ones.
 * @sovereign agents: never collected, dump to disk when idle.
 * ============================================ */

int tardy_vm_gc(tardy_vm_t *vm)
{
    if (!vm)
        return -1;

    tardy_timestamp_t now = now_ns_vm();
    uint64_t demotion_threshold_ns =
        (uint64_t)vm->semantics.lifecycle.demotion_idle_ms * 1000000ULL;
    int collected = 0;

    for (int i = 1; i < vm->agent_count; i++) { /* skip root */
        tardy_agent_t *a = &vm->agents[i];

        if (a->state == TARDY_STATE_DEAD)
            continue;

        /* @sovereign: never collect, never demote */
        if (a->trust == TARDY_TRUST_SOVEREIGN)
            continue;

        uint64_t idle = now - a->last_accessed;

        /* Demote idle Live agents to Static */
        if (a->state == TARDY_STATE_LIVE && idle > demotion_threshold_ns) {
            tardy_vm_demote(vm, a->id);
            collected++;
            continue;
        }

        /* Expire Temp agents back to Static */
        if (a->state == TARDY_STATE_TEMP) {
            uint64_t ttl_ns = a->temp_ttl_ms * 1000000ULL;
            if (idle > ttl_ns) {
                tardy_vm_demote(vm, a->id);
                collected++;
                continue;
            }
        }

        /* Collect unreferenced Static agents */
        if (a->state == TARDY_STATE_STATIC && a->ref_count == 0) {
            /* Create tombstone */
            if (vm->tombstone_count < TARDY_MAX_TOMBSTONES) {
                tardy_tombstone_t *tomb =
                    &vm->tombstones[vm->tombstone_count++];
                tomb->id         = a->id;
                tomb->was_type   = a->type_tag;
                tomb->birth_hash = a->snapshot.value_hash;
                tomb->died_at    = now;
                tardy_sha256(&a->static_value, sizeof(int64_t),
                             &tomb->final_hash);
            }
            a->state = TARDY_STATE_DEAD;
            collected++;
        }
    }

    return collected;
}

/* ============================================
 * Demote — Live agent becomes Static
 * Free the heavy memory. Keep just the value + snapshot.
 * ============================================ */

int tardy_vm_demote(tardy_vm_t *vm, tardy_uuid_t agent_id)
{
    tardy_agent_t *a = tardy_vm_find(vm, agent_id);
    if (!a || a->state != TARDY_STATE_LIVE)
        return -1;

    /* Cannot demote @sovereign */
    if (a->trust == TARDY_TRUST_SOVEREIGN)
        return -1;

    /* Save the raw value */
    tardy_page_read(&a->memory.primary, &a->static_value, sizeof(int64_t));

    /* Create snapshot */
    a->snapshot.original_id = a->id;
    a->snapshot.created_by  = a->provenance.created_by;
    a->snapshot.created_at  = a->provenance.created_at;
    a->snapshot.trust       = a->trust;
    a->snapshot.type_tag    = a->type_tag;
    tardy_sha256(&a->static_value, sizeof(int64_t), &a->snapshot.value_hash);

    /* Free the heavy memory */
    tardy_mem_free(&a->memory);

    a->state = TARDY_STATE_STATIC;
    return 0;
}

/* ============================================
 * Promote — Static agent becomes Temp
 * Resurrect with a fresh agent memory.
 * ============================================ */

int tardy_vm_promote(tardy_vm_t *vm, tardy_uuid_t agent_id)
{
    tardy_agent_t *a = tardy_vm_find(vm, agent_id);
    if (!a || a->state != TARDY_STATE_STATIC)
        return -1;

    /* Verify the static value hasn't been corrupted */
    tardy_hash_t check;
    tardy_sha256(&a->static_value, sizeof(int64_t), &check);
    if (!tardy_hash_eq(&check, &a->snapshot.value_hash))
        return -1; /* corruption detected */

    /* Reallocate as mutable temp agent */
    a->memory = tardy_mem_alloc(sizeof(int64_t), TARDY_TRUST_MUTABLE, 0);
    tardy_mem_init(&a->memory, &a->static_value, sizeof(int64_t), NULL);

    a->state        = TARDY_STATE_TEMP;
    a->temp_ttl_ms  = vm->semantics.lifecycle.temp_ttl_ms;
    a->last_accessed = now_ns_vm();

    return 0;
}
