# Datalog Backbone Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace substring matching in the self-hosted ontology with a Datalog inference engine. Facts + rules = derived knowledge. Verification becomes "can this claim be logically derived?"

**Architecture:** Bottom-up evaluation (semi-naive). Facts are ground atoms. Rules are Horn clauses with only variables and constants. No function symbols = guaranteed termination. The engine lives inside the VM as a set of agents.

## Data Model

```c
// An atom: predicate(arg1, arg2)
// e.g., capital(paris, france), creator(doctorWho, sydneyNewman)
typedef struct {
    char predicate[64];
    char arg1[128];  // subject
    char arg2[128];  // object
} tardy_atom_t;

// A rule: head :- body1, body2, ...
// e.g., locatedIn(X, Y) :- capital(X, Y)
// Variables are strings starting with uppercase: "X", "Y"
typedef struct {
    tardy_atom_t head;       // what we derive
    tardy_atom_t body[4];    // conditions (max 4 atoms in body)
    int          body_count;
} tardy_rule_t;

// The Datalog program
typedef struct {
    tardy_atom_t facts[4096];   // ground facts
    int          fact_count;
    tardy_rule_t rules[128];    // inference rules
    int          rule_count;
    int          derived_start; // index where derived facts begin
} tardy_datalog_t;
```

## Synthetic Backbone Rules

Pre-loaded at startup. These encode structural reasoning:

```
% Spatial
locatedIn(X, Y) :- capital(X, Y).
locatedIn(X, Y) :- location(Y, X).        % reverse
contains(Y, X) :- locatedIn(X, Y).        % inverse

% Creation
createdBy(X, Y) :- creator(X, Y).         % synonym
createdBy(X, Y) :- founder(X, Y).         % founder = creator
createdBy(X, Y) :- inventor(X, Y).        % inventor = creator
createdIn(X, Y) :- dateCreated(X, Y).     % temporal creation

% Transitivity
locatedIn(X, Z) :- locatedIn(X, Y), locatedIn(Y, Z).

% Derivation chain: if X was created by Y, and Y is in Z, then X is associated with Z
associatedWith(X, Z) :- createdBy(X, Y), locatedIn(Y, Z).
```

## Semi-Naive Evaluation

```
1. Start with base facts (loaded ontology + verified claims)
2. Apply all rules to derive new facts
3. Only process NEW facts from the previous round (semi-naive)
4. Repeat until no new facts are derived (fixpoint)
5. Answer query: is the claim in the derived fact set?
```

Termination is guaranteed because:
- Finite set of constants (from facts)
- Finite set of predicates (from rules)
- No function symbols (no new terms can be created)
- Maximum possible facts = predicates * constants^2

## Integration with Verify Pipeline

Replace `tardy_self_ontology_ground()` internals:

```
Old: for each triple, substring-match against agent names
New: for each triple, convert to atom, query Datalog program
```

The Datalog engine runs inside the VM. Facts are still @sovereign agents. Rules are the synthetic backbone. Derived facts are cached as new agents.

## Self-Growing

When a claim is verified:
1. Add all its triples as new Datalog facts
2. Re-run semi-naive evaluation (only processes new facts, fast)
3. New derived facts become available for future queries

## Rule Mining

When multiple claims verify with the same pattern:
1. Extract the predicate co-occurrence: "creator and locatedIn often appear together"
2. Generate a candidate rule: `associatedWith(X, Z) :- creator(X, Y), locatedIn(Y, Z)`
3. Test rule against known facts (does it derive anything contradicted?)
4. If clean, add to ruleset

## Computational Verification

Separate from Datalog. If a claim contains numbers:
1. Check against known constants (speed of light, pi, etc.)
2. If it contains math operators, exec() python to compute
3. Compare result to claimed value

## Success Criteria

- `tardy run "Paris is the capital of France"` -> VERIFIED (via Datalog: capital(paris, france) in facts)
- `tardy run "Paris is in France"` -> VERIFIED (via Datalog: locatedIn(paris, france) derived from capital rule)
- `tardy run "Doctor Who is associated with London"` -> VERIFIED (via chain: creator -> locatedIn -> associatedWith)
- `tardy run "The speed of light is 299792458 m/s"` -> VERIFIED (via computational check)
- `tardy run "Tokyo is the capital of Germany"` -> NOT VERIFIED (capital(tokyo, germany) not derivable, capital(tokyo, japan) exists -> potential contradiction)
