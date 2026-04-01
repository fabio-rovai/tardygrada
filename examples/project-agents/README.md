# Tardygrada Project Agent Orchestration

A case study showing how to build a multi-agent project management system with verified outputs, inspired by [oh-my-claudecode](https://github.com/Yeachan-Heo/oh-my-claudecode).

## The Problem with Current Agent Orchestration

Systems like oh-my-claudecode (OMC), CrewAI, and AutoGen coordinate multiple AI agents on tasks. They work, but they **trust agents implicitly**:

- An executor agent says "I implemented the feature" — believed without proof
- A verifier agent says "tests pass" — no cryptographic evidence
- A worker claims to have reviewed code — no trace of what was actually checked
- Agent outputs can be tampered with between execution and verification

## How Tardygrada Solves This

Every step in the pipeline carries cryptographic verification:

```
┌─────────────────────────────────────────────────────┐
│ OMC Pattern              │ Tardygrada Pattern        │
├─────────────────────────────────────────────────────┤
│ Agent says "done"        │ Agent submits claim       │
│ Leader trusts it         │ Pipeline verifies (8 layers)│
│ File-based state         │ @sovereign immutable state│
│ Markdown personas        │ Typed agents with trust   │
│ Retry on failure         │ Feedback-driven retry     │
│ Watchdog heartbeat       │ Constitution invariants   │
│ Model routing            │ Per-agent @semantics()    │
│ tmux isolation           │ VM nesting + mprotect     │
└─────────────────────────────────────────────────────┘
```

## Architecture

```
project.tardy compiles to MCP server

External agents (Claude, GPT, human) connect via MCP:

1. CLAIM: Worker submits "auth module implemented"
   → submit_claim(agent="task_1", claim="implemented OAuth2 with PKCE...")
   → Claim stored as mutable agent, conversation logged

2. VERIFY: Leader triggers verification
   → verify_claim(agent="task_1")
   → 8-layer pipeline: decompose → ground → consistency → ...
   → BFT: 3 independent passes, majority vote
   → If verified: frozen @verified, immutable, hash-locked

3. COORDINATE: Leader dispatches to workers
   → coordinate {executor, tester, reviewer} on("task")
   → Each worker gets the task in their inbox
   → Workers respond via send_message

4. AUDIT: Anyone can inspect
   → get_conversation(agent="task_1") → full history
   → query_agents(query="auth") → find related agents
   → Every read returns provenance (trust, hash, timestamp)
```

## Running the Example

```bash
# Build Tardygrada
make

# Start the project agent
./tardygrada examples/project-agents/project.tardy

# From another terminal (or via MCP client):
# Submit work from executor agent
submit_claim agent=task_1 claim="Implemented OAuth2 with PKCE flow..."

# Verify the work
verify_claim agent=task_1

# Check what happened
get_conversation agent=task_1
```

## Key Differences from OMC

| Feature | OMC | Tardygrada |
|---------|-----|------------|
| Binary size | ~200MB (Node.js + deps) | 177KB (zero deps) |
| Agent trust | Implicit | Cryptographic (ed25519 + SHA-256 + BFT) |
| State integrity | File-based JSON | mprotect + hash + replicas |
| Verification | Same-process assertion | 8-layer pipeline, 3-pass BFT consensus |
| Failure diagnosis | "task failed" | 11 structured failure types |
| Retry | Blind retry with watchdog | Feedback-driven (adjusts based on failure type) |
| Audit trail | Log files | Immutable provenance chain per agent |
| Recovery | Session resume | Self-healing (reverify + reconstruct from replicas) |

## When to Use This Pattern

- **Regulated domains**: compliance, audit, medical, financial
- **High-stakes automation**: deployment pipelines, data processing
- **Multi-team coordination**: when you need cryptographic proof of who did what
- **Audit requirements**: every decision traceable to evidence

## When NOT to Use This

- Quick prototyping (use OMC)
- Casual chatbots (overkill)
- Single-agent tasks (no coordination needed)
