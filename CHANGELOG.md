# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [2.0.0] — 2026-04-29

This release closes the gap between what Tardygrada was *claimed* to do and what
it actually does. Several v1.x headlines did not survive close reading; they
have been corrected, not buried. The runtime is unchanged in spirit; the
positioning, the in-code naming, and a handful of real bugs are not.

### TL;DR

- **Repositioning:** "the school where AI agents go to specialize" replaces the older "catch lazy agents" tagline. The verifier is the exam, the `.tardy` programs are the curriculum, the daemon is the campus.
- **Truth-in-naming:** the in-repo lexical heuristics no longer claim to be SelfCheckGPT or "LLM-assisted". Old names are kept as backwards-compatible shims so external code does not break.
- **Real bugs fixed:** daemon socket permissions, slow-loris DoS, mmap leak, MCP brace-parsing bug, JSON injection in the optional Anthropic call, API key in `argv`. The `verify_document` MCP tool is now wired to the actual pipeline (it was a stub).
- **Honest scope:** the README now lists what does NOT work (HaluEval F1 0.03, terraform-as-skeleton-not-rewrite, Coq covers abstract algorithm only) instead of hand-waving.
- **Hardening:** stack protector, FORTIFY_SOURCE, PIE; on Linux additionally PIE-link, RELRO, immediate binding.

### Added

- `verify_document` MCP tool actually verifies documents now. The daemon redirects stdout to a tmpfile, runs the real `verify_doc`, captures the report, and returns structured JSON (`contradictions`, `path`, `report`). Previously it returned `"verify-doc dispatched"` and did no work.
- `lexical_baseline_evaluate` and `lexical_baseline_result_t` in `evaluation/baselines.h`. Honest names for the in-repo deterministic heuristic that compares pairwise claims for negation/antonym/numeric inconsistencies.
- `tardy_lexical_decompose` and `tardy_lexical_decomposition_t` in `src/verify/llm_decompose.h`. Honest names for the lexical pattern-matching layer that surfaces implicit relations (Bonferroni thresholds, ISA mismatches, blood-type compatibility, etc.).
- `json_escape_str` helper in `src/mcp/server.c` covering `"`, `\`, `\n`, `\r`, `\t`, plus `\u00XX` for control characters.
- Build hardening flags: `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE`. Linux additionally gets `-pie -Wl,-z,relro,-z,now`.
- Daemon socket recv/send timeouts (5 s) on every accepted client.
- Buffered (rather than byte-by-byte) request read in the daemon accept loop.
- This `CHANGELOG.md`.

### Changed

- **README.md** rewritten end-to-end. New tagline: *"The school where AI agents go to specialize."* New mental-model table (agents = students, `.tardy` = curriculum, pipeline = exam, daemon = campus, MCP bridge = front gate). New "What does NOT work" section calling out HaluEval, terraform scaffolding scope, lexical-baseline naming, Coq scope, and the default-daemon grounding caveat.
- Daemon Unix socket (`/tmp/tardygrada.sock`) is now created with mode `0600`. Previously inherited the default umask, which is `srw-rw-r--` (world-readable) on most systems — a privilege boundary violation on shared boxes.
- `daemon_cleanup()` now `munmap`'s `daemon_srv` on every shutdown path. Previously the success path freed it but error paths leaked `sizeof(tardy_mcp_server_t)`.
- `mcp_bridge.c` argument extraction uses the JSON parser's recorded token length instead of re-counting braces. A `}` inside a string value (e.g. `{"claim":"x}y"}`) no longer truncates the parsed object.
- Optional Anthropic API call (`TARDY_LLM_DECOMPOSE=1`):
  - User claim is JSON-escaped before being embedded in the prompt body. Closes a quote/backslash injection.
  - API key is no longer placed on the `curl` command line. It is written to a `mkstemp(0600)` temp file and passed to `curl` via `-K config_file`. The file is `unlink`'d the moment `waitpid` returns (and on every error path). The secret is no longer visible in `ps auxe` or `/proc/PID/cmdline`.
- Display label `"SelfCheck"` in benchmark output renamed to `"LexBase"` across `contradoc_bench.c`, `agenthallu_bench.c`, `halueval_bench.c`, `hallucination_bench.c`, `vitaminc_bench.c`. Reflects what the in-repo baseline actually is: a deterministic lexical heuristic, not the published SelfCheckGPT.

### Deprecated

These names still work via inline backwards-compatibility shims in their respective headers, but new code should not use them:

- `selfcheck_evaluate` → `lexical_baseline_evaluate`
- `selfcheck_result_t` → `lexical_baseline_result_t`
- `tardy_llm_decompose` → `tardy_lexical_decompose`
- `tardy_llm_decomposition_t` → `tardy_lexical_decomposition_t`

### Fixed

- **HIGH:** Daemon Unix socket world-readable on shared systems. Now `0600`.
- **HIGH:** Slow-loris DoS — a client connecting and never sending `\n` would block the single-threaded daemon indefinitely. Now bounded by a 5 s `SO_RCVTIMEO` / `SO_SNDTIMEO`.
- **MED:** `tardy_mcp_server_t` mmap leaked on every error path through `daemon_cleanup()`.
- **MED:** `verify_document` MCP path was a no-op stub.
- **MED:** `mcp_bridge` argument-extraction brace-counting did not respect string boundaries.
- **MED:** Optional Anthropic API call: JSON injection on `"` or `\` in user claims; API key visible in process arg list.

### Scope clarifications (NOT silent fixes — explicit statements)

These are not changes to the code; they are corrections to the public claims around it.

- **`tardy terraform` is a structural skeleton extractor, not a framework rewrite.** It scans an existing CrewAI / LangGraph / LlamaIndex / AutoGen / etc. repo and emits an agent-shaped `.tardy` file with stubbed tool bodies (one of ~13 canned shell commands selected by name pattern, or a `receive()` placeholder). Each tool body must be wired to a real implementation. The verification scaffold around the stubs is real; the underlying tool execution is not. v1 README implied a runtime replacement ("This file replaces the entire framework with verified agents"). v2 README does not.
- **`evaluation/hallucination_bench.c` synthetic compositional test set.** This benchmark uses a hand-coded Boolean answer-key array (`group_b_has_contradiction[]` in `evaluation/hallucination_data.h`) which the test harness sets directly via `set_inconsistent()` before invoking the pipeline. The pipeline's consistency layer reads back what the harness set. The headline number this produced is therefore not a measurement of detection capability and has been removed from the README. The independently-graded ContraDoc / AgentHallu numbers (F1 0.58 on real data) are unchanged and remain the load-bearing benchmarks.
- **Coq proofs** (`proofs/consensus.v`) are real, complete, and `Qed.`-closed (no `Admitted`). They prove an *abstract* BFT majority-vote algorithm: lemmas `honest_corrupt_total`, `honest_votes_for_original`, theorem `bft_safety`, corollaries `bft_3_replicas`, `bft_5_replicas`, theorem `majority_unique`. They do NOT prove that the C code in `src/vm/memory.c` etc. is a refinement of that abstraction. The implementation is a faithful translation by hand. README v1 said "BFT consensus mathematically proven"; README v2 says "Coq proves the abstract BFT majority-vote algorithm; the C implementation is a faithful translation, not formally refined."
- **`HaluEval F1 0.03`.** This is a real number on real data, and we lose. The benchmark tests single-sentence factual errors against world knowledge — that is a retrieval problem. Tardygrada catches contradictions *between* claims and *between* claims and an attached ontology, not isolated false statements. v1 README de-emphasized this; v2 README makes it the first item on the "what does not work" list.
- **The default daemon does not ground `"Paris is in France"` to `VERIFIED`.** Grounding requires the optional `tests/wikidata_common.nt` ontology to be loaded. The v1 README's first code example showed `tardy run "Paris is in France" # VERIFIED (80%)`. That output is not what the default install produces. The example has been removed from v2.

### Still open (not fixed in v2.0)

- The synthetic `hallucination_bench` answer-key arrays. Replacing them with held-out, blind-evaluated test cases is methodology rework, not a quick code fix.
- HaluEval F1 0.03 — fundamental fit-of-task, not a bug.
- No model-to-code refinement for Coq proofs. Adding one is a research project on its own.
- A `static` 64 KB stack-allocated `tardy_triple_t all_triples[]` in `daemon.c::handle_run` (~48 KB). Within default 8 MB stack, but means the daemon cannot run with reduced stack limits.
- No C unit tests for `daemon.c`, `mcp_bridge.c`, `daemon_client.c`. CI exercises build + benchmarks only.

### Security

- `/tmp/tardygrada.sock` permissions: `0600` (was world-readable via default umask).
- `/tmp/tardy.curl.XXXXXX` (Anthropic API key, when `TARDY_LLM_DECOMPOSE=1` is set): created by `mkstemp` with explicit `fchmod 0600`, written and closed, passed to `curl -K`, `unlink`'d immediately after `waitpid`. Best-effort `memset` to zero of the on-stack copy of the header line.
- Build hardening: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-fPIE` everywhere. Linux additionally `-pie -Wl,-z,relro,-z,now`.

---

## [1.x] — earlier

See git history for the v1 line. Notable commits relevant to v2:

- `80d2aea` — *fix: 7 medium severity bugs — memory leak, duplicate conflicts, buffer overflows, palace race, line tracking*
- `8c90a84` — *fix: critical entity grouping, truncate overflow, -- separator, int overflow*
- `3ef4723` — *fix: daemon race condition between SessionStart and first UserPromptSubmit*
- `c096900` — *fix: new session = clean palace — wipe all state on SessionStart*
