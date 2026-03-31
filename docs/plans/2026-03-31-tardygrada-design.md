# Tardygrada: Formally Verified Agent Programming Language

## Design Document — 2026-03-31

## 1. Core Premise

Every value is a living agent. There are no variables, only agents holding values.
The language compiles to an MCP server. Programs don't print — they respond to callers with verified responses.

## 2. Agent-as-Value Model

```
let x: int = 5          // immutable agent holding 5
x: int = 5              // mutable agent holding 5 (no let)
let x: int = 5 @verified   // + SHA-256 hash check
let x: int = 5 @hardened   // + replicas + Byzantine vote
let x: int = 5 @sovereign  // + full BFT + ed25519 signatures
```

- `let` = immutable (mprotect enforced by OS/CPU)
- no `let` = mutable (provenance-tracked mutations)
- Reading a variable = asking the agent "what are you holding?"
- Errors are agents — you converse with them

## 3. Memory Model — Context Pointers

Context is addressable memory, not a sliding window.

- **Direct access**: `ctx_load r0, [x_ptr]` — O(1), two CPU instructions
- **Semantic access**: `agent.query("...")` — O(log n), vector DB lookup
- No degradation. Ever. It's an address, not a hope.

Parent agents are searchable context stores (like vector DBs).
Scoping IS agent hierarchy. Nested agents see parent context.

## 4. Tiered Immutability

| Level | Mechanism | Overhead | Corrupted by |
|-------|-----------|----------|-------------|
| `let` (default) | mprotect | ~0 | kernel exploit |
| `@verified` | mprotect + SHA-256 | ~50ns/read | kernel + SHA-256 break |
| `@hardened` | replicas + hash | ~150ns/read | majority kernel + SHA-256 |
| `@sovereign` | full BFT + ed25519 | ~500ns/read | all above + ed25519 break |

## 5. Truth Model

Truth is not boolean. It's a proof structure with strength:

**Axiomatic > Proven > Evidenced > Attested > Hypothetical > Contested > Refuted**

Every Fact carries: empirical basis, consistency proof, consensus record, provenance chain.
Programmer sets threshold per agent via `@semantics()`.

## 6. Hallucination — Formal Definition

A value typed as `Fact` with no evidence path to the ontology.

Three states: **Grounded** (evidence exists), **Unknown** (no data), **Contradicted** (ontology disproves it).

Two ontologies run in parallel: sketch (fast/permissive) and complete (slow/strict).

## 7. Laziness — Formal Definition

Difference between what the VM observed and what the agent claims.

The VM logs every operation independently (like a dashcam). Types:
NoWork, ShallowWork, FakeProof, CopiedWork, CircularVerification.

VM computes WorkSpec BEFORE task assignment (deterministic C, not LLM).

## 8. Consensus — Proof-Weighted, Not Averaged

Agents show proofs. Proofs compete. Strongest evidence wins regardless of vote count.
One expert with proof beats a million agents without.
Contested results reported as-is. No forced resolution.

## 9. 8-Layer Verification Pipeline

Runs on every LLM-produced Fact. Skipped for literals/arithmetic.

1. **Decompose** — text to triples (multiple independent agents + constrained generation)
2. **Ontology grounding** — triples vs knowledge graph (GraphRAG — 98% accuracy proven)
3. **Consistency check** — OWL reasoner for contradictions
4. **Probabilistic scoring** — quantitative confidence (MDP modelling)
5. **Protocol check** — session types compliance (Yoshida MPST)
6. **Formal certification** — proof-certificate asymmetry
7. **Cross-representation bridge** — all layers agree
8. **VM work verification** — laziness detection (AgentSpec — 95.56% precision)

Fail fast: one layer fails, stop, report which one.
Overall confidence = minimum across all layers.

## 10. Agent Lifecycle / GC

```
Born → Live → (idle) → Static → (needed) → Temp → (idle) → Static
```

- **Live**: full agent with provenance, context, constitution
- **Static**: just the value + ~100 byte JSON snapshot. Agent memory freed.
- **Temp**: resurrected from static, auto-demotes after TTL
- **@sovereign**: never demoted in memory, but dump to disk dict when idle
- **Tombstones**: dead agents leave hash proof for provenance chain integrity

## 11. Module System — Terraform, Not Import

No imports. Programs fork dependencies and verify in their own context.
Crate registry with cached verified forks.
Each program owns its agents, its memory, its trust chain.

## 12. Compilation Target

Programs compile to MCP servers. The world connects and asks questions.
Hello world:

```
agent HelloWorld {
    let greeting: str = "hello world"
}
```

Deploy. Connect. Ask. Get verified response with provenance.

## 13. Self-Healing

- No subagents alive = hard error (only fatal condition)
- Everything else: self-heal via repair agents, re-verification, re-grounding
- Debug with live rule miners + Rust-like testing

## 14. Formal Semantics — Thresholds Struct

A C struct with numerical bounds. Every guarantee is a tunable threshold.
Defaults are safe. ARIA research refines numbers over time.

```c
typedef struct {
    TruthSemantics truth;
    HallucinationSemantics hallucination;
    LazinessSemantics laziness;
    ImmutabilitySemantics immutability;
    LifecycleSemantics lifecycle;
    PipelineSemantics pipeline;
} TardygradaSemantics;
```

## 15. BFT Consensus — Coq Proven

Consensus protocol proven in Coq using Bythos framework (CCS 2024).
The foundation everything depends on — must be mathematically certain.

## 16. Implementation

- **Core VM**: C + inline assembly, <100KB binary
- **No stdlib, no malloc** — direct syscalls, custom allocator via mmap
- **Rust borrowed for**: OWL reasoner (open-ontologies as separate agent process), crypto
- **Ontology engine**: separate process, communicates over unix socket
- **Existing foundations**: open-ontologies (~10.5K lines), brain-in-the-fish (~24.7K lines)

## 17. Not Human-First

Autonomous system. Agents decide, verify, resolve.
Humans interact as agents, not as gods. No authority level above @sovereign.

## 18. Research Foundations

| Technique | Source | Status |
|-----------|--------|--------|
| Proof-certificate asymmetry | davidad / ARIA | Theoretical |
| Runtime enforcement | AgentSpec (ICSE 2026) | 95.56% precision |
| Constrained generation | Formal-LLM (PDA) | Published |
| Session types | Yoshida MPST | 16 years proven |
| Entity grounding | HalluGraph / GraphRAG | 98% accuracy |
| Agent MDP modelling | AgentGuard | Published |
| BFT verification | Bythos (CCS 2024) | Coq proven |
| KG hallucination reduction | Ontology-grounded GraphRAG | 1.7% hallucination rate |
