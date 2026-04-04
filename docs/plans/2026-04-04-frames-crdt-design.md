# Frames + CRDT + Datalog: Internal Consistency World Model

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace probabilistic grounding with algebraic consistency checking. Frames define structure, CRDTs guarantee merge correctness, Datalog derives consequences. All deterministic, no external data.

**Architecture:** Three layers that compose:
1. **Frames** (schema): what relationships exist and their constraints
2. **CRDT** (algebra): merge function derived from frame constraints
3. **Datalog** (inference): derive new facts from existing ones (already built)

A new fact arrives -> frame match -> dry-run CRDT merge -> Datalog derivation -> CONSISTENT / CONFLICT / DERIVED.

**Tech Stack:** C11, extends existing Datalog engine, ~300 lines new code.

---

## Data Model

```c
// A frame slot: one relationship in a frame
typedef struct {
    char name[64];       // slot name: "city", "country", "creator"
    char type[64];       // type constraint: "City", "Country", "Person"
    int  functional;     // 1 = one value per key, 0 = many values ok
    int  required;       // 1 = must be filled, 0 = optional
} tardy_frame_slot_t;

// A frame: structured expectation about a type of relationship
typedef struct {
    char name[64];                  // "Capital", "Creation", "Location"
    char predicate[64];             // which predicate this frame covers
    tardy_frame_slot_t slots[8];    // subject slot at [0], object slot at [1]
    int  slot_count;
    int  transitive;                // 1 = locatedIn(X,Y) + locatedIn(Y,Z) -> locatedIn(X,Z)
    int  symmetric;                 // 1 = relatedTo(X,Y) -> relatedTo(Y,X)
    int  inverse;                   // 1 = has an inverse relationship
    char inverse_pred[64];          // inverse predicate name
} tardy_frame_t;

// Frame registry
#define TARDY_MAX_FRAMES 64

typedef struct {
    tardy_frame_t frames[TARDY_MAX_FRAMES];
    int           count;
} tardy_frame_registry_t;
```

## CRDT Merge Semantics

```c
typedef enum {
    TARDY_MERGE_OK,        // merges cleanly, no conflict
    TARDY_MERGE_DUPLICATE, // already exists, no change
    TARDY_MERGE_CONFLICT,  // violates functional dependency
    TARDY_MERGE_DERIVED,   // already derivable from existing facts
} tardy_merge_result_t;

// Dry-run merge: would this fact conflict with existing state?
// Does NOT modify state. Pure query.
tardy_merge_result_t tardy_crdt_dry_merge(
    const tardy_frame_registry_t *frames,
    const tardy_dl_program_t *datalog,
    const tardy_dl_atom_t *new_fact);

// Actual merge: add fact if consistent, reject if conflict
tardy_merge_result_t tardy_crdt_merge(
    const tardy_frame_registry_t *frames,
    tardy_dl_program_t *datalog,
    const tardy_dl_atom_t *new_fact);
```

## Synthetic Backbone Frames

Pre-loaded at startup:

```
Frame: Capital
  predicate: capitalOf
  slots: [(city, City, functional=1), (country, Country, functional=1)]
  inverse: capitalCity
  -- One capital per country. One country per capital.

Frame: Location
  predicate: locatedIn
  slots: [(entity, Thing, functional=0), (place, Place, functional=0)]
  transitive: yes
  inverse: contains
  -- Many things in one place. Containment is transitive.

Frame: Creation
  predicate: creator
  slots: [(creation, Thing, functional=0), (agent, Agent, functional=0)]
  inverse: createdBy
  -- Things can have multiple creators.

Frame: Founding
  predicate: founder
  slots: [(organization, Organization, functional=0), (agent, Agent, functional=0)]
  inverse: foundedBy

Frame: Temporal
  predicate: dateCreated
  slots: [(thing, Thing, functional=1), (date, Date, functional=1)]
  -- Each thing has one creation date.

Frame: Type
  predicate: type
  slots: [(instance, Thing, functional=0), (class, Class, functional=0)]
  transitive: no
  -- Multiple types per thing.

Frame: Description
  predicate: description
  slots: [(thing, Thing, functional=0), (text, Text, functional=0)]

Frame: KnownFor
  predicate: knownFor
  slots: [(agent, Agent, functional=0), (achievement, Thing, functional=0)]
```

## Type Learning

Types are NOT pre-defined. They emerge from usage.

When the system sees `creator(Python, GuidoVanRossum)`:
- Python is assigned type ?a
- GuidoVanRossum is assigned type ?b
- creator frame says slot[0] type is "Thing", slot[1] type is "Agent"
- Unify: Python : Thing, GuidoVanRossum : Agent

After 10 creator facts:
- All slot[0] values are typed as Thing
- All slot[1] values are typed as Agent
- New claim `creator(Linux, LinusTorvalds)`: both slots type-check immediately

Types are stored as Datalog facts:
```
type(Python, Thing).
type(GuidoVanRossum, Agent).
type(Paris, City).        -- from capitalOf frame
type(France, Country).    -- from capitalOf frame
```

## Verification Flow

```
Claim: "Berlin is the capital of Germany"
Decomposed: capitalOf(Berlin, Germany)

1. FRAME MATCH: capitalOf -> Capital frame
   slots: (city=Berlin, country=Germany)

2. TYPE CHECK:
   - Germany: seen before? Yes, type=Country. Slot expects Country. OK.
   - Berlin: seen before? No. Slot expects City. UNRESOLVED.
   - Result: PARTIAL TYPE MATCH (1/2 slots confirmed)

3. DRY-RUN MERGE:
   - Capital frame is functional on country: one capital per country.
   - Does Germany already have a capital in the Datalog? No.
   - No conflict. Merge would succeed.
   - Result: MERGE_OK

4. DATALOG DERIVE:
   - capitalOf(Berlin, Germany) would trigger rule:
     locatedIn(Berlin, Germany) [from backbone rule]
   - No contradictions with existing facts.
   - Result: DERIVATION_CLEAN

5. FINAL VERDICT:
   - Frame: matched
   - Types: 1/2 confirmed (Germany=Country), 1/2 unresolved (Berlin=?City)
   - Merge: no conflict
   - Derivation: clean
   -> CONSISTENT (structurally valid, no conflicts, one unresolved type)
```

Compare with a FALSE claim:

```
Claim: "Tokyo is the capital of Germany"
Decomposed: capitalOf(Tokyo, Germany)

1. FRAME MATCH: capitalOf -> Capital frame. OK.

2. TYPE CHECK:
   - Germany: type=Country. OK.
   - Tokyo: type=City (from capitalOf(Tokyo, Japan)). OK.

3. DRY-RUN MERGE:
   - If capitalOf(Berlin, Germany) was previously verified:
     Capital frame is functional on country. Germany already has Berlin.
     CONFLICT.
   - If NO capital for Germany exists:
     No conflict. But Tokyo already is capitalOf Japan.
     Capital frame is functional on city too (one country per capital).
     CONFLICT: Tokyo is already Japan's capital.

4. FINAL VERDICT: CONFLICT (functional dependency violation)
```

## Integration with Existing Code

The frame registry and CRDT merge replace the grounding confidence scoring:

```
Old: ground triples -> count evidence -> compute confidence -> threshold check
New: ground triples -> frame match -> CRDT merge check -> CONSISTENT/CONFLICT/DERIVED
```

No probabilities. No thresholds. No confidence scores. Three deterministic outcomes.

The pipeline layers still run but the grounding layer (Layer 2) changes from "count matching triples" to "check CRDT merge result."

## Implementation Tasks

### Task 1: Frame data structures + registry + backbone frames
- Create: `src/ontology/frames.h`, `src/ontology/frames.c`
- Load 8 backbone frames at startup

### Task 2: CRDT merge function
- Implement `tardy_crdt_dry_merge` and `tardy_crdt_merge`
- Check functional dependencies, type compatibility

### Task 3: Type learning from verified facts
- When a fact is added, extract types from frame slot constraints
- Store types as Datalog facts

### Task 4: Wire into verification pipeline
- Replace confidence-based grounding with frame + CRDT check
- Pipeline outputs: CONSISTENT / CONFLICT / DERIVED / UNRESOLVABLE

### Task 5: Integration test
- Test chain reasoning, functional dep conflicts, type learning
