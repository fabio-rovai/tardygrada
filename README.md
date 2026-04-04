[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/tardygrada-logo.png" alt="Tardygrada" width="300">
</p>

# Tardygrada

**Trust infrastructure for AI agents.** Know WHO produced a value, WHEN, and that it hasn't been tampered with.

Every agent framework today accepts output at face value. You ask an LLM agent to research something and you get text back. No proof of who produced it. No guarantee it hasn't been modified. No chain of custody. No audit trail.

Tardygrada fixes this. Every value is a living agent with cryptographic provenance. Once frozen, a value cannot be changed -- not by your code, not by a bug, not by a malicious agent. The OS kernel enforces it.

```
229KB binary | Zero dependencies | Pure C11 | Real ed25519 | Coq-proven BFT
```

## What Tardygrada Guarantees (and What It Doesn't)

**Guaranteed (mathematically proven):**

- **Integrity**: once a value is frozen, it CANNOT change. mprotect (hardware-enforced) + SHA-256 + ed25519 + BFT replicas. Proven in Coq.
- **Provenance**: every value carries WHO created it, WHEN, from WHAT operation, signed with ed25519.
- **Audit trail**: every mutation, every message, every verification attempt is logged in an immutable conversation history.
- **Tamper detection**: if a replica is corrupted, the BFT consensus detects it and the system self-heals from honest replicas.

**Not guaranteed (requires external knowledge):**

- **Correctness**: Tardygrada can tell you "this value was produced by agent X, verified by the pipeline, and hasn't changed." It cannot tell you "this value is true." Truth requires knowledge, and knowledge comes from ontologies, which are always incomplete.

This is the honest difference between CrewAI and Tardygrada. CrewAI's "reviewer agent" is an LLM checking itself -- it says "VERIFIED" but has no proof. Tardygrada says "this value has integrity -- here's the cryptographic evidence" and is honest when it can't verify content.

## Quick Start

```bash
# Build (< 1 second)
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make

# Check integrity of a claim
tardy run "Doctor Who was created at BBC Television Centre in 1963"

# Convert any agent framework to Tardygrada
tardy terraform /path/to/crewai

# Run a .tardy program as an MCP server
tardy serve examples/medical.tardy
```

## The Language

```
agent MedicalAdvisor @sovereign @semantics(
    truth.min_confidence: 0.99,
) {
    invariant(trust_min: @verified)

    // Claims from external agents -- integrity-checked before freezing
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified

    // Shell commands as agent bodies
    let patient_data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")

    // Multi-agent coordination
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)

    let name: str = "Tardygrada Medical" @sovereign
}
```

What each keyword does:
- `receive()` -- pending slot filled by external agents via MCP. Mutable until verified.
- `exec()` -- forks /bin/sh, captures stdout, stores as agent value. No API calls.
- `grounded_in()` -- checks claims against an ontology. Returns GROUNDED, UNKNOWN, or CONTRADICTED.
- `@sovereign` -- mprotect + ed25519 + SHA-256 + 5 BFT replicas. Hardware-enforced immutability.
- `invariant()` -- constitution rules checked on every operation. Cannot be bypassed.
- `coordinate` -- dispatches tasks to brain-in-the-fish debate engine.
- `fork` -- imports another .tardy file into current context. Trust cannot escalate.

## How Verification Works

```
External agent submits a claim via MCP
        |
    Decompose into structured triples
        |-- Preprocessor: strip markdown, extract key-value pairs
        |-- Rule engine: 60+ English patterns, 3 independent passes
        |-- Optional: LLM-assisted decomposition (TARDY_LLM_DECOMPOSE=1)
        |
    Ground against ontology (if available)
        |-- Self-hosted: triples as @sovereign agents (200ns, in-process)
        |-- External: open-ontologies via unix socket (SPARQL + OWL)
        |-- No ontology: honest UNKNOWN (never fakes confidence)
        |
    8-layer verification pipeline
        |-- Decomposition agreement, ontology grounding, consistency,
        |-- probabilistic scoring, protocol check, certification,
        |-- cross-layer contradiction detection, work verification
        |
    BFT consensus: 3 independent passes, 2/3 must agree
        |
    Passed -> frozen with provenance (integrity guaranteed)
    Failed -> 11 structured failure types + feedback-driven retry
```

**Integrity vs Correctness:**
- The pipeline checks claim structure, consistency, ontology grounding (when available), and agent work.
- It guarantees the claim has INTEGRITY (provenance, immutability, tamper detection).
- It does NOT guarantee the claim is TRUE unless the ontology has supporting data.
- When the ontology lacks data, the system reports UNKNOWN -- never VERIFIED.

## Tiered Immutability

| Level | Mechanism | What Breaks It |
|-------|-----------|----------------|
| `x: int = 5` | Provenance-tracked | Any write (logged) |
| `let x: int = 5` | mprotect (OS kernel) | Kernel exploit |
| `let x = 5 @verified` | + SHA-256 hash | Kernel + SHA-256 preimage |
| `let x = 5 @hardened` | + 3 BFT replicas | Corrupt majority + SHA-256 |
| `let x = 5 @sovereign` | + ed25519 + 5 replicas | All above + forge ed25519 |

## Real Benchmark: CrewAI vs Tardygrada

Same task (Tesla investment analysis), same LLM (Claude Sonnet), both run for real:

| | CrewAI | Tardygrada |
|---|---|---|
| Time | 38s | 15s |
| LLM calls | 2+ (sequential agents) | 1 |
| Output | 2000 chars | 1846 chars |
| Integrity proof | None | ed25519 + SHA-256 + mprotect |
| Provenance | None | WHO, WHEN, audit trail |
| Dependencies | 30+ packages | Zero (229KB binary) |

CrewAI's reviewer agent added `[RISK FLAG: UNVERIFIED]` -- the LLM admitting it can't verify itself. Tardygrada provides cryptographic proof of integrity: who produced the value, when, and that it hasn't changed.

## Three Projects, One Stack

```
Tardygrada (C, 229KB)          -- language, compiler, VM, MCP server
        |
brain-in-the-fish (Rust, 25K) -- debate, scoring, moderation
        |                         coordinate keyword connects here
open-ontologies (Rust, 10K)    -- OWL reasoning, SPARQL, knowledge graphs
                                  grounded_in() connects here
```

## terraform

```bash
tardy terraform /path/to/any-agent-framework
# CrewAI:     153,245 lines -> 53 instructions
# LlamaIndex: 237,414 lines -> 15 instructions
# LangGraph:  101,662 lines -> 39 instructions
# MetaGPT:     89,734 lines -> 11 instructions
```

## Benchmarks

```
Read @verified (SHA-256):       197ns    5,000,000 ops/sec
Read @sovereign (BFT+sig):   1,538ns      650,000 ops/sec
Verification pipeline:          692ns    1,400,000 ops/sec
Message send:                   190ns    5,300,000 ops/sec
```

## Building

```bash
make        # < 1 second
make run    # run tests
make bench  # run benchmarks
```

C11 compiler. No external libraries. No malloc. Direct syscalls only.

## License

MIT. See [LICENSE](LICENSE).

## Research Foundations

Built on: [ARIA Safeguarded AI](https://www.aria.org.uk/programme/safeguarded-ai/) (formal verification for AI), [HalluGraph](https://arxiv.org/abs/2406.12072) (knowledge-graph grounding), [AgentSpec](https://arxiv.org/abs/2401.13178) (runtime agent enforcement), [Bythos](https://arxiv.org/abs/2302.01527) (Coq-proven BFT consensus).
