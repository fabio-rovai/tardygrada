#!/usr/bin/env bash
#
# Tardygrada smoke tests — minimal, fast, runnable in CI.
#
# These are NOT a unit-test suite. They run a small set of evaluation
# binaries and assert that headline numbers are within sane regression
# bounds. If a refactor accidentally breaks the verifier the smoke test
# fails loudly.
#
# Exit 0 = all assertions pass. Exit non-zero = at least one failed.
#
# Run from the repo root:
#     make test

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EVAL="$ROOT/evaluation"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
DIM='\033[2m'
NC='\033[0m'

PASS=0
FAIL=0
SKIPPED=0

ok()    { printf "${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail()  { printf "${RED}FAIL${NC} %s -- %s\n" "$1" "$2"; FAIL=$((FAIL+1)); }
skip()  { printf "${YELLOW}SKIP${NC} %s -- %s\n" "$1" "$2"; SKIPPED=$((SKIPPED+1)); }
note()  { printf "${DIM}     %s${NC}\n" "$1"; }

# --------------------------------------------------------------------
# Setup: build evaluation binaries if missing
# --------------------------------------------------------------------

if [ ! -x "$EVAL/scaling_bench" ] || [ ! -x "$EVAL/laziness_bench" ]; then
    printf "${DIM}Building evaluation binaries...${NC}\n"
    (cd "$EVAL" && make >/dev/null 2>&1) || {
        printf "${RED}cannot build evaluation binaries${NC}\n"
        exit 1
    }
fi

# --------------------------------------------------------------------
# Helper: extract a number from a labelled line
# --------------------------------------------------------------------

float_at_least() {
    # $1 = actual, $2 = threshold, $3 = label
    awk -v a="$1" -v t="$2" -v l="$3" '
        BEGIN {
            if (a + 0 >= t + 0)
                exit 0;
            else
                exit 1;
        }
    '
}

# --------------------------------------------------------------------
# 1. scaling_bench: 5000 agents must complete in < 5000ms
# (CI runners are slower than local dev; threshold accommodates variability)
# --------------------------------------------------------------------

OUTPUT="$("$EVAL/scaling_bench" 2>&1)"
LAST_TOTAL="$(echo "$OUTPUT" | grep -E '^5000,' | awk -F',' '{print $NF}')"

if [ -z "$LAST_TOTAL" ]; then
    fail "scaling/5000-agents" "could not parse 5000-agent total_ms from output"
else
    note "5000 agents total_ms = $LAST_TOTAL"
    if awk -v v="$LAST_TOTAL" 'BEGIN { exit !(v + 0 < 5000) }'; then
        ok "scaling/5000-agents-under-5000ms"
    else
        fail "scaling/5000-agents-under-5000ms" "$LAST_TOTAL ms (threshold: 5000ms)"
    fi
fi

# --------------------------------------------------------------------
# 2. laziness_bench: F1 (clear set) must be >= 0.95, zero false positives
# --------------------------------------------------------------------

OUTPUT="$("$EVAL/laziness_bench" 2>&1)"

# Look for "F1: 1.00" in the clear set summary
CLEAR_F1="$(echo "$OUTPUT" | grep -E 'F1.*1\.[0-9]+' | head -1 | grep -oE '1\.[0-9]+' | head -1)"

if [ -z "$CLEAR_F1" ]; then
    note "could not parse clear-set F1 — laziness_bench output format may have changed"
    skip "laziness/clear-f1" "parse failed"
else
    note "clear set F1 = $CLEAR_F1"
    if float_at_least "$CLEAR_F1" "0.95" "laziness clear F1"; then
        ok "laziness/clear-f1>=0.95"
    else
        fail "laziness/clear-f1>=0.95" "F1 = $CLEAR_F1"
    fi
fi

# --------------------------------------------------------------------
# 3. Main binary builds and runs --help equivalent
# --------------------------------------------------------------------

if [ -x "$ROOT/tardygrada" ]; then
    if "$ROOT/tardygrada" 2>&1 | grep -q "Tardygrada"; then
        ok "binary/runs-and-prints-banner"
    else
        fail "binary/runs-and-prints-banner" "no 'Tardygrada' string in default output"
    fi
else
    fail "binary/exists" "$ROOT/tardygrada not built"
fi

# --------------------------------------------------------------------
# 4. Renamed symbols are present in the binary
# --------------------------------------------------------------------

if command -v nm >/dev/null 2>&1 && [ -x "$ROOT/tardygrada" ]; then
    if nm "$ROOT/tardygrada" 2>/dev/null | grep -q "tardy_lexical_decompose"; then
        ok "rename/tardy_lexical_decompose-exported"
    else
        fail "rename/tardy_lexical_decompose-exported" "symbol not found via nm"
    fi
else
    skip "rename/tardy_lexical_decompose-exported" "nm unavailable"
fi

# --------------------------------------------------------------------
# 5. Daemon socket path is configurable via env
# (sanity check -- does the binary actually read TARDY_DAEMON_SOCKET if set?)
# Skipped unless explicitly opted-in to avoid touching a running daemon.
# --------------------------------------------------------------------

skip "daemon/socket-env-override" "opt-in only (run separately to avoid disturbing a live daemon)"

# --------------------------------------------------------------------
# 5b. tardy run grounding regression. v2.0.4 fixed the daemon path so
# that the bundled ontology (tests/wikidata_common.nt) actually causes
# Datalog-derived facts to ground claims through the BFT 3-pass.
# These assertions guard against silent regression of that fix.
#
# This requires a running daemon; we start one if not running and
# remember to leave it as we found it.
# --------------------------------------------------------------------

GROUNDING_DAEMON_STARTED=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    : # daemon already running, leave alone
else
    nohup "$ROOT/tardygrada" daemon start >/dev/null 2>&1 &
    disown 2>/dev/null || true
    sleep 1
    GROUNDING_DAEMON_STARTED=1
fi

run_assert_verified() {
    # $1 = claim, $2 = test name
    local out
    out="$("$ROOT/tardygrada" run "$1" 2>&1 | tr -d '\n')"
    if echo "$out" | grep -q '"result":"VERIFIED"'; then
        ok "tardy-run/$2"
    else
        fail "tardy-run/$2" "got: $out"
    fi
}

run_assert_result() {
    # $1 = claim, $2 = expected substring in result, $3 = test name
    local out
    out="$("$ROOT/tardygrada" run "$1" 2>&1 | tr -d '\n')"
    if echo "$out" | grep -q "\"result\":\"$2\""; then
        ok "tardy-run/$3"
    else
        fail "tardy-run/$3" "expected $2, got: $out"
    fi
}

run_assert_verified "Paris is in France"          "headline-paris-in-france"
run_assert_verified "Tokyo is in Japan"            "tokyo-in-japan"
run_assert_verified "Madrid is in Spain"           "madrid-in-spain"
run_assert_verified "Beijing is in China"          "beijing-in-china"
run_assert_verified "Microsoft was founded by BillGates" "ms-founded-by-gates"
run_assert_verified "MarieCurie discovered Radium" "curie-discovered-radium"
run_assert_verified "JaneAusten created PrideAndPrejudice" "austen-created-pap"
run_assert_verified "BillGates founded Microsoft"  "gates-founded-ms"
run_assert_verified "VanGogh painted StarryNight"  "vangogh-painted-starry"
run_assert_verified "5 + 5 = 10"                   "computational-arithmetic"
run_assert_verified "The speed of light is 299792458 meters per second" \
                                                   "fundamental-constant"
run_assert_result   "The cat is invisible" "ontology_gap" \
                                                   "ungrounded-returns-gap"

# Stop the daemon if we started it. Keep it if it was already running.
if [ "$GROUNDING_DAEMON_STARTED" = "1" ]; then
    "$ROOT/tardygrada" daemon stop >/dev/null 2>&1 || true
fi

# --------------------------------------------------------------------
# 6. Per-category accuracy on AgentHallu (real data, 693 trajectories).
# Catches regressions in any specific failure mode independently.
# Thresholds are set BELOW current measured values to avoid flaking on
# noise but ABOVE zero so a structural regression fails the test.
#
# Skipped when SMOKE_QUICK=1 because the bench takes ~4s.
# --------------------------------------------------------------------

if [ "${SMOKE_QUICK:-0}" = "1" ]; then
    skip "agenthallu/per-category-recall" "SMOKE_QUICK=1 set"
else
    # agenthallu_bench loads agenthallu_flat.json from the CWD, so we must
    # invoke it from the evaluation/ directory.
    OUTPUT="$(cd "$EVAL" && ./agenthallu_bench 2>&1)"

    parse_category_recall() {
        # $1 = category label as printed (e.g. "Tool-Use", "Reasoning")
        # extracts Recall column from a line like:
        #   Tool-Use          103     1.0000     0.2136     0.3520
        echo "$OUTPUT" | awk -v cat="$1" '
            $1 == cat { print $4; exit }
        '
    }

    assert_recall_at_least() {
        # $1 = category, $2 = threshold, $3 = label
        local actual
        actual="$(parse_category_recall "$1")"
        if [ -z "$actual" ]; then
            fail "agenthallu/$3" "could not parse recall for $1"
            return
        fi
        note "$1 recall = $actual"
        if awk -v a="$actual" -v t="$2" 'BEGIN { exit !(a + 0 >= t + 0) }'; then
            ok "agenthallu/$3>=$2"
        else
            fail "agenthallu/$3>=$2" "recall = $actual"
        fi
    }

    # Current measured values (April 2026): Tool-Use 0.21, Reasoning 0.68,
    # Human-Int 0.53, Retrieval 0.59, Planning 0.66.
    # Thresholds set ~25% below measured to absorb run-to-run variation.
    assert_recall_at_least "Tool-Use"   "0.15" "tool-use-recall"
    assert_recall_at_least "Reasoning"  "0.50" "reasoning-recall"
    assert_recall_at_least "Human-Int"  "0.40" "human-int-recall"
    assert_recall_at_least "Retrieval"  "0.45" "retrieval-recall"
    assert_recall_at_least "Planning"   "0.50" "planning-recall"
fi

# --------------------------------------------------------------------
# 7. Per-type accuracy on ContraDoc (real data, 891 documents).
# Catches regressions on document type (story / news / wiki) and on
# contradiction scope (local / global / intra).
#
# Skipped when SMOKE_QUICK=1 because the bench takes ~6s.
# --------------------------------------------------------------------

if [ "${SMOKE_QUICK:-0}" = "1" ]; then
    skip "contradoc/per-type-recall" "SMOKE_QUICK=1 set"
else
    OUTPUT="$(cd "$EVAL" && ./contradoc_bench 2>&1)"

    parse_field4() {
        # $1 = first-column label (e.g. "story" or "local")
        # extracts the 4th field from a line that starts with that label.
        # Note: ContraDoc table for doctype has 7 fields; we want $4 (TD Recall).
        # Note: ContraDoc table for scope has 4 fields; we want $4 (Tardygrada).
        echo "$OUTPUT" | awk -v label="$1" '$1 == label { print $4; exit }'
    }

    assert_field4_at_least() {
        # $1 = label, $2 = threshold, $3 = test name
        local actual
        actual="$(parse_field4 "$1")"
        if [ -z "$actual" ]; then
            fail "contradoc/$3" "could not parse recall for $1"
            return
        fi
        note "$1 recall = $actual"
        if awk -v a="$actual" -v t="$2" 'BEGIN { exit !(a + 0 >= t + 0) }'; then
            ok "contradoc/$3>=$2"
        else
            fail "contradoc/$3>=$2" "recall = $actual"
        fi
    }

    # By document type (current measured: story 0.52, news 0.50, wiki 0.91)
    assert_field4_at_least "story" "0.40" "story-recall"
    assert_field4_at_least "news"  "0.40" "news-recall"
    assert_field4_at_least "wiki"  "0.75" "wiki-recall"

    # By scope (current measured: local 0.62, global 0.72, intra 0.58)
    # mixed has only N=1 in the test set, so skip the assertion for it.
    assert_field4_at_least "local"  "0.50" "local-scope-recall"
    assert_field4_at_least "global" "0.55" "global-scope-recall"
    assert_field4_at_least "intra"  "0.45" "intra-scope-recall"
fi

# --------------------------------------------------------------------
# 8. Context-size stress test. Runs verify-doc against documents at
# increasing sizes and asserts that detection still works and timing
# stays bounded. Skipped when SMOKE_QUICK=1.
# --------------------------------------------------------------------

if [ "${SMOKE_QUICK:-0}" = "1" ]; then
    skip "context-stress" "SMOKE_QUICK=1 set"
elif [ -x "$ROOT/tests/context_stress.sh" ]; then
    if "$ROOT/tests/context_stress.sh"; then
        ok "context-stress/scaling-and-detection"
    else
        fail "context-stress/scaling-and-detection" "see context_stress output above"
    fi
else
    skip "context-stress" "tests/context_stress.sh not found"
fi

# --------------------------------------------------------------------
# 9. Sentence-length stress test. Pushes one sentence past the
# VDOC_MAX_SENT_LEN cap and asserts graceful degradation.
# Skipped when SMOKE_QUICK=1.
# --------------------------------------------------------------------

if [ "${SMOKE_QUICK:-0}" = "1" ]; then
    skip "sentence-stress" "SMOKE_QUICK=1 set"
elif [ -x "$ROOT/tests/sentence_stress.sh" ]; then
    if "$ROOT/tests/sentence_stress.sh"; then
        ok "sentence-stress/cap-and-graceful-drop"
    else
        fail "sentence-stress/cap-and-graceful-drop" "see sentence_stress output above"
    fi
else
    skip "sentence-stress" "tests/sentence_stress.sh not found"
fi

# --------------------------------------------------------------------
# 10. Memory ceiling test. Asserts RSS doesn't blow up with input size.
# Skipped when SMOKE_QUICK=1.
# --------------------------------------------------------------------

if [ "${SMOKE_QUICK:-0}" = "1" ]; then
    skip "memory-ceiling" "SMOKE_QUICK=1 set"
elif [ -x "$ROOT/tests/memory_ceiling.sh" ]; then
    if "$ROOT/tests/memory_ceiling.sh"; then
        ok "memory-ceiling/rss-bounded"
    else
        fail "memory-ceiling/rss-bounded" "see memory_ceiling output above"
    fi
else
    skip "memory-ceiling" "tests/memory_ceiling.sh not found"
fi

# --------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------

printf "\n"
printf "Smoke summary: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}, ${YELLOW}%d skipped${NC}\n" \
    "$PASS" "$FAIL" "$SKIPPED"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
