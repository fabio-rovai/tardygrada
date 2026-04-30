[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

<p align="center">
  <img src="tardygrada-logo.png" alt="Tardygrada" width="200">
</p>

<h3 align="center">The school where AI agents go to specialize.</h3>
<p align="center">A tiny C runtime that turns generic LLM agents into verified specialists, and checks their work on the way out.</p>

---

## What this is

**Two things in one 350 KB C binary.**

1. **A verification runtime.** Catches LLM contradictions, agent traces that *say* they did work but didn't, and tampered intermediate data. Plugs into Claude Code, Cursor, or Qwen Code as an MCP server.

2. **A specialization layer.** A small language (`.tardy`) for writing verifiable, agent-shaped programs. Compiled `.tardy` files run as MCP servers themselves — a "curriculum" your generic agent can pick up to behave as a domain specialist.

The mental model:

| | |
|---|---|
| Agents | the students |
| `.tardy` programs | the curriculum |
| 8-layer verification pipeline | the exam |
| Persistent daemon + memory palace | the campus |
| MCP bridge | the front gate |

A generic Claude agent walks in. You point it at a `.tardy` curriculum and the verification pipeline. It walks out doing the work it was sent to do, with every claim grounded and every step audit-logged.

---

## Quick start

```bash
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make
# Builds in ~3 seconds. ~350 KB binary. Zero runtime deps.

# Try the verifier on a document
./tardygrada verify-doc README.md

# Start the persistent daemon
./tardygrada daemon start
./tardygrada status
```

**Plug into Claude Code** (MCP):

```json
{
  "mcpServers": {
    "tardygrada": {
      "command": "/path/to/tardygrada",
      "args": ["mcp-bridge"]
    }
  }
}
```

The bridge exposes five tools: `verify_claim`, `verify_document`, `spawn_agent`, `read_agent`, `daemon_status`. Then in Claude: *"verify this document for contradictions"*.

**Plug into Qwen Code** (newline-delimited JSON-RPC adapter):

```json
{
  "mcpServers": {
    "tardygrada": {
      "command": "/bin/bash",
      "args": ["/path/to/tardygrada/hooks/targy-mcp-wrapper.sh"]
    }
  }
}
```

---

## What works today (honest list)

| Capability | Status | Number |
|---|---|---|
| Real-document contradiction detection (ContraDoc, 891 docs) | Real benchmark on real data | **F1 0.58** |
| Agent-trajectory hallucination detection (AgentHallu, 693 trajectories) | Real benchmark on real data | **F1 0.58** |
| Laziness / fake-work detection (synthetic adversarial traces, 100) | Synthetic, designed-for-it | **F1 0.92, 0 false positives** |
| Memory tampering detection (OS mprotect + SHA-256 + ed25519) | Real OS-level enforcement | n/a |
| VM scaling (5 → 5,000 agents) | Real | ~92 ms total |
| MCP bridge (Claude Code, Qwen Code, Cursor) | Real, four of five tools fully wired (see below) | — |
| `tardy verify-doc` over MCP | Wired to the real pipeline as of v2.0 | — |

---

## What does NOT work, or works differently than you'd expect

**This section exists because previous READMEs hand-waved over these. They don't anymore.**

- **HaluEval F1 0.03.** This benchmark tests single-sentence factual errors against world knowledge ("Paris is the capital of Germany"). That is a retrieval problem, not a verification problem, and Tardygrada is bad at it. We catch claims that contradict *each other* or contradict an attached ontology — not isolated false statements. Use a retrieval-augmented LLM for HaluEval-style tasks.

- **`tardy terraform` is a skeleton extractor, not a framework rewrite.** It scans an existing CrewAI / LangGraph / LlamaIndex / AutoGen / etc. repo and emits an agent-shaped `.tardy` skeleton with stubbed tool bodies. Each tool body must be wired to a real implementation by the user. Treat the output as migration scaffolding around the verification pipeline, not as a working drop-in for the framework. Files in `terraform_prs/` are demonstration scaffolds, not running ports.

- **The lexical baseline in benchmarks is NOT SelfCheckGPT.** The in-repo `lexical_baseline_evaluate` is a deterministic hand-coded antonym/negation heuristic. It is *not* the published SelfCheckGPT (which uses LLM sampling + NLI). The legacy name `selfcheck_evaluate` and the column header `SelfCheck` were renamed in v2.0; benchmark output now prints `LexBase`. See `evaluation/baselines.h` for what the heuristic actually does.

- **`tardy_llm_decompose` does not call an LLM by itself.** It's lexical pattern matching on cue terms (Bonferroni, ISA mismatch, blood type compatibility, etc.). The function was renamed to `tardy_lexical_decompose` in v2.0. The old name is kept as a backwards-compatible inline shim. A separate, opt-in path in `src/mcp/server.c` *does* call Anthropic when `TARDY_LLM_DECOMPOSE=1` is set — that path is unrelated to this file.

- **Coq proofs cover the abstract BFT algorithm, not the C implementation.** `proofs/consensus.v` is real, complete, and `Qed.`-closed (no `Admitted`). It proves abstract majority-vote safety. It does NOT prove the C code is a refinement of that abstraction; the implementation is a faithful translation by hand.

- **Verifying a default-installed daemon with "Paris is in France" returns `low_confidence`, not `VERIFIED`.** Grounding requires the optional `tests/wikidata_common.nt` ontology to be loaded. The README example you may have seen in older versions overstated this.

---

## Catches what (with examples that actually work)

### Contradicting claims across a document

```bash
./tardygrada verify-doc paper.md
# === Tardygrada Document Verification ===
# File: paper.md
# Sentences: 87
# Triples extracted: 121
# [CONFLICT] Lines 42 vs 89:
#   "We used no external APIs"
#   "API costs totalled $2,400"
#   -> claims no APIs but reports API costs
#   Confidence: 0.85
```

Three layers run together: triple consistency (same subject + predicate, different object), numeric checks (the math doesn't add up), and the lexical implicit-relation layer (cue patterns).

### Lazy / fake-work agents

`laziness_bench` produces synthetic agent traces designed to test the detector. Catches: did-nothing-but-produced-output, skimmed-instead-of-analyzed, fabricated-evidence-of-work, copied-another-agent's-answer, verified-itself-in-a-circle. F1 0.92 across 100 adversarial traces.

### Tampered intermediate data

Values created with `let x = 5 @verified` are protected at the OS page-table level (`mprotect`) plus a SHA-256 read-time check. `@hardened` adds replicas + a Byzantine vote. `@sovereign` adds ed25519 signatures and BFT consensus. Tampering requires breaking SHA-256 or forging an ed25519 signature.

---

## How verification works

```mermaid
graph LR
    subgraph Pipeline["Verification Pipeline (8 layers)"]
        direction LR
        C["Claim"] --> D["Decompose"]
        D --> G["Ground"]
        G --> CON["Consistency"]
        CON --> P["Probabilistic"]
        P --> PR["Protocol"]
        PR --> F["Certification"]
        F --> CR["Cross-Rep"]
        CR --> W["Work Verify"]
        W --> V{"VERIFIED /<br>CONFLICT /<br>UNVERIFIABLE"}
    end
```

Layers 1, 2, 3, 4, 5, and 8 are deterministic and run on every check. Layers 6 and 7 are formal-methods primitives that ship as building blocks; they are not auto-invoked.

```mermaid
graph LR
    subgraph Trust["Tamper protection levels"]
        direction LR
        MUT["Mutable"] --> DEF["Default<br>(OS-locked)"]
        DEF --> VER["Verified<br>(+ SHA-256)"]
        VER --> HARD["Hardened<br>(+ replicas)"]
        HARD --> SOV["Sovereign<br>(+ ed25519 + BFT)"]
    end
```

```mermaid
graph TB
    subgraph Tardygrada["Tardygrada"]
        CLI_CMD["CLI"] --> DAEMON_S["Daemon"]
        DAEMON_S --> VM["VM Core"]
        VM --> VERIFY_S["Verification"]
        VM --> ONTO["Knowledge Base"]
        VM --> CRYPTO_S["Cryptography"]
    end

    subgraph External["Optional integrations"]
        BITF["brain-in-the-fish<br>(multi-agent debate)"]
        OO["open-ontologies<br>(OWL reasoning)"]
    end

    VM -- "coordinate" --> BITF
    VM -- "grounded_in" --> OO
```

---

## The language (for power users)

```text
agent MedicalAdvisor @sovereign @semantics(truth.min_confidence: 0.99) {
    invariant(trust_min: @verified)
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified
    let data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)
}
```

Every value is an agent. `receive()` accepts claims from external systems (over MCP). `@sovereign` means cryptographically signed and replicated. `coordinate` dispatches multi-agent debate.

You don't need to write Tardygrada to use Tardygrada. The CLI, daemon, and MCP bridge cover every common case.

---

## Reproduce the benchmarks

```bash
cd evaluation && make

./laziness_bench       # 100 synthetic traces, F1 0.92
./scaling_bench        # 5 -> 5000 agents, near-linear
./contradoc_bench      # 891 real ContraDoc documents, F1 0.58
./agenthallu_bench     # 693 real AgentHallu trajectories, F1 0.58
./halueval_bench       # 500 HaluEval examples, F1 0.03 (intentional weakness)
./vitaminc_bench       # 500 VitaminC fact-verification cases
./hallucination_bench  # synthetic; see CHANGELOG.md for caveats on this one
./ablation_bench       # layer-by-layer contribution
```

External datasets (`contradoc.json`, `agenthallu_flat.json`, `halueval_500.json`, `vitaminc_500.json`) are checked into the repo. Everything is reproducible offline.

---

## Hardening (v2.0)

- Daemon socket: created with mode 0600 (owner-only). Previously default umask, often world-readable on shared boxes.
- Daemon recv timeout: 5 s. Closes slow-loris DoS against the single-threaded dispatcher.
- Build flags: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE`. Linux additionally `-pie -Wl,-z,relro,-z,now`.
- Optional Anthropic API key (set when `TARDY_LLM_DECOMPOSE=1`): no longer placed on the command line for `curl`. Written to a 0600 temp file passed via `-K`, unlinked after the call. Not visible in `ps auxe` or `/proc/PID/cmdline`.
- Claude API request body: user claim is JSON-escaped before embedding. Closes a quote/backslash injection.

---

## Research foundations

Built on:

- [AgentSpec](https://arxiv.org/abs/2503.18666) (ICSE 2026) — runtime enforcement
- [Bythos](https://arxiv.org/abs/2302.01527) (CCS 2024) — Coq BFT verification
- Minsky frames (1974), CRDTs (Shapiro 2011), Datalog (1986)

Evaluated against:

- [SelfCheckGPT](https://arxiv.org/abs/2303.08896) (EMNLP 2023) — *cited as inspiration; the in-repo lexical baseline is NOT a re-implementation, see above*
- [FActScore](https://aclanthology.org/2023.emnlp-main.741/) (EMNLP 2023)
- [ContraDoc](https://aclanthology.org/2024.naacl-long.362/) (NAACL 2024)
- [HaluEval](https://huggingface.co/datasets/pminervini/HaluEval)

Related:

- [Mundler et al.](https://arxiv.org/abs/2305.15852) (ICLR 2024)
- [Fang et al.](https://arxiv.org/abs/2409.11283) (AAAI 2025)

---

## Companion projects

- **[open-ontologies](https://github.com/fabio-rovai/open-ontologies)** — ~11k lines of Rust. OWL reasoner, optionally invoked by the verifier as the ontology grounding source.
- **brain-in-the-fish** — ~24k lines of Rust. Multi-agent coordination/debate substrate, optionally invoked for `coordinate { ... } consensus(...)` blocks.

Both run as separate processes; Tardygrada speaks to them over local IPC.

---

## License

MIT. See [LICENSE](LICENSE).

---

## Versioning

This is v2.0. See [CHANGELOG.md](CHANGELOG.md) for the v1 → v2 scope realignment — what was renamed, what was scoped down, and what was hardened.
