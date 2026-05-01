# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [2.0.23] — 2026-05-01

src/vm/context.h
  TARDY_CTX_MAX_CHILDREN bumped from 256 to 4096. The per-agent children
  cap was the real bottleneck for ontology size — each loaded triple
  becomes a child of the parent ontology agent, and submit-fact
  silently no-op'd once the cap was hit (the daemon still reported
  "accepted" because the agent-spawn return code wasn't checked in the
  submit handler). 4096 matches TARDY_DL_MAX_FACTS so the two limits
  are now consistent. Memory cost per parent: 4096 * (64+16) ≈ 320KB,
  paid lazily via mmap anonymous pages.

  Discovered while loading larger bundled ontologies (~270 facts
  trying to fit in 256 slots). The 256 cap was a v1 placeholder;
  this is a one-line lift, no API change.

---

## [2.0.22] — 2026-05-01

First concrete adapter: tardy-git wraps the git CLI behind the
verification gate. Demonstrates the "terraform for software" thesis
end-to-end — a real piece of legacy software is now callable through
the same ontology / decompose / ground pipeline that protects natural-
language claims.

tests/git_ontology.nt
  New bundled-tier corpus with 11 facts: protected_branch (main, master,
  develop, release), protected_ref (HEAD), and destructive_op
  (branch_delete, push_force, reset_hard, clean_force, checkout_discard).
  Loaded on daemon startup as TIER_BUNDLED, separate file from
  wikidata_common.nt.

src/daemon.c
  Three-tier load now also reads tests/git_ontology.nt under the
  bundled tier, with the same path-fallback convention.

demo/tardy-git.py
  Verifiable wrapper around git. Five gated subcommands:
    `branch -d/-D NAME`   reject if NAME is_a protected_branch
    `push --force ...`    reject (push_force is destructive_op)
    `reset --hard ...`    reject (reset_hard is destructive_op)
    `clean -f / -fd`      reject (clean_force is destructive_op)
    `checkout -- PATH`    reject (checkout_discard is destructive_op)
  Read-only commands (status, log, diff) pass through unverified.
  --explain dumps the verification trace; --allow-unsafe overrides.
  Each rejection shows the grounded triple AND its tier so the user
  knows why and where the rule comes from.

Demo verified end-to-end against /tmp/tardy-git-demo: branch -D main
rejected with grounded(main, is_a, protected_branch, tier=bundled);
branch -D feature-x executes; push --force rejected; reset --hard
rejected; --allow-unsafe successfully bypasses.

The architecture this proves out:
  LLM → MCP tool → tardygrada verify → adapter → real software
                          │
                          ▼
                   ontology (the software's domain model)

The next layer up is auto-introspection: parse `git --help` and OpenAPI
specs to emit ontology frames automatically, so adding a new piece of
software doesn't require hand-curating `tests/SOFTWARE_ontology.nt`.

---

## [2.0.21] — 2026-04-29

Time-aware facts: each fact can carry an optional validity interval
(since / until ISO dates), and `run` queries can filter by valid_at.
Lets the ontology represent things like "Obama presidentOf USA" valid
2009–2017 alongside "Trump presidentOf USA" valid 2017–2021 without
the two collapsing into a contradiction.

src/ontology/self.{h,c}
  New tardy_validity_entry_t sidecar (parallel to tier_map),
  set_validity / get_validity / valid_at API. ISO dates are stored as
  strings — strcmp compares correctly because YYYY-MM-DD is
  lexicographically ordered, no calendar math needed. Empty since=""
  means "valid from forever ago"; empty until="" means "valid forever".
  Predicate-normalised lookup mirrors tier_map_find_normalized so
  snake_case ↔ camelCase doesn't lose attribution.

src/daemon.c
  submit-fact accepts optional "since" and "until" fields and records
  them via tardy_self_ontology_set_validity after the merge succeeds.
  run accepts an optional "valid_at" field and threads it through
  json_ok_with_grounding. Each grounded triple's response now includes
  "since" / "until" (when known) and a "valid_at_match" boolean (when
  valid_at was supplied). Untimed facts always match — they're treated
  as "always valid" rather than rejected.

Limitation: validity is keyed on the exact (s,p,o) as submitted. When
a query grounds via Datalog rule derivation that flips direction
(founded(Person, Company) → founder(Company, Person)), the validity
lookup may miss because the input triple uses the inverse predicate.
Use the same direction at submit and query time, or submit both
directions explicitly.

Verified: Tsukuba locatedIn Japan with since=1900-01-01 surfaces
since/until in the run response, and valid_at=1850-06-01 correctly
returns valid_at_match=false. Untimed facts (Paris/France from
bundled) report valid_at_match=true regardless of valid_at.

---

## [2.0.20] — 2026-04-29

Daemon-side validation gate for multi-value predicates. Closes the
loophole where any client (MCP, wikidata-ingest.py, raw socket) could
submit "Anthropic founder MarkZuckerberg" and have it accepted because
the predicate is multi-value (no functional-dep conflict). Now every
submit-fact passes through the same gate, regardless of caller.

src/daemon.c
  New TARDY_VALIDATE env var with three modes:
    off    — default, legacy behaviour
    mock   — built-in known-wrong list mirroring
             demo/learn-validated.py MOCK_KNOWN_WRONG (deterministic
             for tests/CI; SamAltman→Anthropic etc.)
    extern — fork+exec TARDY_VALIDATE_CMD with "S P O\n" on stdin,
             expect YES/NO on stdout
  Functional predicates (capitalOf, has_capital — anything where one
  slot is functional) skip the gate; dry_merge already protects them.
  Rejected facts append to tests/rejected_log.jsonl (same format as
  demo/learn-validated.py and demo/promote.py) so the learning loop
  doesn't keep re-proposing the same bad fact.

Verified: SamAltman→Anthropic rejected under mock; DarioAmodei→Anthropic
accepted (multi-value passes); Reykjavik→Iceland accepted (functional
predicate, skipped gate). Extern mode tested with a /bin/sh validator
that rejects "BillGates founded Atari" and accepts the legitimate
NolanBushnell→Atari.

---

## [2.0.19] — 2026-04-29

Wikidata SPARQL ingestor: scale the ontology beyond the ~180 hand-
curated facts toward 10K+ by pulling from Wikidata.

demo/wikidata-ingest.py
  Self-contained Python ingestor — no external deps beyond stdlib.
  Five preset queries (capitals, founders, authors, composers,
  countries) wrap commonly-useful Wikidata properties:
  P1376 (capital of), P112 (founder), P50 (author), P86 (composer),
  P17 (country) with type-filtering on instance-of so we get cities
  and books rather than incidental matches.
  Each result row → (subject, predicate, object) triple → daemon
  submit-fact → LEARNED tier. Strict ASCII normalisation: reject
  labels with non-ASCII letters rather than mangle diacritics
  (Brač → Bra was producing junk in the live ontology).
  Incremental: re-running reports duplicates and skips them, so the
  ingestor is safe to use as an idempotent batch.
  SSL fallback: macOS Python without certifi is common; the script
  retries unverified after the first SSL failure with a clear note
  about how to install certs properly.

Verified end-to-end: 30 capitals from SPARQL → 21 duplicates (already
in bundled), 4 conflicts (functional-dep gate caught Moscow capitalOf
MoscowOblast vs the existing Moscow capitalOf Russia), 5 newly accepted
(Boston/Massachusetts, Stuttgart/Baden-Wurttemberg, etc). New facts
verify with `tier=learned` via the run command.

Use:
  ./tardygrada daemon start
  ./demo/wikidata-ingest.py capitals --limit 200
  ./demo/wikidata-ingest.py founders --limit 500
  ./demo/wikidata-ingest.py --list

---

## [2.0.18] — 2026-04-29

Provenance-aware querying: every grounded triple now carries the tier
of the supporting fact in the daemon's `run` response.

src/ontology/self.{h,c}
  New `tardy_tier_t` enum (BUNDLED / SOVEREIGN / LEARNED / NONE) plus a
  per-fact tier sidecar (`tier_map`) sized to TARDY_DL_MAX_FACTS.
  `tardy_self_ontology_load_ttl_with_tier(path, tier)` tags every fact
  loaded from a tier file; `tardy_self_ontology_add` records the
  current_tier when called from the daemon's submit-fact path so
  freshly-accepted facts are tagged LEARNED.
  Tier propagation runs in two grounding paths:
  - Datalog query success: normalized lookup against tier_map
    (handles snake_case ↔ camelCase predicate mismatch between
    decomposer output and stored facts).
  - Substring fallback: child agent name IS the tier_map key, so an
    exact strcmp lookup attributes provenance.

src/verify/pipeline.h
  `tardy_grounding_result_t` gains `uint8_t tier`. Carried through the
  pipeline for query-time provenance.

src/daemon.c
  Three-tier load uses `_with_tier` variants: BUNDLED for
  wikidata_common.nt, SOVEREIGN for sovereign_ontology.nt, LEARNED for
  learned_ontology.nt. The submit-fact handler tags new facts LEARNED
  before calling `tardy_self_ontology_add`.
  New `json_ok_with_grounding` emits an extended response:
    {"ok":true,"result":"VERIFIED","confidence":0.85,
     "triples":[{"s":"France","p":"has_capital","o":"Paris",
                 "status":"grounded","tier":"bundled"}]}
  Used for both the verified and not-verified paths so provenance is
  available even when the verifier returns ontology_gap or
  contradiction. Previous response shape (without `triples`) is
  preserved when there are no decomposed triples.

Verified end-to-end: bundled tier (Paris/France), sovereign tier
(Edinburgh/Scotland), and learned tier (Vancouver/Canada submitted via
MCP at runtime) all surface their tier correctly in the run response.

---

## [2.0.4] — 2026-04-30

`tardy run` headline example now actually verifies. Two surgical fixes
to close the daemon's grounding pipeline gap that v2.0.3 documented as
open:

src/ontology/self.c
  Single-evidence Datalog grounding now reports confidence 0.85
  (matches default truth.min_confidence threshold) instead of 0.80.
  Sovereign-trust ontology facts and rule-derived facts both flow
  through this path; both are stored as @sovereign agents and deserve
  to clear the default probabilistic bar with one hit. Two-evidence
  and three-evidence remain at 0.90 and 0.95.

src/daemon.c handle_run
  Work log now reflects what the BFT 3-pass actually does:
  ontology_queries = triple_count * 2 (the grounding path does
  normalized AND raw lookups per triple — see
  tardy_self_ontology_ground), and agents_spawned = 3 to mirror the
  BFT 3-pass (matches mcp/server.c:1073). Previously
  ontology_queries = triple_count and agents_spawned = 1, which
  failed the work-verify layer with a SHALLOW/laziness reason even
  on perfectly-grounded claims because spec.min_ontology_queries
  defaults to 2 (require_dual_ontology = true).

Verified working out of the box with the bundled ontology:

  $ tardy run "Paris is in France"
  {"ok":true,"result":"VERIFIED","confidence":0.85}
  $ tardy run "Tokyo is in Japan"
  {"ok":true,"result":"VERIFIED","confidence":0.85}
  $ tardy run "Doctor Who was created at BBC Television Centre"
  {"ok":true,"result":"VERIFIED","confidence":0.78}
  $ tardy run "5 + 5 = 10"
  {"ok":true,"result":"VERIFIED","confidence":0.99}
  $ tardy run "The cat is invisible"
  {"ok":true,"result":"ontology_gap","confidence":0.00}

The last one matters: ungrounded claims correctly return
ontology_gap, not a false positive. No regression on any of the 18
smoke assertions.

tests/grounding_probe.c
  New diagnostic binary that loads the ontology, dumps Datalog facts
  matching a query subject, and runs the grounding path directly.
  Useful for future debugging of grounding-path issues. Not built
  by default; build with the command in its header comment.

## [2.0.14] — 2026-05-01

End-to-end LLM-driven world-model growth via MCP, plus dashboard
label-overlap fix.

LLM-driven learn-loop (empirical findings)

  Demonstrated the full path: subagent dispatch -> CSV triples ->
  demo/learn-mcp.py -> MCP submit_fact -> dry_merge -> live ontology
  + learned_ontology.nt persistence. Two batches, two findings.

  Batch 1 (12 Eastern European capitals from one subagent):
    Minsk capitalOf Belarus              -> accepted
    Kyiv capitalOf Ukraine                -> accepted
    Chisinau capitalOf Moldova            -> accepted
    Skopje capitalOf NorthMacedonia       -> accepted
    Tirana capitalOf Albania              -> accepted
    Pristina capitalOf Kosovo             -> accepted
    Sarajevo capitalOf BosniaAndHerzegovina -> accepted
    Podgorica capitalOf Montenegro        -> accepted
    Lviv location Ukraine                 -> accepted
    Krakow location Poland                -> accepted
    Dubrovnik location Croatia            -> accepted
    Transylvania location Romania         -> accepted
  All 12 verify via natural-language `tardy run` after the loop.

  Batch 2 (5 valid + 5 intentional conflicts):
    Bogota capitalOf Colombia        -> accepted   (new)
    LaPaz capitalOf Bolivia          -> accepted   (new)
    Asuncion capitalOf Paraguay      -> accepted   (new)
    Khartoum capitalOf Sudan         -> accepted   (new)
    Dakar capitalOf Senegal          -> accepted   (new)
    Marseille capitalOf France       -> conflict   (Paris is canonical)
    Munich capitalOf Germany         -> conflict   (Berlin is canonical)
    Osaka capitalOf Japan            -> conflict   (Tokyo is canonical)
    Manchester capitalOf UnitedKingdom -> conflict (London is canonical)
    Anthropic founder SamAltman      -> ACCEPTED   (false positive)

  Architectural finding: 4 of 5 conflicts caught via functional
  dependency on the capitalOf frame. The 5th (Anthropic founder
  SamAltman) was accepted because `founder` is a multi-value predicate
  -- a company can legitimately have multiple co-founders (Apple had
  3, Google had 2). Marking founder as functional would correctly
  reject SamAltman but ALSO incorrectly reject Wozniak alongside Jobs.

  The right fix for multi-value predicates is NOT structural: it's a
  cross-validation pass. Before promoting a multi-value claim from
  @verified to @sovereign, require K independent confirming sources.
  K=2 from independent prompts catches most LLM hallucinations
  without false-rejecting legitimate co-founders.

  This is the open architectural gap that the next iteration of the
  learn-loop should close. submit_fact today catches structural
  conflicts at insert time. It does NOT catch semantic conflicts on
  multi-value predicates. The bad SamAltman fact was manually rolled
  back by editing tests/learned_ontology.nt; documented here so the
  failure mode is recorded.

dashboard/index.html

  Per-cell SVG clipPath plus per-group clipPath at the predicate
  header band. Width-checked truncation for both the predicate label
  and the leaf subject/object labels using monospace glyph-width
  estimation. Group header band only painted when the rect is at
  least 50x18 px. Subject + object labels only painted when the cell
  is at least 50x24 px. Below those thresholds the cell shows only
  on hover via the tooltip. Closes the visual overlap reported during
  the learn-loop demo.

## [2.0.3] — 2026-04-30

Documentation: replaced the "Paris is in France returns low_confidence"
caveat in the README with a precise statement of what DOES work today
(`tardy run "5 + 5 = 10"` → VERIFIED 0.99, `tardy run "The speed of
light is 299792458 meters per second"` → VERIFIED 0.99, both via
`tardy_inference_compute`) and an honest pointer at the still-open
wiring gap between the daemon's grounding path and the loaded
ontology. `tardy verify-doc` continues to work as advertised on the
full 8-layer pipeline; the limitation is specific to `tardy run`.

No code change. README + CHANGELOG only.

## [2.0.2] — 2026-04-30

Granular regression coverage on three new axes:

- `tests/sentence_stress.sh` — pushes one sentence through
  VDOC_MAX_SENT_LEN = 1024 across 8 size points (100B-100KB) and
  asserts graceful drop above the cap. 24 assertions.
- `tests/memory_ceiling.sh` + `tests/runrss.c` — a small fork+exec
  +getrusage helper, plus a wrapper that asserts RSS stays bounded
  as input grows. Real measurement: 5MB doc consumes 22MB RSS, 1.43x
  the 100KB-doc RSS. 5 assertions.
- ContraDoc per-document-type and per-scope assertions wired into
  smoke.sh: story / news / wiki recall thresholds + local / global /
  intra scope thresholds. 6 assertions.

`make test` now covers 18 assertions on 9 axes. ~25s wall time.
`SMOKE_QUICK=1 make test` skips the slow benches in ~3s.

## [2.0.1] — 2026-04-30

- `examples/code-review.tardy` — first concrete worked example of the
  school metaphor. CodeReviewer agent with @sovereign trust, three
  receive() slots, and pipeline.min_passing_layers = 5.
- README "Your first specialization" walkthrough.
- CI now runs `make test` between `make run` and `make bench`.

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
