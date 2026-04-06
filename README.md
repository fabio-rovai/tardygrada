[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/tardygrada-logo.png" alt="Tardygrada" width="300">
</p>

# Tardygrada

**Pass a value through 100 agents in Python: it might be intact, but you have 0% proof. In Tardygrada: 100 handoffs, 100 hash-verified reads, tampering blocked by the OS kernel.**

| | Python / CrewAI / MiroFish | Tardygrada |
|---|---|---|
| 100 handoffs, no tampering | Value intact, **0% proof** | Value intact, **100% proof** |
| 100 handoffs, tamper at #50 | Value changed, **not detected** | **Blocked by OS** |
| "Just hash it yourself" | Attacker recomputes hash too | Hash in **read-only memory** (mprotect) |

246KB binary. Zero dependencies. Pure C11. Real ed25519 signatures. Coq-proven BFT consensus.

## What This Is

A programming language where every value carries cryptographic proof of who created it, when, and that it hasn't been tampered with. Programs compile to MCP servers. The OS kernel enforces immutability.

**What it guarantees:** integrity (mprotect + SHA-256 + ed25519 + BFT, proven in Coq), provenance (who, when, signed), audit trail (every operation logged).

**What it doesn't guarantee:** that a value is TRUE. Truth requires external knowledge. Tardygrada guarantees the value hasn't CHANGED. That's a different problem, and no other agent framework solves either.

## 30 Seconds

```bash
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make

tardy run "Python was created by Guido van Rossum"   # VERIFIED (85%)
tardy run "Paris is in France"                        # VERIFIED via Datalog chain reasoning
tardy run "The speed of light is 299792458 m/s"       # VERIFIED via computational check
tardy serve examples/medical.tardy                    # MCP server with 13 tools
tardy terraform /path/to/crewai                       # 153K lines -> 53 instructions
```

## The Language

```
agent MedicalAdvisor @sovereign @semantics(truth.min_confidence: 0.99) {
    invariant(trust_min: @verified)
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified
    let data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)
}
```

`receive()` accepts claims from external agents via MCP. `exec()` runs shell commands. `@sovereign` means mprotect + ed25519 + 5 BFT replicas. `invariant()` is checked on every operation. `coordinate` dispatches to the brain-in-the-fish debate engine.

## How It Verifies

```
Claim arrives -> decompose (60+ patterns) -> Datalog inference engine
    -> frame matching (structural constraints) -> CRDT merge check
    -> 8-layer pipeline -> BFT 3-pass consensus
    -> VERIFIED / CONSISTENT / CONFLICT / UNVERIFIABLE
```

Four outcomes, all deterministic:

- **VERIFIED**: Datalog derives it from known facts
- **CONSISTENT**: structurally valid, no conflicts (CRDT merge passes)
- **CONFLICT**: violates a functional dependency or contradicts known facts
- **UNVERIFIABLE**: no frame matches, honest "I can't check this"

The Datalog engine has 15 backbone inference rules. `capitalOf(Paris, France)` automatically derives `locatedIn(Paris, France)`. Self-growing: verified claims become new Datalog facts.

## Immutability Tiers

| Level | What Breaks It |
|-------|----------------|
| `let x = 5` | Kernel exploit (mprotect) |
| `@verified` | Kernel + SHA-256 preimage |
| `@hardened` | Corrupt majority of 3 replicas + SHA-256 |
| `@sovereign` | All above + forge ed25519 signature |

## Benchmarks

| Operation | Speed |
|-----------|-------|
| Read @verified (SHA-256 check) | 197ns / 5M ops/sec |
| Read @sovereign (BFT + sig) | 1,538ns / 650K ops/sec |
| Full verification pipeline | 692ns / 1.4M ops/sec |
| Message send between agents | 190ns / 5.3M ops/sec |

Real benchmark: 5 agents predict BTC price. MiroFish-style: 6 LLM calls, 10.6s. Tardygrada: 5 calls, 8.8s, with integrity proof.

## Evaluation

### Laziness Detection (60 traces, 10 edge cases)

How do you know your agent actually did its work? The VM observes every operation independently (the "dashcam"). Five laziness types, all detected:

| Type | What it catches | Detection |
|------|-----------------|-----------|
| NoWork | Agent produces output without doing anything | 100% |
| ShallowWork | Agent does minimal work below thresholds | 100% |
| FakeProof | Agent claims work but operations hash doesn't match | 100% |
| CopiedWork | Agent copies another agent's output (similarity > 0.95) | 100% |
| CircularVerification | Agent verifies itself in a circular chain | 100% |

Precision: 1.00 / Recall: 1.00 / F1: 1.00 (zero false positives on honest agents, including boundary edge cases).

### Compositional Hallucination Detection (500 cases, 5 difficulty tiers)

Existing detectors (SelfCheckGPT, FActScore) check claims one by one. They miss contradictions between claims that are each individually plausible. The OWL consistency layer catches these:

| Difficulty | Individual detector | Tardygrada pipeline |
|------------|:---:|:---:|
| Easy (direct opposites) | 0/25 | 25/25 (100%) |
| Medium (logical contradictions) | 0/25 | 25/25 (100%) |
| Hard (requires math/physics) | 0/25 | 24/25 (96%) |
| Subtle (domain knowledge) | 0/25 | 23/25 (92%) |
| Very subtle (statistical/methodological) | 0/25 | 22/25 (88%) |
| **Total** | **0/125** | **119/125 (95%)** |

Three detection layers: OWL consistency (logical contradictions), numeric verification (math/physics/rates), LLM-assisted decomposition (domain-specific implicit contradictions). Individual checking catches 0% of compositional contradictions. The pipeline catches 95%. The 6 remaining misses require true world knowledge (e.g., "copper sulfate is still a pesticide despite organic certification").

### Ablation

| Pipeline configuration | Accuracy |
|------------------------|:---:|
| Full (8 layers) | 100% |
| Remove probabilistic scoring | 75% |
| Remove consistency checking | 75% |

The probabilistic layer is the critical differentiator for partial-evidence cases.

### Scaling

| Agents | Total time |
|-------:|----------:|
| 5 | 0.6 ms |
| 50 | 3.3 ms |
| 500 | 21 ms |
| 5,000 | 97 ms |

Linear. Run `cd evaluation && make && make run` to reproduce.

## Three Projects, One Stack

```
Tardygrada (C, 246KB)         -- language, compiler, VM, MCP server, Datalog engine
brain-in-the-fish (Rust, 25K) -- debate, scoring, moderation (coordinate connects here)
open-ontologies (Rust, 10K)   -- OWL reasoning, SPARQL (grounded_in connects here)
```

## terraform

```bash
tardy terraform /path/to/any-agent-framework
# CrewAI:     153,245 lines -> 53 instructions
# LlamaIndex: 237,414 lines -> 15 instructions
# MiroFish:    21,016 lines -> 15 lines
```

## Building

```bash
make            # < 1 second, 246KB binary
make run        # tests
make bench      # benchmarks
```

C11 compiler. No malloc. Direct syscalls. Zero external libraries.

## License

MIT

## Research

Built on: [ARIA Safeguarded AI](https://www.aria.org.uk/programme/safeguarded-ai/), [AgentSpec](https://arxiv.org/abs/2503.18666) (ICSE 2026), [Agent Behavioral Contracts](https://arxiv.org/abs/2602.22302) (2026), [Bythos](https://arxiv.org/abs/2302.01527) (Coq BFT), Minsky frames (1974), CRDTs (Shapiro 2011), Datalog (1986).
