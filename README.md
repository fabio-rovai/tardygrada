[![CI](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml/badge.svg)](https://github.com/fabio-rovai/tardygrada/actions/workflows/ci.yml)

<p align="center">
  <img src="docs/tardygrada-logo.png" alt="Tardygrada" width="300">
</p>

# Tardygrada

**A programming language where every agent output is cryptographically verified.**

Every value is an agent. Programs compile to MCP servers. Immutability is OS-enforced (mprotect). BFT consensus is Coq-proven. Ontology grounding catches hallucinations. The VM monitors every operation independently.

```
194KB binary | Zero dependencies | Pure C11 | Real ed25519 | Coq-proven BFT | 8-layer verification
```

## Quick Start

```bash
# Verify a claim
tardy run "Doctor Who was created at BBC Television Centre in 1963"

# Serve a .tardy file as MCP server
tardy serve examples/medical.tardy

# Compile check
tardy check examples/receive.tardy

# Build from source
make
```

## The Language

Every value is a living agent. There are no variables, only agents holding values.

```
agent MedicalAdvisor @sovereign @semantics(
    truth.min_confidence: 0.99,
    truth.min_consensus_agents: 5,
) {
    invariant(trust_min: @verified)

    let diagnosis: Fact = receive("symptom analysis") grounded_in(medical) @verified
    let name: str = "Tardygrada Medical" @sovereign

    coordinate {analyzer, validator} on("verify diagnosis") consensus(ProofWeight)
}
```

## How It Works

```
External agent submits claim via MCP
        |
    Decompose into triples (47 English patterns)
        |
    Ground against ontology (SPARQL via unix socket)
        |
    8-layer verification pipeline (BFT 3-pass consensus)
        |
    Verified -> frozen @verified (mprotect + SHA-256 + ed25519)
    Failed -> structured failure type + feedback-driven retry
```

## Verification Pipeline

Every LLM-produced Fact passes through all 8 layers. Skipped for literals and arithmetic. Fail fast: one layer fails, pipeline stops and reports which one. Overall confidence = minimum across all layers.

| Layer | Name | What It Checks |
|-------|------|---------------|
| 1 | Decompose | Text to subject-predicate-object triples (multiple independent agents + constrained generation) |
| 2 | Ontology grounding | Triples matched against knowledge graph via SPARQL |
| 3 | Consistency check | OWL reasoner detects contradictions with existing facts |
| 4 | Probabilistic scoring | Quantitative confidence via MDP modelling |
| 5 | Protocol check | Session types compliance (Yoshida MPST) |
| 6 | Formal certification | Proof-certificate asymmetry -- easy to verify, hard to forge |
| 7 | Cross-representation bridge | All layers agree on the same conclusion |
| 8 | VM work verification | Laziness detection -- did the agent actually do the work? |

## Tiered Immutability

| Level | Mechanism | Corrupted By |
|-------|-----------|-------------|
| `mutable` (no `let`) | Provenance-tracked | Any write |
| `let` | `mprotect` | Kernel exploit |
| `@verified` | `mprotect` + SHA-256 | Kernel + SHA-256 break |
| `@hardened` | Replicas + hash + BFT vote | Majority kernel + SHA-256 |
| `@sovereign` | Full BFT + ed25519 | All of the above + ed25519 break |

## MCP Tools

Tardygrada compiles `.tardy` files and serves them as MCP servers (JSON-RPC 2.0 over stdio).

| Tool | Description |
|------|-------------|
| `submit_claim` | Submit a claim for a pending `receive()` agent |
| `verify_claim` | Run the verification pipeline; freeze if it passes |
| `send_message` | Send a message between agents |
| `read_inbox` | Read an agent's message inbox |
| `query_agents` | Semantic search across all agents |
| `set_semantics` | Set per-agent verification thresholds |
| `get_conversation` | Read agent conversation history |
| `<agent_name>` | Read any agent's value with full provenance (one tool per agent) |

## vs Other Frameworks

| Framework | Size | Files | Deps | Verification | Provenance | Ontology |
|-----------|------|-------|------|-------------|-----------|---------|
| oh-my-claudecode | 200MB+ | ~50 | Node.js + 19 agents | LLM self-review | None | None |
| AI-Scientist-v2 | 4.5MB | 68 | 27 Python pkgs | LLM-as-judge | None | Semantic Scholar |
| DeerFlow | 25MB | 732 | 30+ (LangGraph) | Prompt guardrails | None | None |
| hermes-agent | 105MB | 1260 | 20+ Python pkgs | Regex matching | None | None |
| Claude Code | Closed | ? | Node.js runtime | LLM confidence | None | None |
| slides-grab | 7MB | 175 | Playwright + tldraw | None | None | None |
| PraisonAI | 50MB | 4553 | 100+ LLM providers | Prompt guardrails | None | None |
| background-agents | 2MB | 728 | Cloudflare+Modal+TF | None | None | None |
| **Tardygrada** | **194KB** | **42** | **Zero** | **8-layer + BFT** | **ed25519 + SHA-256** | **SPARQL** |

Every framework above accepts agent output at face value.
Tardygrada verifies it against reality before it becomes a Fact.

## Benchmarks

```
Read @verified (SHA-256):     197ns   (5M ops/sec)
Read @sovereign (BFT+sig):  1,538ns  (650K ops/sec)
Verification pipeline:        692ns  (1.4M ops/sec)
Message send:                 190ns  (5.3M ops/sec)
```

## Compliance Results

From the test harness:

```
Without ontology: 0% false acceptance (honest UNKNOWN fallback)
With ontology:    100% correct acceptance, 0% false acceptance
```

## Project Structure

```
src/
  main.c                -- entry point, test runner, CLI dispatch
  compiler/
    lexer.c/h           -- tokenizer for .tardy files
    compiler.c/h        -- parser and bytecode compiler
    exec.c/h            -- bytecode executor
    terraform.c/h       -- terraform/fork module system
  vm/
    vm.c/h              -- core VM: agent spawn, lifecycle, GC
    memory.c/h          -- arena allocator, mprotect enforcement
    context.c/h         -- context pointers, semantic addressing
    crypto.c/h          -- SHA-256, ed25519 (real Monocypher)
    types.h             -- agent types and trust levels
    semantics.h         -- truth model (Axiomatic > Proven > ... > Refuted)
    semantic.c/h        -- semantic search across agents
    message.c/h         -- agent-to-agent message passing
    constitution.c/h    -- constitution invariant checking
    heal.c/h            -- self-healing and corruption recovery
    persist.c/h         -- agent state persistence
  mcp/
    server.c/h          -- MCP server (JSON-RPC 2.0 stdio transport)
    json.c/h            -- zero-allocation JSON parser
  verify/
    pipeline.c/h        -- 8-layer verification pipeline
    decompose.c/h       -- text-to-triple decomposition
  ontology/
    bridge.c/h          -- ontology grounding (SPARQL via unix socket)
examples/               -- .tardy example programs
proofs/
  consensus.v           -- Coq proof of BFT consensus correctness
tests/                  -- test harness and benchmarks
```

## Building

```bash
make
```

C11 compiler (cc/gcc/clang). No external libraries. No malloc. Direct syscalls only.

## License

MIT. See [LICENSE](LICENSE).

## Based on Research From

- [ARIA Safeguarded AI](https://www.aria.org.uk/programme/safeguarded-ai/) -- formal verification for AI systems
- [davidad](https://www.aria.org.uk/what-we-do/safeguarded-ai/) -- open agency architecture
- Yoshida session types (MPST) -- protocol compliance for concurrent agents
- [HalluGraph](https://arxiv.org/abs/2406.12072) -- knowledge-graph grounding for hallucination detection
- [AgentSpec](https://arxiv.org/abs/2401.13178) -- formal specification of agent behavior
- [Bythos](https://arxiv.org/abs/2302.01527) -- verified multiparty session types in Coq
