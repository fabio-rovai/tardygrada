# Laziness, Hallucination, and Trust: Formal Verification Primitives for Autonomous Agent Runtimes

## Authors

Stylianos Kampakis, Fabio Rovai — The Tesseract Academy / Kampakis and Co Ltd

## Abstract (~250 words)

Autonomous LLM agents increasingly make decisions, produce reports, and verify each other's work — yet no existing framework can answer three fundamental questions: (1) did the agent actually do its work? (2) are the agent's claims internally consistent? (3) can we trust the agent's output hasn't been tampered with?

We present three formal verification primitives for agent runtimes:

**Laziness Detection.** We formalize five types of agent laziness (NoWork, ShallowWork, FakeProof, CopiedWork, CircularVerification) and present a "dashcam" model where the VM independently observes all agent operations. On a benchmark of 60 agent traces with 10 boundary edge cases, the detector achieves F1 1.00 with zero false positives on honest agents.

**Compositional Hallucination Detection.** Existing detectors check claims individually, missing contradictions between claims that are each independently plausible. We combine OWL consistency checking, numeric verification, and LLM-assisted decomposition into a three-layer detector. On 500 test cases across 5 difficulty tiers, the pipeline catches 95% of compositional contradictions (119/125) where individual checking catches 0%. Detection degrades gracefully: 100% on easy contradictions, 88% on very subtle statistical/methodological ones.

**Tiered Trust Enforcement.** We implement five immutability levels enforced at the OS level via mprotect, SHA-256, and ed25519 signatures, with graduated verification cost (197ns for hash-checked reads, 1,538ns for BFT-verified reads).

All three primitives are implemented in a 280KB zero-dependency C runtime that scales linearly to 5,000 agents in 97ms.

---

## 1. Introduction (~1.5 pages)

The deployment of autonomous LLM agents is accelerating. Agents evaluate documents, make hiring decisions, assess medical symptoms, and verify each other's claims. Yet the infrastructure for ensuring these agents behave correctly remains remarkably primitive.

Three problems:

**Problem 1: Agent laziness.** When an agent claims it queried a knowledge base, consulted three sources, and cross-checked its findings — how do you verify it actually did any of that? Current agent frameworks (CrewAI, LangGraph, AutoGen) have no mechanism to distinguish genuine work from fabricated output. An agent that copies another agent's answer or skips verification entirely produces output indistinguishable from an honest agent's.

**Problem 2: Compositional hallucination.** SelfCheckGPT, FActScore, SAFE, and ChainPoll check claims individually. "The project was completed on time" — plausible. "The project was delayed by 3 months" — also plausible. Only checking them TOGETHER reveals the contradiction. No existing detector does this systematically.

**Problem 3: Trust enforcement.** In multi-agent systems, agents read and write shared state. A score of 8.5/10 stored in a Python dict has zero guarantees — any agent can modify it silently. There is no spectrum between "fully mutable" and "fully immutable," and no mechanism to verify a value hasn't changed since it was created.

We address all three with formal definitions and a working runtime.

### Contributions

1. A formal taxonomy of agent laziness (5 types) with a detection mechanism based on independent VM observation (the "dashcam" model). F1: 1.00 on 60 traces.

2. A three-layer compositional hallucination detector (OWL consistency + numeric verification + LLM-assisted decomposition) that catches 95% of contradictions on 500 cases where individual checking catches 0%.

3. Five tiered trust levels enforced at the OS level, with measured verification costs from 197ns to 1,538ns per read.

4. A complete implementation in 280KB of C with zero dependencies, scaling linearly to 5,000 agents.

---

## 2. Related Work (~1 page)

### Hallucination Detection

- **SelfCheckGPT** (Manakul et al., 2023): Sample multiple responses, check consistency. Limitation: circular — same biases across samples. Does not check inter-claim consistency.
- **FActScore** (Min et al., 2023): Decompose into atomic facts, verify each against Wikipedia. Limitation: atom-level only, no compositional checking.
- **SAFE** (Wei et al., 2024): Search-augmented factual evaluation. Limitation: requires external search, not document-internal evidence.
- **ChainPoll** (Friel & Sanchez, 2023): Multiple LLM calls with majority vote. Limitation: still per-claim, no structural reasoning.
- **HaluEval** (Li et al., 2023): Benchmark for hallucination evaluation. We use its taxonomy.

None of these check whether a SET of claims is internally consistent. Our OWL consistency layer fills this gap.

### Agent Verification

- **AgentSpec** (Mukherjee et al., ICSE 2026): Runtime specification enforcement for LLM agents. Focuses on behavioral contracts. We extend with laziness detection, which AgentSpec does not address.
- **Agent Behavioral Contracts** (2026): Formal specification of agent behavior. Complementary — defines WHAT agents should do, we verify they DID it.
- **ReAct** (Yao et al., 2023): Reasoning + acting framework. No verification of whether reasoning was actually performed.

### Formal Methods for AI Safety

- **Dalrymple et al. (2024)**: Propose proof-certificate asymmetry for AI safety. Our work addresses orthogonal problems: laziness detection, compositional hallucination, and runtime trust enforcement.
- **Bythos** (CCS 2024): Compositional BFT verification in Coq. We use this for our sovereign trust tier.

---

## 3. Laziness Detection (~2 pages)

### 3.1 Threat Model

An agent receives a task, claims to perform work, and produces output. The agent may:
- Produce output without doing any work (NoWork)
- Do minimal work below required thresholds (ShallowWork)
- Fabricate evidence of work by tampering with its log (FakeProof)
- Copy another agent's output (CopiedWork)
- Verify itself in a circular chain (CircularVerification)

### 3.2 The Dashcam Model

The VM independently logs every operation an agent performs:
- Ontology queries (knowledge base lookups)
- Context reads (data access)
- Agent spawns (delegation)
- Compute time (CPU ns)
- Memory allocation

This log is stored in a hash-protected structure. The agent cannot modify it.

Before a task begins, the VM deterministically computes a **WorkSpec** — minimum expected operations based on task complexity. After the task, it compares:

```
Detection = Compare(WorkLog_observed, WorkSpec_expected)
```

### 3.3 Detection Mechanisms

**NoWork / ShallowWork**: Total observed operations < minimum threshold.

**FakeProof**: The work log carries an `operations_hash` computed by the VM over all fields. If an agent attempts to inflate its log, the hash won't match the VM's independently computed hash. Detection: recompute hash from log fields, compare.

**CopiedWork**: The VM tracks `work_similarity` between agents. If agent B's output is >95% similar to agent A's (configurable threshold), and agent B's work log shows fewer unique operations, it's flagged. Detection: similarity metric exceeds `max_work_similarity`.

**CircularVerification**: The VM tracks verification chain depth. If agent A verifies B, B verifies C, C verifies A — the chain depth exceeds `max_verification_chain`. Detection: depth counter exceeds threshold.

### 3.4 Evaluation

**Dataset:** 60 traces — 25 honest (varying work levels), 25 lazy (5 of each type), 10 edge cases (boundary thresholds, priority ordering, zero-hash passthrough).

**Results:**

| Type | Precision | Recall |
|------|-----------|--------|
| NoWork | 1.00 | 1.00 |
| ShallowWork | 1.00 | 1.00 |
| FakeProof | 1.00 | 1.00 |
| CopiedWork | 1.00 | 1.00 |
| CircularVerification | 1.00 | 1.00 |
| **Overall** | **1.00** | **1.00** |

Edge cases: similarity at threshold (0.95) correctly passes. Chain depth at max (3) correctly passes. Valid hash with low work correctly classified as ShallowWork (not FakeProof). Zero hash correctly not flagged.

### 3.5 Limitations

- Requires VM-level observation (cannot retrofit to existing frameworks without runtime modification)
- CopiedWork detection depends on similarity metric — semantic paraphrasing may evade
- Does not detect "plausible but wrong" work (agent genuinely tries but makes errors)

---

## 4. Compositional Hallucination Detection (~2.5 pages)

### 4.1 The Problem

Claim A: "The project was completed on time."
Claim B: "The project was delayed by 3 months."

Individually grounded. Compositionally contradictory. Existing detectors check A alone (pass) and B alone (pass). The contradiction is invisible.

### 4.2 Three-Layer Architecture

**Layer 1: OWL Consistency Checking.**
Claims are decomposed into RDF triples. An OWL reasoner checks the triple set for contradictions: disjointWith violations, functional dependency conflicts (e.g., a city can have only one country), sameAs/differentFrom conflicts. Catches: direct logical contradictions, numeric opposites, temporal impossibilities.

**Layer 2: Numeric Verification.**
A lightweight numeric extractor identifies quantities, rates, percentages, and units across claims. Nine pattern checkers detect:
- Rate saturation (Little's law violations)
- Baseline accuracy (model worse than majority class)
- Bonferroni correction (p-value with multiple tests)
- Temperature/physical mismatches
- Security bit mismatches (claimed vs actual entropy)
- Impossible speed (review throughput vs human limits)
- Overfitting (parameters >> data points)
- Sample size violations
- Selection bias in reviews

**Layer 3: LLM-Assisted Decomposition.**
For claims where pattern matching fails, a decomposition layer (14 patterns) extracts implicit relationships and converts them to explicit triples the reasoner can check. Example: "The lock has 128-bit security. It uses a timestamp-seeded RNG." → infers (timestamp_rng, provides, ~32_bits), (32_bits, less_than, 128_bits) → contradiction.

In production, this layer calls an actual LLM. For reproducible benchmarking, we use a deterministic simulator covering the 14 most common implicit contradiction patterns.

### 4.3 Evaluation

**Dataset:** 500 test cases across 4 groups:
- Group A: 125 consistent, grounded claims (should pass)
- Group B: 125 compositional contradictions across 5 difficulty tiers
- Group C: 125 ungrounded claims (should fail)
- Group D: 125 partially grounded (should fail)

**Results:**

| Difficulty | Individual | Pipeline | Delta |
|------------|-----------|----------|-------|
| Easy | 0% | 100% | +100% |
| Medium | 0% | 100% | +100% |
| Hard | 0% | 96% | +96% |
| Subtle | 0% | 92% | +92% |
| Very subtle | 0% | 88% | +88% |
| **Overall** | **0%** | **95%** | **+95%** |

Zero false positives on Group A (125/125 consistent claims correctly pass).

### 4.4 Ablation

| Configuration | Accuracy |
|---------------|----------|
| Full pipeline (all 3 layers) | 95% on Group B |
| OWL consistency only | 71% (misses numeric/statistical) |
| OWL + numeric only | 87% (misses domain-specific) |
| OWL + LLM decomposition only | 87% (misses numeric) |
| Remove probabilistic scoring | 75% overall accuracy |

Each layer contributes independently. The numeric layer adds 16 percentage points over OWL alone. LLM decomposition adds 8 more.

### 4.5 Error Analysis

6 cases remain undetected (require true world knowledge):
1. Telescope aperture insufficient for Pluto features (optical physics)
2. GPL v3 source disclosure requirements (legal domain)
3. Butterfly metamorphosis type (complete vs incomplete — biology)
4. Copper sulfate as pesticide in organic farming (agricultural regulation)
5. Cabin pressure standards at altitude (aviation engineering)
6. Vaccine trial exclusion criteria methodology (clinical trial design)

These require external knowledge bases not present in the system. Adding domain ontologies (medical, legal, aviation) would address them — a clear extension path.

---

## 5. Tiered Trust Enforcement (~1 page)

### 5.1 Five Levels

| Level | Enforcement | Verification Cost | What Breaks It |
|-------|-------------|-------------------|----------------|
| Mutable | None | 0ns | Any write |
| Default (let) | mprotect(PROT_READ) | 0ns | Kernel exploit |
| Verified | mprotect + SHA-256 | 197ns | Kernel + hash preimage |
| Hardened | mprotect + N replicas + majority vote | ~500ns | Corrupt majority + hash |
| Sovereign | mprotect + ed25519 + 5 replicas + BFT | 1,538ns | All above + forge signature |

### 5.2 Implementation

OS-enforced via mmap/mprotect. Memory pages are locked read-only at the kernel level. SHA-256 hashes are computed at creation time and stored in separate read-only pages. Replicas are independent mmap allocations. Ed25519 signatures use the Monocypher library (zero external deps).

### 5.3 Scaling

| Agents | Total Pipeline Time |
|--------|-------------------|
| 5 | 0.6 ms |
| 50 | 3.3 ms |
| 500 | 21 ms |
| 5,000 | 97 ms |

Linear scaling. Score storage (verified writes) dominates at scale. GC is negligible (<0.02ms).

---

## 6. Implementation (~0.5 page)

280KB binary. C11. Zero external dependencies. Direct syscalls (mmap, mprotect, read, write, socket, fork). No malloc — all allocation via mmap with page alignment.

The runtime includes: VM (6,229 lines), verification pipeline (2,163 lines), ontology engine with Datalog inference (2,487 lines), numeric verifier, LLM decomposition simulator. Builds in <1 second.

Programs compile to MCP servers (Model Context Protocol). Every value is an agent. Agent lifecycle: Live → Static → Temp → Dead, with tombstones preserving provenance.

---

## 7. Discussion (~0.5 page)

**Limitations:**
- Laziness evaluation uses synthetic traces, not real-world agent deployments
- Hallucination benchmark uses constructed (not naturally occurring) contradictions
- The LLM decomposition layer uses a deterministic simulator, not actual LLM calls — results may differ with real LLMs
- Trust enforcement assumes OS kernel integrity
- The 6 undetected hallucination cases show where external knowledge is needed

**Why not a standard benchmark?** TruthfulQA and HaluEval test individual claim truthfulness, not compositional consistency. No existing benchmark tests whether a detector can catch contradictions between independently plausible claims. Our 500-case benchmark is a first step — we release it for the community.

**Future work:**
- Evaluate on naturally occurring agent traces from production deployments
- Expand hallucination benchmark with domain ontologies (medical, legal)
- Formal proof of laziness detection completeness in Coq
- Integration with existing agent frameworks (CrewAI, LangGraph) via MCP protocol

---

## 8. Conclusion (~0.25 page)

We presented three verification primitives for autonomous agent runtimes: laziness detection (F1: 1.00), compositional hallucination detection (95% on 500 cases where individual checking gets 0%), and tiered trust enforcement (197ns-1,538ns per verified read). All three are implemented in a 280KB zero-dependency runtime scaling to 5,000 agents in 97ms.

The core insight: verification should happen at the runtime level, not the application level. An agent cannot fake work if the VM independently observes every operation. Claims cannot hide contradictions if an OWL reasoner checks the full set. Values cannot be tampered with if the OS kernel enforces immutability.

Code: https://github.com/fabio-rovai/tardygrada

---

## Appendix A: Benchmark Reproduction

```bash
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada
make                        # 280KB binary, <1s
cd evaluation && make       # Build all benchmarks
./laziness_bench            # 60 traces, F1 1.00
./hallucination_bench       # 500 cases, 95% detection
./scaling_bench             # 5→5000 agents, linear
./ablation_bench            # Layer-by-layer analysis
```

## Appendix B: Comparison to Related Approaches

| | SelfCheckGPT | FActScore | SAFE | ChainPoll | **Ours** |
|---|---|---|---|---|---|
| Individual claims | Yes | Yes | Yes | Yes | Yes |
| Compositional checking | No | No | No | No | **Yes (95%)** |
| Laziness detection | No | No | No | No | **Yes (F1 1.00)** |
| Trust enforcement | No | No | No | No | **Yes (5 levels)** |
| External search required | No | Yes | Yes | No | No |
| Runtime overhead | LLM calls | LLM calls | LLM + search | LLM calls | **197ns-1,538ns** |
| Dependencies | Python + LLM | Python + LLM | Python + search | Python + LLM | **Zero** |
