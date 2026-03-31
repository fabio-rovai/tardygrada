# Tardygrada Complete Language Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Refactor ask() into receive/verify MCP pattern, then build all remaining designed features so the language matches the spec.

**Architecture:** Remove outbound LLM backend. External agents send claims via MCP tools/call. Tardygrada receives, decomposes, grounds against ontology, verifies, and freezes or rejects. Every MCP response carries provenance. Constitution invariants checked on every operation. Agents communicate internally via message passing.

**Tech Stack:** C11 (no stdlib malloc), direct syscalls, SHA-256, ed25519 stubs, JSON-RPC 2.0 MCP stdio transport.

---

### Task 1: Remove LLM Backend, Add receive/verify MCP Tools

**Files:**
- Delete: `src/llm/backend.c`, `src/llm/backend.h`
- Modify: `src/mcp/server.c:250-360` — add `tools/call` handlers for `receive` and `verify`
- Modify: `src/mcp/server.h:20-30` — no LLM dependency
- Modify: `src/compiler/compiler.h:27-33` — replace `OP_ASK` with `OP_RECEIVE`
- Modify: `src/compiler/compiler.c:194-230` — parse `receive()` instead of `ask()`
- Modify: `src/compiler/exec.c:103-226` — remove LLM call, wire to MCP receive
- Modify: `src/compiler/exec.h:10-12` — remove LLM include
- Modify: `Makefile` — remove `src/llm/backend.c`
- Create: `examples/receive.tardy`

**Step 1: Delete LLM backend**

Remove `src/llm/backend.c` and `src/llm/backend.h`. Remove from Makefile.

**Step 2: Add MCP tools: `submit_claim` and `verify_claim`**

In `src/mcp/server.c`, add two new MCP tool handlers:

```c
// tools/call with name="submit_claim"
// Params: {"name": "submit_claim", "arguments": {"agent": "origin", "claim": "Dr Who was created at BBC"}}
// → Spawns a mutable agent holding the claim text
// → Returns: {"status": "pending", "agent": "origin"}

// tools/call with name="verify_claim"
// Params: {"name": "verify_claim", "arguments": {"agent": "origin"}}
// → Runs the pending claim through the verification pipeline
// → If passes: freeze to the agent's declared trust level
// → Returns: {"status": "verified", "strength": 4, "confidence": 0.95}
// → If fails: {"status": "rejected", "failed_at": "grounding", "detail": "..."}
```

**Step 3: Replace `ask()` with `receive()` in compiler**

Replace `OP_ASK` opcode with `OP_RECEIVE`. The `receive()` syntax declares a slot that accepts claims via MCP:

```
// New syntax:
let origin: Fact = receive("Where was Doctor Who created?") grounded_in(bbc) @verified
// This creates a pending agent that MCP clients fill via submit_claim
```

`OP_RECEIVE` spawns a mutable TARDY_TYPE_STR agent with an empty value and a flag marking it as "pending." The prompt string is stored in provenance.reason for documentation.

**Step 4: Update executor**

Remove all LLM includes. `OP_RECEIVE` handler:
```c
case OP_RECEIVE: {
    // Spawn empty mutable agent marked as pending
    const char *empty = "";
    tardy_vm_spawn(vm, current_agent, inst->name,
                  TARDY_TYPE_STR, TARDY_TRUST_MUTABLE,
                  empty, 1);
    // Store the intended trust level + ontology in agent provenance
    break;
}
```

**Step 5: Create `examples/receive.tardy`**

```
agent Researcher {
    let origin: Fact = receive("Where was Doctor Who created?") grounded_in(bbc) @verified
    let capital: Fact = receive("What is the capital of France?") grounded_in(geography) @verified
    let name: str = "tardygrada" @sovereign
}
```

**Step 6: Build and test**

```bash
make clean && make && make run
# Test MCP: submit_claim, then verify_claim, then read
```

**Step 7: Commit**

```bash
git add -A && git commit -m "refactor: replace ask() with receive/verify MCP pattern

External agents submit claims via MCP. Tardygrada verifies, not generates.
Removed outbound LLM backend — the language receives, not calls."
```

---

### Task 2: Provenance in MCP Responses

**Files:**
- Modify: `src/mcp/server.c:290-340` — include provenance in tool call responses
- Modify: `src/vm/vm.h:82-85` — add `tardy_vm_read_with_provenance()`
- Modify: `src/vm/vm.c:219-245` — implement provenance read

**Step 1: Add provenance read to VM**

```c
typedef struct {
    tardy_read_status_t status;
    tardy_provenance_t  provenance;
    tardy_trust_t       trust;
    tardy_truth_strength_t strength;
    tardy_state_t       state;
} tardy_read_result_t;

tardy_read_result_t tardy_vm_read_full(tardy_vm_t *vm,
                                        tardy_uuid_t parent_id,
                                        const char *name,
                                        void *out, size_t len);
```

**Step 2: Include provenance in MCP response**

```json
{
  "content": [{"type": "text", "text": "BBC Television Centre"}],
  "provenance": {
    "created_by": "agent-uuid",
    "created_at": 1234567890,
    "trust": "verified",
    "strength": "evidenced",
    "reason": "frozen from claim submission",
    "birth_hash": "abc123..."
  }
}
```

**Step 3: Build, test, commit**

---

### Task 3: Constitution Checking

**Files:**
- Create: `src/vm/constitution.h`
- Create: `src/vm/constitution.c`
- Modify: `src/vm/context.h:132-134` — constitution struct already exists, extend
- Modify: `src/vm/vm.c` — add constitution check to every read/write/spawn
- Modify: `src/compiler/lexer.h` — add `TOK_ENSURES`, `TOK_INVARIANT` tokens
- Modify: `src/compiler/compiler.c` — parse `ensures()` and `invariant` blocks

**Step 1: Define constitution types**

```c
// src/vm/constitution.h
typedef enum {
    TARDY_INVARIANT_TYPE_CHECK,    // value must be this type
    TARDY_INVARIANT_RANGE,         // int must be in range
    TARDY_INVARIANT_NON_EMPTY,     // string must not be empty
    TARDY_INVARIANT_GROUNDED,      // must have ontology evidence
    TARDY_INVARIANT_TRUST_MIN,     // must have minimum trust level
    TARDY_INVARIANT_CUSTOM,        // custom check function
} tardy_invariant_type_t;

typedef struct {
    tardy_invariant_type_t type;
    int64_t                int_arg;  // for RANGE: min/max
    tardy_trust_t          trust_arg; // for TRUST_MIN
    tardy_hash_t           hash;     // hash of invariant definition
} tardy_invariant_t;

#define TARDY_MAX_INVARIANTS 16

typedef struct {
    tardy_invariant_t invariants[TARDY_MAX_INVARIANTS];
    int               count;
    tardy_hash_t      constitutional_hash; // hash of all invariants combined
} tardy_constitution_t;

// Check all invariants. Returns 0 if all pass, -1 if any fail.
int tardy_constitution_check(const tardy_constitution_t *con,
                              const tardy_agent_t *agent);

// Verify constitution hasn't been tampered with
int tardy_constitution_verify_integrity(const tardy_constitution_t *con);
```

**Step 2: Wire into VM operations**

Every `tardy_vm_read()`, `tardy_vm_mutate()`, `tardy_vm_spawn()` calls `tardy_constitution_check()` on the parent agent before proceeding.

**Step 3: Add syntax**

```
agent MedicalAdvisor @invariant(trust_min: @verified) {
    let diagnosis: Fact = receive("diagnosis") grounded_in(medical) @verified
    // Every operation in this agent checks: is everything at least @verified?
}
```

**Step 4: Build, test, commit**

---

### Task 4: Agent-to-Agent Communication

**Files:**
- Create: `src/vm/message.h`
- Create: `src/vm/message.c`
- Modify: `src/vm/vm.h` — add `tardy_vm_send()` and `tardy_vm_receive_msg()`
- Modify: `src/vm/context.h:70-74` — add message queue to agent context
- Modify: `src/compiler/lexer.h` — add `TOK_SEND`, `TOK_COORDINATE` tokens
- Modify: `src/compiler/compiler.c` — parse `send()` and `coordinate` blocks

**Step 1: Define message types**

```c
// src/vm/message.h
typedef struct {
    tardy_uuid_t    from;
    tardy_uuid_t    to;
    tardy_type_t    payload_type;
    char            payload[512];
    size_t          payload_len;
    tardy_hash_t    hash;           // hash of payload for integrity
    tardy_timestamp_t sent_at;
    tardy_provenance_t provenance;  // full provenance chain
} tardy_message_t;

#define TARDY_MAX_MESSAGES 64

typedef struct {
    tardy_message_t messages[TARDY_MAX_MESSAGES];
    int             count;
    int             head;
    int             tail;
} tardy_message_queue_t;
```

**Step 2: Add send/receive to VM**

```c
int tardy_vm_send(tardy_vm_t *vm, tardy_uuid_t from, tardy_uuid_t to,
                   const void *payload, size_t len, tardy_type_t type);

int tardy_vm_receive_msg(tardy_vm_t *vm, tardy_uuid_t agent_id,
                          tardy_message_t *out);
```

**Step 3: Add `coordinate` keyword**

```
agent Team {
    let a: Agent = spawn Analyzer
    let b: Agent = spawn Validator
    let result: Fact = coordinate [a, b] on("task") consensus(ProofWeight)
}
```

**Step 4: Build, test, commit**

---

### Task 5: Semantic Query (Vector-style Context Lookup)

**Files:**
- Create: `src/vm/semantic.h`
- Create: `src/vm/semantic.c`
- Modify: `src/vm/context.h:70-74` — add embedding storage to agent context
- Modify: `src/vm/vm.h` — add `tardy_vm_query()`

**Step 1: Simple keyword-based semantic search (no ML embeddings yet)**

```c
// Lightweight semantic search: keyword overlap scoring
// Not vector embeddings — that comes later. This is a hash-based
// keyword index that's fast and dependency-free.

typedef struct {
    tardy_uuid_t agent_id;
    float        score;
} tardy_query_result_t;

#define TARDY_MAX_QUERY_RESULTS 16

int tardy_vm_query(tardy_vm_t *vm, tardy_uuid_t scope,
                    const char *query,
                    tardy_query_result_t *results, int max_results);
```

**Step 2: Index agents by keywords from their name + value**

On spawn, extract keywords from name and value, store in a simple inverted index on the parent agent's context.

**Step 3: Add `query()` syntax**

```
agent Knowledge {
    let fact1: Fact = receive("GDP of UK") grounded_in(economics) @verified
    let fact2: Fact = receive("Population of UK") grounded_in(demographics) @verified

    // Query: "what do I know about UK?" → returns fact1 and fact2
}
```

**Step 4: Build, test, commit**

---

### Task 6: Self-Healing

**Files:**
- Create: `src/vm/heal.h`
- Create: `src/vm/heal.c`
- Modify: `src/vm/vm.c` — add self-heal triggers to read/write failures

**Step 1: Define healing actions**

```c
typedef enum {
    TARDY_HEAL_RESPAWN,      // re-create dead agent from tombstone
    TARDY_HEAL_REVERIFY,     // re-run verification pipeline
    TARDY_HEAL_PROMOTE,      // promote static back to live
    TARDY_HEAL_REGROUND,     // re-ground against ontology
} tardy_heal_action_t;

int tardy_heal(tardy_vm_t *vm, tardy_uuid_t agent_id,
                tardy_heal_action_t action);
```

**Step 2: Wire into VM**

When `tardy_vm_read()` fails (hash mismatch, no consensus), instead of returning error, attempt self-heal:
1. Check tombstone for birth_hash
2. Reconstruct from replicas if possible
3. Re-verify against ontology
4. If healed, return the value; if not, THEN return error

**Step 3: Build, test, commit**

---

### Task 7: Error-as-Agent

**Files:**
- Modify: `src/vm/types.h:18-25` — add `TARDY_TYPE_ERROR`
- Modify: `src/vm/vm.c` — on error, spawn an error agent instead of returning int
- Modify: `src/mcp/server.c` — error agents are queryable via MCP

**Step 1: Errors become agents**

When an operation fails, instead of returning `-1`, the VM spawns an error agent in the parent's context:

```c
// Instead of: return -1;
// Do:
tardy_vm_spawn_error(vm, parent_id, "mutate_failed",
                      "agent is immutable (let binding)",
                      TARDY_TRUST_DEFAULT);
```

The error agent holds: error message, what operation failed, which agent was involved, the stack of agent IDs leading to the error. It's queryable via MCP like any other agent — you converse with it.

**Step 2: Build, test, commit**

---

### Task 8: `@semantics()` Per-Agent Override

**Files:**
- Modify: `src/vm/context.h:107-141` — add `tardy_semantics_t *custom_semantics` to agent struct
- Modify: `src/compiler/compiler.c` — parse `@semantics(...)` block into per-agent overrides
- Modify: `src/verify/pipeline.c` — use agent's semantics instead of VM global

**Step 1: Parser for `@semantics(key: value, ...)`**

Lexer already has `TOK_AT_SEMANTICS`. Parse the key-value pairs:
```
agent Medical @semantics(
    truth.min_confidence: 0.99,
    truth.min_consensus_agents: 5,
) { ... }
```

**Step 2: Store on agent, use in pipeline**

When running the verification pipeline for an agent, check if it has custom semantics. If yes, use those. If no, fall back to VM global.

**Step 3: Build, test, commit**

---

### Task 9: Sovereign Disk Dump

**Files:**
- Create: `src/vm/persist.h`
- Create: `src/vm/persist.c`
- Modify: `src/vm/vm.c:280-320` — GC dumps idle sovereign agents to disk

**Step 1: Simple file-based persistence**

```c
// Sovereign agents dump to a directory as individual files
// Filename: <agent-uuid>.tardy.dat
// Format: binary struct { hash, value_bytes, provenance_bytes }

int tardy_persist_dump(const tardy_agent_t *agent, const char *dir);
int tardy_persist_load(tardy_agent_t *agent, const char *dir, tardy_uuid_t id);
```

**Step 2: Wire into GC**

In `tardy_vm_gc()`, sovereign agents that have been idle for `sovereign_dump_idle_ms` get dumped to disk. Their state changes to TARDY_STATE_STATIC but with a flag indicating they're on disk. On next access, they're loaded back.

**Step 3: Build, test, commit**

---

### Task 10: Agent Hierarchy Scope Chain

**Files:**
- Modify: `src/vm/vm.c:92-107` — `tardy_vm_find_by_name()` walks up parent chain
- Modify: `src/vm/context.h:107-141` — add `parent_id` tracking

**Step 1: Add parent tracking to agents**

Every agent already has `provenance.created_by` which is the parent ID. Use this for scope chain walking.

**Step 2: Walk up the chain on name lookup**

```c
// Current: only looks in direct parent
// New: if not found in parent, look in grandparent, etc.
tardy_agent_t *tardy_vm_find_by_name(tardy_vm_t *vm,
                                      tardy_uuid_t parent_id,
                                      const char *name)
{
    tardy_uuid_t current = parent_id;
    while (!is_zero_uuid(current)) {
        tardy_agent_t *parent = tardy_vm_find(vm, current);
        if (!parent) break;
        // Check children
        for (int i = 0; i < parent->context.child_count; i++) {
            if (strncmp(parent->context.children[i].name, name, ...) == 0)
                return tardy_vm_find(vm, parent->context.children[i].agent_id);
        }
        // Walk up to grandparent
        current = parent->provenance.created_by;
    }
    return NULL;
}
```

**Step 3: Build, test, commit**

---

### Task 11: Terraform/Fork Module System

**Files:**
- Create: `src/compiler/terraform.h`
- Create: `src/compiler/terraform.c`
- Modify: `src/compiler/lexer.h` — add `TOK_FORK` token
- Modify: `src/compiler/compiler.c` — parse `fork()` statements

**Step 1: Define fork semantics**

```
// Syntax:
fork "path/to/other.tardy" as OtherAgent

// What happens:
// 1. Read the .tardy file
// 2. Compile it in isolation
// 3. Run the verification pipeline on every value
// 4. If all pass: spawn the agents in current context
// 5. If any fail: reject the fork, spawn error agent
```

```c
int tardy_fork(tardy_vm_t *vm, tardy_uuid_t parent_id,
                const char *path, const char *alias);
```

**Step 2: Verify forked code**

Every agent spawned by the forked program gets its trust level verified against the current agent's constitution. A forked module can't escalate trust — if the parent is @verified, the fork can't declare @sovereign.

**Step 3: Build, test, commit**

---

### Task 12: VM Nesting

**Files:**
- Modify: `src/vm/vm.h` — add `tardy_vm_spawn_child_vm()`
- Modify: `src/vm/vm.c` — child VM creation with parent oversight

**Step 1: VM-inside-agent**

```c
// A child VM is just another agent that happens to run its own agent society
tardy_uuid_t tardy_vm_spawn_child(tardy_vm_t *parent_vm,
                                   tardy_uuid_t parent_agent,
                                   const char *name,
                                   const tardy_semantics_t *child_semantics);
```

The child VM gets its own agents array, its own semantics, but its root key is signed by the parent VM. The parent can inspect the child's agents.

**Step 2: Build, test, commit**

---

### Task 13: Real Decompose Step

**Files:**
- Create: `src/verify/decompose.h`
- Create: `src/verify/decompose.c`
- Modify: `src/verify/pipeline.c:70-120` — use real decomposer instead of stubs

**Step 1: Rule-based triple extraction**

No LLM for decomposition. Simple NLP-style rules:
- Split on sentences
- Extract (subject, verb, object) patterns
- Map to ontology predicates

```c
int tardy_decompose(const char *text, int len,
                     tardy_triple_t *triples, int max_triples);
```

Pattern matching: "X was created at Y" → (X, created_at, Y). "X is Y" → (X, is, Y). Cover the 20 most common English predicate patterns.

**Step 2: Multiple independent decomposers**

Run the same decomposer 3 times with slight variations (different sentence splitting strategies). Compare overlap. This gives agreement without needing 3 separate LLM calls.

**Step 3: Build, test, commit**

---

### Task 14: Wire Ontology Bridge to Open-Ontologies

**Files:**
- Modify: `src/ontology/bridge.c:100-180` — implement real JSON protocol
- Create: `src/ontology/protocol.md` — document the wire protocol

**Step 1: Define the protocol**

```json
// Request: ground triples
{"action": "ground", "triples": [{"s": "DrWho", "p": "created_at", "o": "BBC"}]}

// Response
{"results": [{"status": "grounded", "confidence": 95, "evidence_count": 3}]}

// Request: check consistency
{"action": "check_consistency", "triples": [...]}

// Response
{"consistent": true, "contradiction_count": 0}
```

**Step 2: Build a thin adapter in open-ontologies**

Add a unix socket listener to open-ontologies that speaks this JSON protocol. It wraps the existing `reason.rs` SPARQL-like queries.

**Step 3: Integration test: Tardygrada VM ↔ open-ontologies via socket**

**Step 4: Build, test, commit**

---

### Task 15: Coq BFT Consensus Proofs

**Files:**
- Create: `proofs/consensus.v` — Coq proof of Byzantine consensus correctness
- Create: `proofs/README.md` — how to verify the proofs

**Step 1: Model the consensus protocol in Coq**

Using Bythos framework (CCS 2024), model our specific consensus:
- N replicas, majority vote
- Hash verification after vote
- Signature verification for sovereign

**Step 2: Prove safety**

Prove: if fewer than N/2 replicas are corrupted, the voted value equals the original value.

**Step 3: Prove liveness**

Prove: if at least N/2+1 replicas are honest, consensus always terminates.

**Step 4: Commit proofs**

```bash
git add proofs/ && git commit -m "feat: Coq proofs of BFT consensus correctness"
```

---

### Execution Order

Tasks are ordered by dependency. Some can be parallelized:

```
Sequential (each depends on previous):
  Task 1  → Task 2  → Task 3

Parallel after Task 1:
  Task 4  (agent messaging)
  Task 5  (semantic query)
  Task 7  (error-as-agent)
  Task 8  (@semantics override)
  Task 10 (scope chain)

Parallel after Task 3:
  Task 6  (self-healing)
  Task 9  (sovereign dump)
  Task 13 (real decompose)

Depends on Task 1 + open-ontologies:
  Task 14 (ontology wire)

Independent:
  Task 11 (terraform/fork)
  Task 12 (VM nesting)
  Task 15 (Coq proofs)
```

**Estimated total: ~2,500 lines of C + ~500 lines of Coq.**
