# Tardygrada vs Agent Frameworks

Every agent framework solves the same problem differently.
Here's what each one takes to verify a claim — and what Tardygrada does in 3 lines.

## The Task

Verify: "Doctor Who was created at BBC Television Centre in 1963"

---

## oh-my-claudecode (OMC)

**200MB+ (Node.js + 19 agent personas + tmux)**

```
# Install
npm install oh-my-claude-sisyphus

# Define agents (19 markdown files, ~2000 lines total)
# Configure tmux panes, task routing, watchdog, governance
# Write task YAML, set up file-based inbox/outbox

# Submit task
echo "autopilot: verify that Doctor Who was created at BBC Television Centre"
# → Leader decomposes → Workers execute → Verifier checks
# → But: verifier is another LLM. No independent proof.
# → No cryptographic evidence. No ontology grounding.
# → "Verified" means "another LLM agreed."
```

**Tardygrada equivalent:**
```
agent Verifier {
    let claim: Fact = receive("Where was Doctor Who created?") grounded_in(bbc) @verified
}
```
3 lines. Grounded against real ontology. Cryptographic proof. 177KB binary.

---

## SakanaAI/AI-Scientist-v2

**4.5MB Python, 27 dependencies, runs ML experiments**

```python
# Install
pip install anthropic openai matplotlib wandb torch ...

# Configure 68 files of pipeline code
# agent_manager.py → parallel_agent.py → interpreter.py
# Best-First Tree Search across experiment branches

# The LLM:
# 1. Generates hypothesis
# 2. Writes code
# 3. Runs experiments
# 4. Interprets results
# 5. Writes the paper
# 6. Reviews its own paper (LLM-as-judge)
#
# No independent verification. The LLM judges itself.
```

**Tardygrada equivalent:**
```
agent Scientist @semantics(truth.min_confidence: 0.99) {
    let hypothesis: Fact = receive("experimental claim") grounded_in(literature) @sovereign
    invariant(trust_min: @verified)
}
```
Claims grounded against literature ontology. BFT consensus (3 independent pipeline passes). VM monitors actual work done. Can't self-judge.

---

## ByteDance/DeerFlow

**25MB, 732 files, LangGraph + LangChain + 30 dependencies**

```python
# Install
pip install langgraph langchain langchain-openai tavily firecrawl ...

# Lead agent wraps 8 middleware layers:
#   summarization, clarification, loop detection,
#   memory, token usage, todo tracking, subagent limits
# Sub-agents run in ThreadPoolExecutor
# Guardrails are prompt-based (LLM checks LLM)

# Result: sub-agent says "done" → lead agent trusts it
# No cryptographic proof. No ontology grounding.
# "Guardrails" = asking the LLM "is this safe?"
```

**Tardygrada equivalent:**
```
agent Lead @sovereign {
    let task_result: Fact = receive("subtask output") grounded_in(spec) @verified
    invariant(trust_min: @verified)
    invariant(non_empty)
}
```
Sub-agent output goes through 8-layer verification. Constitution invariants enforced on every operation. Ontology grounding, not prompt-based guardrails.

---

## NousResearch/hermes-agent

**105MB, 1260 files, tool registry + skill system + gateway**

```python
# Install
pip install nous-hermes-agent[all]

# 30+ skill categories, Telegram/Discord/Slack gateway
# Tool handlers return strings → straight into conversation
# Delegation: child AIAgent with restricted toolset
# Safety: regex pattern matching for dangerous commands

# Result: tool says X → agent believes X
# No verification. No provenance. No grounding.
# Safety = regex. Not formal invariants.
```

**Tardygrada equivalent:**
```
agent Assistant {
    let answer: Fact = receive("tool output") grounded_in(knowledge) @verified
}
```
Tool output verified before becoming a Fact. Regex replaced by 8-layer pipeline. Agent can't bypass verification — it's the VM, not a prompt.

---

## Claude Code + Superpowers

**Closed-source runtime + markdown prompts as code**

```markdown
# commands/review.md — a prompt file IS the code
---
allowed-tools: [Agent, Read, Grep]
---
Review this code for bugs.
Spawn sub-agents: haiku for triage, sonnet for CLAUDE.md, opus for bugs.
Filter by confidence > 0.7.

# Result: confidence = LLM self-assessment
# No cryptographic signing. No ontology grounding.
# A typo in this markdown silently produces wrong behavior.
# Runtime is closed-source — can't audit the agent loop.
```

**Tardygrada equivalent:**
```
agent Reviewer @semantics(truth.min_confidence: 0.95) {
    let finding: Fact = receive("code review finding") grounded_in(codebase) @verified
    invariant(trust_min: @verified)
}
```
Findings are facts, not self-assessed confidence scores. Grounded against codebase ontology. Open source — 177KB of auditable C.

---

## Summary

| Framework | Size | Deps | Verification | Provenance | Ontology |
|-----------|------|------|-------------|-----------|---------|
| oh-my-claudecode | 200MB+ | Node.js + 19 agents | LLM self-review | None | None |
| AI-Scientist-v2 | 4.5MB | 27 Python pkgs | LLM-as-judge | None | Semantic Scholar (novelty only) |
| DeerFlow | 25MB | 30+ (LangGraph) | Prompt-based guardrails | None | None |
| hermes-agent | 105MB | 20+ Python pkgs | Regex pattern matching | None | None |
| Claude Code | Closed | Node.js runtime | LLM confidence scoring | None | None |
| **Tardygrada** | **177KB** | **Zero** | **8-layer pipeline + BFT** | **ed25519 + SHA-256** | **SPARQL grounding** |

---

## slides-grab

**7MB, JavaScript, Playwright + tldraw + Express**

```
# Visual slide editor: select region → ask LLM to edit → export PDF
# Pipeline: Plan → Design → Visual Edit → Export
# The LLM generates HTML. You trust it. Export to PDF.
# No verification that the content is accurate.
# No provenance on who edited what.
```

**Tardygrada equivalent:**
```
agent SlideAgent {
    let slide_content: Fact = receive("slide HTML content") grounded_in(spec) @verified
    let exported: str = receive("export result") @verified
}
```
Slide content verified against spec before export. Every edit tracked with provenance.

---

## PraisonAI

**50MB, 4553 files, Python, "100+ LLM providers"**

```yaml
# YAML-driven agent teams with handoffs, guardrails, memory, RAG
# Deploy to Telegram/Discord/WhatsApp
# Supports 100+ LLM providers via litellm

# agents.yaml — 50+ lines per agent definition
# The "guardrails" are prompt-based LLM checks
# Memory is unverified writes to a store
# 4500+ files for what is essentially: call LLM, pass result to next LLM
```

**Tardygrada equivalent:**
```
agent Team @semantics(truth.min_consensus_agents: 3) {
    let analysis: Fact = receive("agent output") grounded_in(domain) @verified
    coordinate {worker_1, worker_2, worker_3} on("task") consensus(ProofWeight)
}
```
4553 files → 3 lines. Consensus is cryptographic, not prompt-based. Memory is hash-verified.

---

## background-agents

**2MB, TypeScript monorepo, Cloudflare + Modal + Terraform + GitHub App**

```
# 8 packages: control-plane, modal-infra, web, slack-bot, github-bot, etc.
# Cloudflare Durable Objects for state
# Modal sandboxes for code execution
# WebSocket bridge between control/data plane
# Creates PRs from background tasks

# Setup: Terraform + Cloudflare + Modal + GitHub App + OAuth
# Single-tenant only. No multi-tenant isolation.
# No verification of code correctness. Agent creates PR, you review.
```

**Tardygrada equivalent:**
```
agent BackgroundWorker @sovereign {
    let code_change: Fact = receive("code diff") grounded_in(codebase) @verified
    invariant(trust_min: @verified)
    invariant(non_empty)
}
```
Code changes verified against codebase ontology before creating PR. No Terraform. No Cloudflare. 177KB binary.

---

## Summary

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
| **Tardygrada** | **177KB** | **30** | **Zero** | **8-layer + BFT** | **ed25519 + SHA-256** | **SPARQL** |

Every framework above accepts agent output at face value.
Tardygrada verifies it against reality before it becomes a Fact.

That's the difference between hope-based and proof-based agent systems.
