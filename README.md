[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

<p align="center">
  <img src="tardygrada-logo.png" alt="Tardygrada" width="200">
</p>

<h3 align="center">Catch lazy agents, contradicting claims, and tampered data</h3>

---

## Your agent says it checked three sources. Did it?

Your document says "completed on time" on page 2 and "delayed 3 months" on page 7. Did anyone notice?

Your scoring pipeline passed through 5 agents. Can you prove the scores weren't changed along the way?

```bash
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make

tardy run "Paris is in France"                    # verified
tardy verify-doc report.md                        # 2 contradictions found
tardy daemon start && tardy run "check this"      # persistent, remembers everything
```

---

## What it does

### Catches lazy agents

Your agent claims it queried the knowledge base, consulted sources, and cross-checked. Tardygrada records every operation independently — like a dashcam. If the agent faked it, you'll know.

| Laziness type | What it means | Caught? |
|---|---|:-:|
| Did nothing, produced output anyway | NoWork | Yes |
| Skimmed instead of analyzing | ShallowWork | Yes |
| Fabricated evidence of work | FakeProof | Yes |
| Copied another agent's answer | CopiedWork | Yes |
| "Verified" itself in a circle | CircularVerification | Yes |

### Catches contradicting claims

"The project was completed on time." and "The project was delayed by 3 months." — both sound fine alone. Together, they're a contradiction. Existing tools check claims one by one and miss this.

Tardygrada checks them together. Three layers:
- Logical contradictions (direct opposites, impossible combinations)
- Numeric contradictions (the math doesn't add up)
- Domain contradictions (the science doesn't work)

```bash
tardy verify-doc paper.md
# [CONFLICT] Lines 42 vs 89:
#   "We used no external APIs"
#   "API costs totalled $2,400"
#   → claims no APIs but reports API costs
```

### Catches tampered data

A score of 8.5 stored in a Python dict — any agent can silently change it to 9.5. In Tardygrada, values are locked by the operating system. Tampering requires breaking SHA-256 or forging an ed25519 signature.

---

## Get started

**Just the CLI:**
```bash
make                                    # builds in < 1 second
tardy run "your claim here"             # verify anything
tardy verify-doc your-file.md           # scan for contradictions
```

**Persistent mode** (remembers between runs):
```bash
tardy daemon start                      # start background service
tardy run "claim"                       # uses persistent knowledge base
tardy daemon status                     # see what it knows
tardy daemon stop                       # clean shutdown
```

**Inside Claude Code:**
```json
{
  "mcpServers": {
    "tardygrada": {
      "command": "tardygrada",
      "args": ["mcp-bridge"]
    }
  }
}
```
Then just ask: *"verify this document for contradictions"*

**Convert your existing agents:**
```bash
tardy terraform /path/to/crewai         # 153K lines → 53 instructions
tardy terraform /path/to/llamaindex     # 237K lines → 15 instructions
```

---

## How well does it work?

### Laziness detection

| | Precision | Recall | F1 |
|---|:-:|:-:|:-:|
| Clear cases (60 traces) | 1.00 | 1.00 | 1.00 |
| + Adversarial (40 traces) | 1.00 | 0.85 | **0.92** |

100 traces total. Zero false positives. Smart copiers who change 10-15% of the text slip through (similarity below threshold) — a known limitation. No existing tool does any of this.

### Contradiction and hallucination detection

| Dataset | What it is | Tardygrada | Best alternative |
|---|---|:-:|:-:|
| Clear contradictions (125) | Designed compositional | **95%** | SelfCheck: 59% |
| + Borderline cases (100) | Soft/ambiguous contradictions | **68%** | SelfCheck: ~40% |
| **AgentHallu (693 trajectories)** | Real agent hallucinations, 7 frameworks | **F1: 0.57** | DeepSeek-V3.1: 0.52 |
| ContraDoc (891 docs) | Real documents, human-annotated | **10%** | SelfCheck: 9% |
| HaluEval (500 responses) | Individual factual errors | F1: 0.32 | SelfCheck: 0.32 |

Detection runs in two modes: fully deterministic (11ms/trajectory, benchmarks use this) or LLM-enhanced for broader coverage.

On AgentHallu — the largest agent hallucination benchmark (693 trajectories, 7 frameworks, 5 categories) — Tardygrada **beats DeepSeek-V3.1** (F1 0.57 vs 0.52) without any LLM calls. GPT-5 gets 0.70 but costs per-trajectory API calls.

The sweet spot: logical, numeric, and structural contradictions between agent steps. Borderline cases and perspective shifts remain hard.

<details>
<summary>AgentHallu per-category recall</summary>

| Category | Recall |
|---|:-:|
| Tool-Use | 57% |
| Reasoning | 53% |
| Human-Interaction | 51% |
| Retrieval | 44% |
| Planning | 42% |

</details>

<details>
<summary>Detailed breakdown (clear cases)</summary>

| Difficulty | Detection |
|---|:-:|
| Easy (direct opposites) | 100% |
| Medium (logical) | 100% |
| Hard (math/physics) | 96% |
| Subtle (domain knowledge) | 92% |
| Very subtle (statistical) | 88% |

</details>

### Scaling

| Agents | Time |
|-------:|-----:|
| 5 | 0.6 ms |
| 500 | 21 ms |
| 5,000 | 97 ms |

---

## Under the hood

<details>
<summary><b>How verification works</b></summary>

```mermaid
graph LR
    subgraph Pipeline["Verification Pipeline"]
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

    style Pipeline fill:transparent
```

Claims are decomposed into triples, grounded against a knowledge base, checked for consistency, scored probabilistically, and verified for work integrity. Eight layers, all deterministic.

</details>

<details>
<summary><b>How tamper protection works</b></summary>

```mermaid
graph LR
    subgraph Trust["Protection Levels"]
        direction LR
        MUT["Mutable"] --> DEF["Default<br>(OS-locked)"]
        DEF --> VER["Verified<br>(+ SHA-256)"]
        VER --> HARD["Hardened<br>(+ replicas)"]
        HARD --> SOV["Sovereign<br>(+ ed25519 + BFT)"]
    end

    style Trust fill:transparent
```

Values are protected at the operating system level. The OS kernel enforces read-only memory. SHA-256 hashes detect any change. Ed25519 signatures prove authorship. BFT consensus requires corrupting multiple independent replicas.

</details>

<details>
<summary><b>How the daemon works</b></summary>

```mermaid
graph TB
    subgraph visible["What you see"]
        USER["You"] --> CLI["tardy run / verify-doc"]
    end

    subgraph hidden["What happens"]
        CLI --> DAEMON["Persistent daemon"]
        DAEMON --> AGENTS["Living agents"]
        DAEMON --> KB["Growing knowledge base"]
        DAEMON --> VERIFY["Verification pipeline"]
    end

    style visible fill:transparent
    style hidden fill:transparent
```

The daemon keeps agents alive between commands. The knowledge base grows as verified claims accumulate. Sovereign agents persist to disk on shutdown and reload on restart.

</details>

<details>
<summary><b>Architecture</b></summary>

```mermaid
graph TB
    subgraph Tardygrada["Tardygrada"]
        CLI_CMD["CLI"] --> DAEMON_S["Daemon"]
        DAEMON_S --> VM["VM Core"]
        VM --> VERIFY_S["Verification"]
        VM --> ONTO["Knowledge Base"]
        VM --> CRYPTO_S["Cryptography"]
        VERIFY_S --> DECOMP_S["Decompose"]
        VERIFY_S --> NUMERIC_S["Numeric Check"]
        VERIFY_S --> DOMAIN_S["Domain Check"]
        VERIFY_S --> WORK_S["Work Verify"]
    end

    subgraph External["Optional integrations"]
        BITF["brain-in-the-fish<br>(multi-agent debate)"]
        OO["open-ontologies<br>(OWL reasoning)"]
    end

    VM -- "coordinate" --> BITF
    VM -- "grounded_in" --> OO

    style Tardygrada fill:transparent
    style External fill:transparent
```

</details>

<details>
<summary><b>The language (for power users)</b></summary>

```
agent MedicalAdvisor @sovereign @semantics(truth.min_confidence: 0.99) {
    invariant(trust_min: @verified)
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified
    let data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)
}
```

Every value is an agent. Programs compile to servers. `receive()` accepts claims from external systems. `@sovereign` means the value is cryptographically signed and replicated. `coordinate` dispatches to multi-agent debate.

You don't need to learn this to use Tardygrada. The CLI and daemon handle everything.

</details>

<details>
<summary><b>Reproduce all evaluations</b></summary>

```bash
cd evaluation && make
./laziness_bench           # 60 traces, F1 1.00
./hallucination_bench      # 500 cases, 95% compositional
./scaling_bench            # 5→5000 agents, linear
./ablation_bench           # layer-by-layer analysis
./contradoc_bench          # 891 real documents (external)
./halueval_bench           # 500 HaluEval examples (external)
```

</details>

---

## Research

Built on: [AgentSpec](https://arxiv.org/abs/2503.18666) (ICSE 2026), [Bythos](https://arxiv.org/abs/2302.01527) (Coq BFT), Minsky frames (1974), CRDTs (Shapiro 2011), Datalog (1986).

Evaluated against: [SelfCheckGPT](https://arxiv.org/abs/2303.08896) (EMNLP 2023), [FActScore](https://aclanthology.org/2023.emnlp-main.741/) (EMNLP 2023), [ContraDoc](https://aclanthology.org/2024.naacl-long.362/) (NAACL 2024), [HaluEval](https://huggingface.co/datasets/pminervini/HaluEval).

Related: [Mundler et al.](https://arxiv.org/abs/2305.15852) (ICLR 2024), [Fang et al.](https://arxiv.org/abs/2409.11283) (AAAI 2025), [He et al.](https://arxiv.org/abs/2601.13600) (2026).

## License

MIT
