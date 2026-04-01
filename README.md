# Tardygrada

A formally verified agent programming language where every value is an agent. Programs compile to MCP servers. External agents submit claims via MCP, Tardygrada verifies them against ontologies, and serves verified responses with provenance.

107KB binary. Zero dependencies. Pure C11.

## Why

Current AI agent frameworks (MCP clients, LangChain, etc.) are unreliable. They hallucinate, ignore instructions, produce shallow work. Tardygrada makes agent behavior formally verifiable:

- Every value carries cryptographic proof of its integrity
- Every claim from an LLM is grounded against an ontology before acceptance
- Every agent operation is independently monitored by the VM (like a dashcam)
- Coq-proven BFT consensus for agent agreement

## Language

Every value is a living agent. There are no variables, only agents holding values.

```
agent Researcher {
    let origin: Fact = receive("Where was Doctor Who created?") grounded_in(bbc) @verified
    let capital: Fact = receive("Capital of France?") grounded_in(geography) @verified
    let name: str = "tardygrada" @sovereign
}
```

### Tiered Immutability

| Level | Mechanism | Corrupted by |
|-------|-----------|-------------|
| mutable (no `let`) | provenance-tracked | any write |
| `let` | mprotect | kernel exploit |
| `@verified` | mprotect + SHA-256 | kernel + SHA-256 break |
| `@hardened` | replicas + hash + BFT vote | majority kernel + SHA-256 |
| `@sovereign` | full BFT + ed25519 | all of the above + ed25519 break |

### 8-Layer Verification Pipeline

Runs on every LLM-produced Fact. Skipped for literals and arithmetic.

1. **Decompose** -- text to triples (multiple independent agents + constrained generation)
2. **Ontology grounding** -- triples vs knowledge graph
3. **Consistency check** -- OWL reasoner for contradictions
4. **Probabilistic scoring** -- quantitative confidence (MDP modelling)
5. **Protocol check** -- session types compliance (Yoshida MPST)
6. **Formal certification** -- proof-certificate asymmetry
7. **Cross-representation bridge** -- all layers agree
8. **VM work verification** -- laziness detection

Fail fast: one layer fails, pipeline stops and reports which one. Overall confidence = minimum across all layers.

### Other Features

- **Error-as-agent**: errors are agents you converse with, not exceptions you catch
- **Constitution checking**: invariants checked on every operation
- **Self-healing**: VM detects and recovers from agent corruption
- **Terraform/fork module system**: no imports, agents fork or terraform from other files
- **VM nesting**: child VMs run as agents inside parent VMs
- **Semantic search**: `agent.query("...")` for O(log n) vector lookups across agent context

## MCP Server

Tardygrada compiles `.tardy` files and serves them as MCP servers (JSON-RPC 2.0 over stdio). Available tools:

| Tool | Description |
|------|-------------|
| `submit_claim` | Submit a claim for a pending `receive()` agent |
| `verify_claim` | Run the verification pipeline; freeze if it passes |
| `send_message` | Send a message between agents |
| `read_inbox` | Read an agent's message inbox |
| `query_agents` | Semantic search across all agents |
| `set_semantics` | Set per-agent verification thresholds |
| `<agent_name>` | Read any agent's value with full provenance |

## Usage

```sh
# Run tests
./tardygrada

# Compile and serve a .tardy file as an MCP server
./tardygrada examples/receive.tardy

# Build from source
make

# Check binary size
make size

# Clean
make clean
```

## Project Structure

```
src/
  main.c              -- entry point, test runner, MCP dispatch
  compiler/
    lexer.c/h         -- tokenizer for .tardy files
    compiler.c/h      -- parser and bytecode compiler
    exec.c/h          -- bytecode executor
    terraform.c/h     -- terraform/fork module system
  vm/
    vm.c/h            -- core VM: agent spawn, lifecycle, GC
    memory.c/h        -- arena allocator, mprotect enforcement
    context.c/h       -- context pointers, semantic addressing
    crypto.c/h        -- SHA-256, ed25519 stubs
    types.h           -- agent types and trust levels
    semantics.h       -- truth model (Axiomatic > Proven > ... > Refuted)
    semantic.c/h      -- semantic search across agents
    message.c/h       -- agent-to-agent message passing
    constitution.c/h  -- constitution invariant checking
    heal.c/h          -- self-healing and corruption recovery
    persist.c/h       -- agent state persistence
  mcp/
    server.c/h        -- MCP server (JSON-RPC 2.0 stdio transport)
    json.c/h          -- zero-allocation JSON parser
  verify/
    pipeline.c/h      -- 8-layer verification pipeline
    decompose.c/h     -- text-to-triple decomposition
  ontology/
    bridge.c/h        -- ontology grounding (sketch + complete)
  llm/                -- (reserved, LLM backend removed)
  agent/              -- (reserved)
examples/             -- .tardy example programs
proofs/
  consensus.v         -- Coq proof of BFT consensus correctness
docs/plans/           -- design documents and implementation plans
```

## Build Requirements

- C11 compiler (cc/gcc/clang)
- No external libraries. No malloc. Direct syscalls only.

## Design Documents

- [Language design](docs/plans/2026-03-31-tardygrada-design.md) -- core semantics, memory model, truth model
- [Complete implementation plan](docs/plans/2026-04-01-complete-language.md) -- task-by-task build plan

## License

See LICENSE file.
