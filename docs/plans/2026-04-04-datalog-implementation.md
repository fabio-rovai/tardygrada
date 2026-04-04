# Datalog Backbone Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace substring matching in the self-hosted ontology with a Datalog inference engine. Facts + rules = derived knowledge. Verification = "can this claim be logically derived?"

**Architecture:** Bottom-up semi-naive evaluation. Facts are ground atoms stored as agents. Rules are Horn clauses (the synthetic backbone). The engine derives all consequences at startup and after each new fact. Query = membership check in derived set. ~400 lines of C, no dependencies.

**Tech Stack:** C11, direct syscalls, existing Tardygrada VM types.

---

### Task 1: Datalog Data Structures

**Files:**
- Create: `src/ontology/datalog.h`
- Create: `src/ontology/datalog.c`

**Step 1: Write datalog.h**

```c
#ifndef TARDY_DATALOG_H
#define TARDY_DATALOG_H

#include <stdbool.h>

#define TARDY_DL_MAX_FACTS   4096
#define TARDY_DL_MAX_RULES   128
#define TARDY_DL_MAX_BODY    4
#define TARDY_DL_MAX_STR     128

// A ground atom: predicate(arg1, arg2)
typedef struct {
    char pred[64];
    char arg1[TARDY_DL_MAX_STR];
    char arg2[TARDY_DL_MAX_STR];
} tardy_dl_atom_t;

// A rule: head :- body[0], body[1], ...
// Variables are uppercase single letters: X, Y, Z
// Constants are lowercase strings
typedef struct {
    tardy_dl_atom_t head;
    tardy_dl_atom_t body[TARDY_DL_MAX_BODY];
    int             body_count;
} tardy_dl_rule_t;

// The Datalog program
typedef struct {
    tardy_dl_atom_t facts[TARDY_DL_MAX_FACTS];
    int             fact_count;
    tardy_dl_rule_t rules[TARDY_DL_MAX_RULES];
    int             rule_count;
    int             base_fact_count;  // facts before derivation
    bool            evaluated;        // has fixpoint been reached?
} tardy_dl_program_t;

// Initialize empty program
void tardy_dl_init(tardy_dl_program_t *prog);

// Add a ground fact
int tardy_dl_add_fact(tardy_dl_program_t *prog,
                       const char *pred, const char *arg1, const char *arg2);

// Add a rule
int tardy_dl_add_rule(tardy_dl_program_t *prog, const tardy_dl_rule_t *rule);

// Helper: build a rule from strings
// e.g., tardy_dl_make_rule("locatedIn", "X", "Y", "capital", "X", "Y", NULL)
// means: locatedIn(X, Y) :- capital(X, Y)
tardy_dl_rule_t tardy_dl_make_rule(
    const char *head_pred, const char *head_a1, const char *head_a2,
    const char *b1_pred, const char *b1_a1, const char *b1_a2,
    const char *b2_pred, const char *b2_a1, const char *b2_a2);

// Run semi-naive evaluation to fixpoint
int tardy_dl_evaluate(tardy_dl_program_t *prog);

// Query: can this atom be derived?
// Returns: 1 = yes (grounded), 0 = no (unknown), -1 = contradicted
int tardy_dl_query(const tardy_dl_program_t *prog,
                    const char *pred, const char *arg1, const char *arg2);

// Load the synthetic backbone rules
void tardy_dl_load_backbone(tardy_dl_program_t *prog);

// Load facts from N-Triples file
int tardy_dl_load_nt(tardy_dl_program_t *prog, const char *path);

#endif
```

**Step 2: Write datalog.c -- core engine**

The semi-naive evaluation:
```
1. delta = base facts
2. repeat:
     for each rule:
       for each way to match rule body against (facts + delta):
         if the head instantiation is NEW:
           add to new_delta
     delta = new_delta
     add new_delta to facts
   until delta is empty (fixpoint)
```

Key implementation details:
- Variables are strings starting with uppercase letter (checked by `isupper(s[0])`)
- Unification: if atom field is a variable, it matches anything and binds the value
- Two-body rules need join: match body[0] against facts, for each match try body[1]
- Duplicate detection: linear scan of facts (fast enough for 4K facts)

**Step 3: Build and test**

```bash
make clean && make
```

**Step 4: Commit**

```bash
git add src/ontology/datalog.h src/ontology/datalog.c Makefile
git commit -m "feat: Datalog inference engine -- facts + rules = derived knowledge"
```

---

### Task 2: Synthetic Backbone Rules

**Files:**
- Modify: `src/ontology/datalog.c` -- implement `tardy_dl_load_backbone()`

**Step 1: Implement the 15 backbone rules**

```c
void tardy_dl_load_backbone(tardy_dl_program_t *prog) {
    // Spatial
    // locatedIn(X, Y) :- capital(X, Y)
    // locatedIn(X, Y) :- capitalOf(X, Y)
    // contains(Y, X) :- locatedIn(X, Y)

    // Creation synonyms
    // createdBy(X, Y) :- creator(X, Y)
    // createdBy(X, Y) :- founder(X, Y)
    // createdBy(X, Y) :- inventor(X, Y)
    // createdBy(X, Y) :- discoverer(X, Y)

    // Reverse
    // creator(Y, X) :- createdBy(X, Y)
    // locatedIn(Y, X) :- contains(X, Y)

    // Temporal
    // createdIn(X, Y) :- dateCreated(X, Y)

    // Chain reasoning
    // locatedIn(X, Z) :- locatedIn(X, Y), locatedIn(Y, Z)
    // associatedWith(X, Z) :- createdBy(X, Y), locatedIn(Y, Z)
}
```

**Step 2: Build and test**

**Step 3: Commit**

---

### Task 3: Wire Datalog into Self-Hosted Ontology

**Files:**
- Modify: `src/ontology/self.c` -- replace substring matching with Datalog queries
- Modify: `src/ontology/self.h` -- add `tardy_dl_program_t` to the struct
- Modify: `src/mcp/server.c` -- initialize Datalog on ontology load

**Step 1: Add Datalog program to self-hosted ontology struct**

In `self.h`:
```c
#include "datalog.h"

typedef struct {
    tardy_vm_t          *vm;
    tardy_uuid_t         ontology_agent;
    int                  triple_count;
    bool                 initialized;
    tardy_dl_program_t   datalog;  // NEW
} tardy_self_ontology_t;
```

**Step 2: On ontology init, load backbone**

In `tardy_self_ontology_init()`:
```c
tardy_dl_init(&ont->datalog);
tardy_dl_load_backbone(&ont->datalog);
```

**Step 3: On triple add, also add to Datalog**

In `tardy_self_ontology_add()`:
```c
tardy_dl_add_fact(&ont->datalog, predicate, subject, object);
```

**Step 4: On load_nt, also load into Datalog and evaluate**

After loading N-Triples:
```c
tardy_dl_load_nt(&ont->datalog, path);
tardy_dl_evaluate(&ont->datalog);
```

**Step 5: Replace grounding with Datalog query**

In `tardy_self_ontology_ground()`, replace the child-name scanning loop:
```c
// OLD: ci_contains(child_name, norm_s) && ci_contains(child_name, norm_o)
// NEW: tardy_dl_query(&ont->datalog, norm_p, norm_s, norm_o)

int result = tardy_dl_query(&ont->datalog, triples[i].predicate,
                             norm_s, norm_o);
if (result == 1) {
    evidence++;
} else if (result == -1) {
    contradictions++;
}

// Also try with raw (un-normalized) names
if (evidence == 0) {
    result = tardy_dl_query(&ont->datalog, triples[i].predicate,
                             triples[i].subject, triples[i].object);
    if (result == 1) evidence++;
}
```

**Step 6: Build and test**

```bash
make clean && make && make run
```

**Step 7: Test the chain reasoning**

```bash
# This should now VERIFY via chain: capital(paris, france) -> locatedIn(paris, france)
tardy run "Paris is in France"
```

**Step 8: Commit**

---

### Task 4: Computational Verification

**Files:**
- Modify: `src/ontology/inference.c` -- the `tardy_inference_compute()` function already exists
- Modify: `src/mcp/server.c` -- call computational check in verify pipeline

**Step 1: Wire computational check into verify_claim**

Before the Datalog grounding, try computational verification:
```c
float comp_confidence = 0.0f;
int comp_result = tardy_inference_compute(claim_buf, claim_len, &comp_confidence);
if (comp_result == 1) {
    // Computational claim verified -- set grounding as fully grounded
    grounding.grounded = 1;
    grounding.count = 1;
    grounding.results[0].status = TARDY_KNOWLEDGE_GROUNDED;
    grounding.results[0].confidence = comp_confidence;
}
```

**Step 2: Build and test**

```bash
tardy run "The speed of light is 299792458 meters per second"
# Should VERIFY via known constants check
```

**Step 3: Commit**

---

### Task 5: Self-Growing via Verified Claims

**Files:**
- Modify: `src/mcp/server.c` -- after verification passes, add triples to Datalog + re-evaluate

**Step 1: After freeze, add to Datalog and re-derive**

The self-growing ontology code already adds triples to the self-hosted ontology. Add Datalog integration:
```c
if (srv->self_ontology_loaded) {
    for (int t = 0; t < triple_count; t++) {
        tardy_self_ontology_add(&srv->self_ontology, ...);
        tardy_dl_add_fact(&srv->self_ontology.datalog,
            all_triples[t].predicate,
            all_triples[t].subject,
            all_triples[t].object);
    }
    // Re-evaluate to derive new facts from the new additions
    tardy_dl_evaluate(&srv->self_ontology.datalog);
}
```

**Step 2: Build and test**

```bash
# Verify claim 1: "Paris is the capital of France"
# Then verify claim 2: "Paris is in France"
# Claim 2 should verify via rule derivation from claim 1
```

**Step 3: Commit**

---

### Task 6: Rule Mining (Learning from Patterns)

**Files:**
- Modify: `src/ontology/inference.c` -- `tardy_inference_learn()` already exists
- Modify: `src/mcp/server.c` -- call learning after verification

**Step 1: After successful verification, mine rules**

```c
if (verified) {
    tardy_inference_learn(&srv->ruleset, all_triples, triple_count);
}
```

**Step 2: Convert mined rules to Datalog rules**

When a new pattern is learned, add it to the Datalog program:
```c
if (learned > 0) {
    // New rules were mined -- re-evaluate Datalog
    tardy_dl_evaluate(&srv->self_ontology.datalog);
}
```

**Step 3: Build, test, commit**

---

### Task 7: Integration Test -- Chain Reasoning

**Files:**
- Create: `tests/test_datalog.sh` -- shell script testing the full chain

**Step 1: Write test script**

```bash
#!/bin/bash
# Test: load ontology, verify direct facts, verify derived facts

echo "=== Datalog Chain Reasoning Test ==="

# Direct fact: should verify
result=$(tardy run "Python was created by Guido van Rossum" 2>&1)
echo "Direct: $result"

# Derived fact: capital -> locatedIn
result=$(tardy run "Paris is in France" 2>&1)
echo "Derived (capital->locatedIn): $result"

# Chain: creator + locatedIn -> associatedWith
result=$(tardy run "Doctor Who is associated with London" 2>&1)
echo "Chain (creator+location): $result"

# Computational
result=$(tardy run "The speed of light is 299792458 meters per second" 2>&1)
echo "Computational: $result"

# Contradiction
result=$(tardy run "Tokyo is the capital of Germany" 2>&1)
echo "Contradiction: $result"
```

**Step 2: Run and verify**

**Step 3: Final commit**

```bash
git add -A && git commit -m "feat: Datalog backbone -- logical inference for verification

Semi-naive Datalog evaluation with 15 backbone rules.
Chain reasoning: capital -> locatedIn, creator -> createdBy -> associatedWith.
Computational verification for numeric claims.
Self-growing: verified claims become Datalog facts.
Rule mining: patterns from verifications become new rules."
```

---

### Execution Order

Tasks are sequential (each builds on the previous):

```
Task 1 (data structures) -> Task 2 (backbone rules) -> Task 3 (wire into ontology)
    -> Task 4 (computational) -> Task 5 (self-growing) -> Task 6 (rule mining)
    -> Task 7 (integration test)
```

**Estimated: ~600 lines of new C code.**
