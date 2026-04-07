[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C11-Pure-green.svg)]()
[![Binary](https://img.shields.io/badge/Binary-280KB-orange.svg)]()
[![Deps](https://img.shields.io/badge/Dependencies-Zero-brightgreen.svg)]()

<p align="center">
  <img src="docs/tardygrada-logo.png" alt="Tardygrada" width="300">
</p>

<h3 align="center">Formally verified agent runtime — every value is a living agent with cryptographic proof</h3>

<p align="center">
  280KB binary. Zero dependencies. Pure C11. Programs compile to MCP servers.<br>
  Laziness detection. Compositional hallucination catching. OS-enforced immutability.
</p>

---

## The Problem

[![SafeSkill 92/100](https://img.shields.io/badge/SafeSkill-92%2F100_Verified%20Safe-brightgreen)](https://safeskill.dev/scan/fabio-rovai-tardygrada)

Pass a value through 100 agents in Python: it might be intact, but you have **0% proof**. In Tardygrada: 100 handoffs, 100 hash-verified reads, tampering blocked by the OS kernel.

| | Python / CrewAI / LangGraph | Tardygrada |
|---|---|---|
| 100 handoffs, no tampering | Value intact, **0% proof** | Value intact, **100% proof** |
| 100 handoffs, tamper at #50 | Value changed, **not detected** | **Blocked by OS kernel** |
| "Just hash it yourself" | Attacker recomputes hash too | Hash in **read-only memory** (mprotect) |
| "Did the agent do its work?" | No way to know | **VM dashcam catches lazy agents** |
| "Are claims consistent?" | Check one by one (miss contradictions) | **OWL consistency catches 95%** |

---

## Quick Start

```bash
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make
# Built: tardygrada (280KB), < 1 second

tardy run "Python was created by Guido van Rossum"   # VERIFIED (85%)
tardy run "Paris is in France"                        # VERIFIED via Datalog chain
tardy run "The speed of light is 299792458 m/s"       # VERIFIED via computational check
tardy serve examples/medical.tardy                    # MCP server with 13 tools
tardy terraform /path/to/crewai                       # 153K lines -> 53 instructions
```

---

## How It Works

| Path | Description |
|------|-------------|
| **Language** | Every value is an agent. `let x = 5` spawns an immutable micro-agent. Programs compile to MCP servers. |
| **Verification** | 8-layer pipeline: decompose → ground → consistency → probabilistic → protocol → certification → cross-rep → work verify |
| **Immutability** | 5 tiers enforced by OS: mprotect → SHA-256 → replicas → ed25519 → BFT consensus |
| **Laziness** | VM independently observes all operations (dashcam model). Catches: NoWork, ShallowWork, FakeProof, CopiedWork, CircularVerification |
| **Hallucination** | OWL consistency + numeric verification + LLM decomposition. Catches contradictions between individually plausible claims |

<details>
<summary><b>The Language</b></summary>

```
agent MedicalAdvisor @sovereign @semantics(truth.min_confidence: 0.99) {
    invariant(trust_min: @verified)
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified
    let data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)
}
```

- `receive()` accepts claims from external agents via MCP
- `exec()` runs shell commands, captures stdout
- `@sovereign` = mprotect + ed25519 + 5 BFT replicas
- `invariant()` checked on every operation
- `coordinate` dispatches to the [brain-in-the-fish](https://github.com/fabio-rovai/brain-in-the-fish) debate engine

</details>

<details>
<summary><b>Verification Pipeline</b></summary>

```
Claim arrives → decompose (60+ patterns) → Datalog inference engine
    → frame matching (structural constraints) → CRDT merge check
    → 8-layer pipeline → BFT 3-pass consensus
    → VERIFIED / CONSISTENT / CONFLICT / UNVERIFIABLE
```

Four outcomes, all deterministic:

- **VERIFIED**: Datalog derives it from known facts
- **CONSISTENT**: structurally valid, no conflicts (CRDT merge passes)
- **CONFLICT**: violates a functional dependency or contradicts known facts
- **UNVERIFIABLE**: no frame matches, honest "I can't check this"

The Datalog engine has 15 backbone inference rules. `capitalOf(Paris, France)` automatically derives `locatedIn(Paris, France)`. Self-growing: verified claims become new Datalog facts.

</details>

<details>
<summary><b>Immutability Tiers</b></summary>

| Level | Enforcement | Verification Cost | What Breaks It |
|-------|-------------|-------------------|----------------|
| `let x = 5` | mprotect(PROT_READ) | 0ns | Kernel exploit |
| `@verified` | mprotect + SHA-256 | 197ns | Kernel + hash preimage |
| `@hardened` | mprotect + N replicas + majority vote | ~500ns | Corrupt majority + hash |
| `@sovereign` | mprotect + ed25519 + 5 replicas + BFT | 1,538ns | All above + forge signature |

</details>

<details>
<summary><b>Terraform — Convert Any Agent Framework</b></summary>

```bash
tardy terraform /path/to/any-agent-framework
# CrewAI:     153,245 lines → 53 instructions
# LlamaIndex: 237,414 lines → 15 instructions
# MiroFish:    21,016 lines → 15 lines
```

Reads entire agent framework codebases, extracts the logic, converts to Tardygrada instructions.

</details>

---

## Evaluation Results

### Laziness Detection

How do you know your agent actually did its work? The VM observes every operation independently. Five laziness types, all detected:

| Type | What It Catches | Detection |
|------|-----------------|:---------:|
| NoWork | Agent produces output without doing anything | **100%** |
| ShallowWork | Minimal work below thresholds | **100%** |
| FakeProof | Claims work but operations hash is invalid | **100%** |
| CopiedWork | Copies another agent's output (similarity > 0.95) | **100%** |
| CircularVerification | Verifies itself in a circular chain | **100%** |

> **F1: 1.00** — 60 traces, 10 edge cases, zero false positives on honest agents. No prior work formalizes agent laziness detection.

### Compositional Hallucination Detection

Existing detectors check claims one by one. They miss contradictions between individually plausible claims. Three detection layers: OWL consistency, numeric verification, LLM-assisted decomposition.

| | Individual | SelfCheck | FActScore | **Tardygrada** |
|---|:-:|:-:|:-:|:-:|
| Synthetic (125 compositional) | 0% | 59% | 0% | **95%** |
| ContraDoc (449 real documents) | — | 9% | 5% | **10%** |
| HaluEval (500 real responses) | — | F1: 0.32 | F1: 0.00 | **F1: 0.32** |
| "Are you sure?" (LLM baseline) | — | — | — | — |
| ContraDoc 50-doc subset | — | — | — | **10% vs LLM 4%** |

> **On designed compositional contradictions**: 95% detection where individual checking gets 0%.
> **On external data (ContraDoc)**: modest 10% recall — but deterministic, 12ms/doc, no LLM calls needed. GPT-4 gets 34.7% on the same dataset.

<details>
<summary><b>Per-difficulty breakdown (synthetic)</b></summary>

| Difficulty | Pipeline |
|---|:-:|
| Easy (direct opposites) | 100% |
| Medium (logical contradictions) | 100% |
| Hard (math/physics) | 96% |
| Subtle (domain knowledge) | 92% |
| Very subtle (statistical) | 88% |

</details>

<details>
<summary><b>Ablation study</b></summary>

| Configuration | Accuracy |
|---|:-:|
| Full pipeline (8 layers) | 100% |
| Remove probabilistic scoring | 75% |
| Remove consistency checking | 75% |

The probabilistic layer is the critical differentiator for partial-evidence cases.

</details>

### Performance

| Operation | Speed |
|-----------|------:|
| Read @verified (SHA-256 check) | 197ns / 5M ops/sec |
| Read @sovereign (BFT + sig) | 1,538ns / 650K ops/sec |
| Full verification pipeline | 692ns / 1.4M ops/sec |
| Message send between agents | 190ns / 5.3M ops/sec |

| Agents | Total Time |
|-------:|-----------:|
| 5 | 0.6 ms |
| 50 | 3.3 ms |
| 500 | 21 ms |
| 5,000 | 97 ms |

Linear scaling. Run `cd evaluation && make && make run` to reproduce all benchmarks.

---

## Architecture

```
Tardygrada (C, 280KB)         — language, compiler, VM, MCP server, Datalog engine
brain-in-the-fish (Rust, 25K) — debate, scoring, moderation (coordinate connects here)
open-ontologies (Rust, 10K)   — OWL reasoning, SPARQL (grounded_in connects here)
```

<details>
<summary><b>VM internals</b></summary>

| Component | Lines | What It Does |
|-----------|------:|--------------|
| VM core | 6,229 | Agent spawn/read/write/kill, GC, lifecycle |
| Verification | 2,163 | 8-layer pipeline, decomposition, work verification |
| Ontology | 2,487 | Datalog inference, Minsky frames, CRDT merge |
| Compiler | 1,598 | .tardy → VM instructions, terraform converter |
| MCP server | 2,133 | JSON-RPC, MCP protocol, tool exposure |
| Crypto | — | SHA-256, ed25519 via Monocypher (embedded) |

</details>

<details>
<summary><b>Ontology configuration</b></summary>

Tardygrada supports two ontology engines:

| Engine | What It Is | When To Use |
|--------|------------|-------------|
| **open-ontologies** (bridge) | Full OWL reasoning via Unix socket | Production — needs open-ontologies running |
| **Self-hosted** (built-in) | Datalog inference + Minsky frames | Standalone — no external deps |

Control via `TARDY_ONTOLOGY` env var:
- `both` (default) — open-ontologies preferred, self-hosted fallback
- `bridge` — open-ontologies only, fail if unavailable
- `self` — self-hosted only
- `none` — skip ontology

</details>

---

## Building

```bash
make            # < 1 second, 280KB binary
make run        # tests
make bench      # benchmarks
```

C11 compiler. No malloc. Direct syscalls. Zero external libraries.

### Reproduce Evaluation

```bash
cd evaluation && make
./laziness_bench           # 60 traces, F1 1.00
./hallucination_bench      # 500 cases, 95% compositional
./scaling_bench            # 5→5000 agents, linear
./ablation_bench           # layer-by-layer analysis
./contradoc_bench          # 891 real documents (external)
./halueval_bench           # 500 HaluEval examples (external)
```

---

## Research

Built on: [ARIA Safeguarded AI](https://www.aria.org.uk/programme/safeguarded-ai/), [AgentSpec](https://arxiv.org/abs/2503.18666) (ICSE 2026), [Agent Behavioral Contracts](https://arxiv.org/abs/2602.22302) (2026), [Bythos](https://arxiv.org/abs/2302.01527) (Coq BFT), Minsky frames (1974), CRDTs (Shapiro 2011), Datalog (1986).

Evaluated against: [SelfCheckGPT](https://arxiv.org/abs/2303.08896) (EMNLP 2023), [FActScore](https://aclanthology.org/2023.emnlp-main.741/) (EMNLP 2023), [ContraDoc](https://aclanthology.org/2024.naacl-long.362/) (NAACL 2024), [HaluEval](https://huggingface.co/datasets/pminervini/HaluEval).

Related: [Mundler et al.](https://arxiv.org/abs/2305.15852) (ICLR 2024) — self-contradiction detection, [Fang et al.](https://arxiv.org/abs/2409.11283) (AAAI 2025) — graph-based triple consistency, [He et al.](https://arxiv.org/abs/2601.13600) (2026) — global consistency theory.

---

## License

MIT
