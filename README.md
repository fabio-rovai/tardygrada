[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/tardygrada-logo.png" alt="Tardygrada" width="300">
</p>

# Tardygrada

**LangChain is 50,000 lines of Python. CrewAI is 153,000. LlamaIndex is 237,000. Tardygrada replaces them in 15-53 lines.**

```bash
tardy terraform /path/to/any-agent-framework
# CrewAI:     153,245 lines -> 53 instructions
# LlamaIndex: 237,414 lines -> 15 instructions
# LangGraph:  101,662 lines -> 39 instructions
# MetaGPT:     89,734 lines -> 11 instructions
```

Tardygrada is a programming language where every value is a living agent. Programs compile to MCP servers. Every output is cryptographically verified before it can become a Fact. 212KB binary. Zero dependencies. Pure C11.

## Why

Every agent framework does the same thing: call an LLM, pass the result to the next LLM, hope it's correct. None of them verify the output. None of them prove the agent did the work. None of them ground claims against reality.

Tardygrada does. An agent says "Doctor Who was created at BBC Television Centre" and the system decomposes it into triples, grounds it against an ontology, runs 3 independent verification passes with Byzantine majority vote, and only then freezes it as an immutable, cryptographically signed Fact.

If the system doesn't have the knowledge to verify a claim, it says "I don't know" instead of making something up. 0% false acceptance rate without an ontology. 100% correct acceptance with one.

## 30-Second Demo

```bash
# Build (takes < 1 second)
git clone https://github.com/fabio-rovai/tardygrada && cd tardygrada && make

# Verify a claim
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

    // Claims from external agents -- verified before becoming Facts
    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified

    // Shell commands as agent bodies -- output captured and verified
    let patient_data: str = exec("sqlite3 patients.db 'SELECT * FROM current'")

    // Multi-agent coordination with debate + scoring
    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)

    let name: str = "Tardygrada Medical" @sovereign
}
```

Every keyword does something real:
- `receive()` creates a pending slot that external agents fill via MCP
- `exec()` forks /bin/sh, captures stdout, stores as agent value
- `grounded_in()` grounds claims against an OWL ontology via SPARQL
- `@sovereign` means: mprotect + ed25519 + SHA-256 + 5 BFT replicas
- `invariant()` defines constitution rules checked on every operation
- `coordinate` dispatches to brain-in-the-fish debate engine (or falls back to inbox)
- `fork` terraforms another .tardy file into the current context

## How It Works

```
External agent submits claim via MCP
        |
    Decompose into triples (47 English patterns, 3 independent passes)
        |
    Ground against ontology
        |-- Self-hosted: triples as @sovereign agents (in-process, 200ns)
        |-- External: open-ontologies via unix socket (SPARQL + OWL reasoning)
        |-- Offline: honest UNKNOWN (no fake confidence)
        |
    8-layer verification pipeline
        |-- 1. Decomposition agreement
        |-- 2. Ontology grounding (catches hallucination)
        |-- 3. OWL consistency (catches contradictions)
        |-- 4. Probabilistic confidence scoring
        |-- 5. Protocol structure validation
        |-- 6. Triple connectivity certification
        |-- 7. Cross-layer contradiction detection
        |-- 8. VM work verification (catches laziness)
        |
    BFT consensus: 3 independent passes, 2/3 must agree
        |
    Verified -> frozen (mprotect + SHA-256 + ed25519), provenance locked
    Failed -> 11 structured failure types + feedback-driven retry
```

## What's Real (Not Marketing)

**Mathematically proven:**
- BFT consensus safety: proven in Coq (Rocq 9.1). If < N/2 replicas corrupted, original value wins.
- Immutability: mprotect is CPU/MMU enforced. The hardware faults on any write attempt.
- Signatures: real ed25519 via Monocypher. Not HMAC stubs.

**Structurally enforced:**
- Laziness detection: VM independently logs all operations and compares to minimum work spec.
- Hallucination prevention: claims grounded against ontology. Unknown = Unknown, not "verified."
- Constitution invariants: checked on every read, write, and spawn. Not optional.

**Honest limitations:**
- Laziness detection catches zero-work and shallow-work agents. A clever agent doing minimum viable work passes.
- Hallucination prevention only works for claims the ontology has data about. It can't verify what it doesn't know.
- The text decomposer (47 patterns) is rule-based, not ML. It handles common English but not complex sentences.

## Three Projects, One Stack

```
Tardygrada (C, 212KB)          -- the language, compiler, VM, MCP server
        |
brain-in-the-fish (Rust, 25K) -- debate, scoring, moderation engine
        |                         coordinate keyword connects here
open-ontologies (Rust, 10K)    -- OWL reasoning, SPARQL, knowledge graphs
                                  grounded_in() connects here
```

Tardygrada is the front door. brain-in-the-fish is the execution engine. open-ontologies is the knowledge layer. Each works independently. Together they form a verified agent stack.

## Tiered Immutability

| Level | Mechanism | What It Takes to Corrupt |
|-------|-----------|--------------------------|
| `x: int = 5` | Provenance-tracked | Any write (tracked in audit log) |
| `let x: int = 5` | mprotect (OS kernel) | Kernel exploit |
| `let x = 5 @verified` | + SHA-256 hash check | Kernel exploit + SHA-256 preimage |
| `let x = 5 @hardened` | + 3 BFT replicas | Corrupt majority + SHA-256 |
| `let x = 5 @sovereign` | + ed25519 + 5 replicas | All of the above + forge ed25519 |

## Benchmarks

```
Read @verified (SHA-256):       197ns    5,000,000 ops/sec
Read @sovereign (BFT+sig):   1,538ns      650,000 ops/sec
Verification pipeline:          692ns    1,400,000 ops/sec
Message send:                   190ns    5,300,000 ops/sec
Spawn @sovereign:            19,820ns       50,000 ops/sec
```

Self-hosted ontology (pizza, 581 triples): 40x faster than the Rust+Oxigraph equivalent.

## vs Other Frameworks

| Framework | Size | Deps | Verification | Provenance |
|-----------|------|------|-------------|-----------|
| LangChain | 50K+ lines | 30+ pkgs | None | None |
| CrewAI | 153K lines | 30+ pkgs | None | None |
| LlamaIndex | 237K lines | 40+ pkgs | None | None |
| AutoGen | - | pyautogen + OpenAI | None | None |
| MetaGPT | 90K lines | 90 deps | LLM self-review | None |
| browser-use | 89K lines | - | None | None |
| PraisonAI | 50MB, 4553 files | 100+ | Prompt guardrails | None |
| **Tardygrada** | **212KB** | **Zero** | **8-layer + BFT** | **ed25519 + SHA-256** |

## terraform

```bash
# Convert any agentic repo to .tardy
tardy terraform /path/to/crewai

# Output: .tardy file + compile check + stats
# Detects: CrewAI, AutoGen, LangChain, LangGraph, LlamaIndex
# Maps: agents -> receive(), tools -> exec(), tasks -> verified claims
```

## Building

```bash
make        # < 1 second, produces 212KB binary
make run    # run tests
make bench  # run benchmarks
```

C11 compiler (cc/gcc/clang). No external libraries. No malloc. Direct syscalls only.

## License

MIT. See [LICENSE](LICENSE).

## Research Foundations

Built on techniques from: [ARIA Safeguarded AI](https://www.aria.org.uk/programme/safeguarded-ai/) (formal verification for AI), [HalluGraph](https://arxiv.org/abs/2406.12072) (knowledge-graph hallucination detection), [AgentSpec](https://arxiv.org/abs/2401.13178) (runtime agent enforcement), and [Bythos](https://arxiv.org/abs/2302.01527) (Coq-proven BFT consensus).
