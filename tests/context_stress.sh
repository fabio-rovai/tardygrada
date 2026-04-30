#!/usr/bin/env bash
#
# Tardygrada context-size stress test.
#
# Generates synthetic documents at five sizes (1KB, 10KB, 50KB, 200KB, 500KB)
# with planted contradictions, runs `tardy verify-doc` against each, and
# reports a degradation table:
#
#   - wall time per size
#   - sentences parsed
#   - triples extracted
#   - planted contradictions vs detected
#   - whether internal caps (1024 sentences, 256 entities, 64 conflicts) are hit
#
# Asserts:
#   - verify-doc completes within a generous wall-time bound at each size
#   - detection still finds at least one planted contradiction at every size
#     the parser reaches (i.e. before hitting the 1024-sentence cap)
#   - the binary does not crash, hang, or return a non-zero exit code
#
# Exit 0 = all assertions pass. Exit non-zero = at least one failed.
#
# Usage:
#     ./tests/context_stress.sh             # run with default sizes
#     STRESS_SIZES="1 5 25" ./tests/...     # override sizes (in KB chunks)
#     STRESS_KEEP=1 ./tests/...             # keep generated docs in /tmp

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
DIM='\033[2m'
NC='\033[0m'

PASS=0
FAIL=0

ok()   { printf "  ${GREEN}PASS${NC} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  ${RED}FAIL${NC} %s -- %s\n" "$1" "$2"; FAIL=$((FAIL+1)); }
note() { printf "  ${DIM}%s${NC}\n" "$1"; }

if [ ! -x "$BIN" ]; then
    printf "${RED}context_stress: $BIN not built${NC}\n" >&2
    exit 1
fi

# Sizes in number of 1-KB filler chunks. Each chunk is ~1KB of realistic text
# plus exactly two contradictory sentences.
SIZES="${STRESS_SIZES:-1 10 50 200 500}"

WORKDIR="$(mktemp -d -t tardy-stress.XXXXXX)"

# If the daemon is running, stop it for the duration of this test. The
# daemon's verify-doc round-trip caps the captured report at ~5KB to fit
# the JSON response buffer, which truncates output for large docs and
# breaks our parsing. Standalone mode has no such cap.
DAEMON_WAS_RUNNING=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    DAEMON_WAS_RUNNING=1
    "$BIN" daemon stop >/dev/null 2>&1 || true
    sleep 0.5
fi

cleanup() {
    [ "${STRESS_KEEP:-0}" = "1" ] || rm -rf "$WORKDIR"
    if [ "$DAEMON_WAS_RUNNING" = "1" ]; then
        # Restart the daemon as a backgrounded process so it survives
        # this script's exit. Suppress its output.
        nohup "$BIN" daemon start >/dev/null 2>&1 &
        disown 2>/dev/null || true
        sleep 0.5
    fi
}
trap cleanup EXIT

# --------------------------------------------------------------------
# Generator: produces a 1KB chunk with two planted contradictions.
# Each chunk is identical so we can reason about how many contradictions
# SHOULD be findable and how the verifier scales.
# --------------------------------------------------------------------

generate_chunk() {
    local idx="$1"
    # Each chunk plants a numeric contradiction (cost = 10k pounds vs cost
    # = 50k pounds, 5x ratio so the >2x numeric heuristic triggers) plus a
    # subject-predicate-object contradiction (project status: completed vs
    # delayed). Filler text takes the chunk to ~1KB.
    cat <<EOF
Section ${idx}.

The cost of project Apollo${idx} was ten thousand pounds. The team delivered all
milestones within the original schedule and the project was completed on time.
The lead engineer was Alex, supported by three junior developers. The system
passed acceptance testing on the first attempt and entered production the
following Monday. Subsequent monitoring showed zero incidents over the first
ninety days of operation. The retrospective was held in person at the London
office and lasted ninety minutes. Stakeholders reported satisfaction with the
outcome and approved follow-on funding for phase two of the programme.

According to the late audit appendix, the cost of project Apollo${idx} was fifty
thousand pounds. The project was delayed by three months relative to schedule.

EOF
}

# Build a doc of N chunks
build_doc() {
    local n="$1"
    local out="$2"
    : > "$out"
    for i in $(seq 1 "$n"); do
        generate_chunk "$i" >> "$out"
    done
}

# --------------------------------------------------------------------
# Parse verify-doc output for metrics. Works whether the daemon path
# wrapped the report in JSON ("Sentences: 11\\n...") or the standalone
# path produced plain multi-line text ("Sentences: 11"). In both cases
# the substrings we want are present; we just regex them out.
# --------------------------------------------------------------------

parse_metric() {
    # $1 = label (e.g. "Sentences:" or "Triples extracted:"), $2 = file
    grep -oE "$1[[:space:]]+[0-9]+" "$2" | head -1 | grep -oE '[0-9]+' | head -1
}

parse_time_ms() {
    grep -oE 'Time:[[:space:]]+[0-9]+ms' "$1" | head -1 | grep -oE '[0-9]+' | head -1
}

parse_contradictions() {
    # Prefer the structured JSON field if present (daemon path).
    # Fall back to the "Summary: N contradiction" line (standalone).
    local v
    v="$(grep -oE '"contradictions":[0-9]+' "$1" | head -1 | grep -oE '[0-9]+' | head -1)"
    if [ -z "$v" ]; then
        v="$(grep -oE 'Summary:[[:space:]]+[0-9]+ contradiction' "$1" | head -1 | grep -oE '[0-9]+' | head -1)"
    fi
    echo "$v"
}

# --------------------------------------------------------------------
# Run one stress sample
# --------------------------------------------------------------------

printf "\n${DIM}Context-size stress test${NC}\n"
printf "%-8s  %-9s  %-9s  %-9s  %-12s  %-9s  %s\n" \
    "Size_KB" "Doc_KB" "Sents" "Triples" "Contradicts" "Time_ms" "Status"
printf "%-8s  %-9s  %-9s  %-9s  %-12s  %-9s  %s\n" \
    "-------" "------" "-----" "-------" "-----------" "-------" "------"

for n in $SIZES; do
    DOC="$WORKDIR/doc-${n}kb.md"
    OUT="$WORKDIR/out-${n}kb.txt"

    build_doc "$n" "$DOC"
    DOC_BYTES="$(wc -c < "$DOC" | tr -d ' ')"
    DOC_KB="$(awk -v b="$DOC_BYTES" 'BEGIN { printf "%.1f", b/1024 }')"

    # Per-size wall-time budget. Generous so we don't flake on slow machines.
    # Bench is ~5ms/doc on real hardware; we give 100x headroom plus 50ms/KB.
    BUDGET_MS="$(awk -v n="$n" 'BEGIN { print 1000 + 50*n }')"

    # Run with a hard timeout (kill after 30s) using perl for portability.
    # verify-doc legitimately returns exit code 1 to signal "contradictions
    # found" -- we treat both 0 (none) and 1 (some) as success. Only
    # timeout (124) or other codes count as a real failure.
    perl -e '
        use strict; use warnings; use POSIX;
        my $pid = fork(); die "fork: $!" unless defined $pid;
        if ($pid == 0) { exec @ARGV or POSIX::_exit(127); }
        my $deadline = time() + 30;
        while (1) {
            my $r = waitpid($pid, POSIX::WNOHANG);
            last if $r > 0;
            if (time() >= $deadline) { kill 9, $pid; waitpid($pid, 0); exit 124; }
            select undef, undef, undef, 0.05;
        }
        exit ($? >> 8);
    ' "$BIN" verify-doc "$DOC" > "$OUT" 2>&1
    RC=$?
    if [ "$RC" -eq 0 ] || [ "$RC" -eq 1 ]; then
        EXIT_OK=1
    else
        EXIT_OK=0
    fi

    SENTS="$(parse_metric 'Sentences:' "$OUT")"
    TRIPS="$(parse_metric 'Triples extracted:' "$OUT")"
    CONFLICTS="$(parse_contradictions "$OUT")"
    TIME_MS="$(parse_time_ms "$OUT")"

    : "${SENTS:=?}"
    : "${TRIPS:=?}"
    : "${CONFLICTS:=?}"
    : "${TIME_MS:=?}"

    STATUS="ok"
    if [ "$EXIT_OK" -ne 1 ]; then
        STATUS="fail/exit"
    elif [ "$TIME_MS" != "?" ] && \
         awk -v t="$TIME_MS" -v b="$BUDGET_MS" 'BEGIN { exit !(t+0 > b+0) }'; then
        STATUS="slow"
    fi

    printf "%-8s  %-9s  %-9s  %-9s  %-12s  %-9s  %s\n" \
        "${n}KB-x" "$DOC_KB" "$SENTS" "$TRIPS" "$CONFLICTS" "$TIME_MS" "$STATUS"

    # ---------- Assertions ----------
    if [ "$EXIT_OK" -ne 1 ]; then
        fail "size=${n}/exit-clean" "verify-doc returned non-zero or timed out"
        continue
    fi

    if [ "$TIME_MS" = "?" ]; then
        fail "size=${n}/parses-time" "could not parse Time: from output"
        continue
    fi

    if awk -v t="$TIME_MS" -v b="$BUDGET_MS" 'BEGIN { exit !(t+0 <= b+0) }'; then
        ok "size=${n}/time<=${BUDGET_MS}ms"
    else
        fail "size=${n}/time<=${BUDGET_MS}ms" "took ${TIME_MS}ms"
    fi

    # Below the 1024-sentence cap we should always find at least one
    # planted contradiction. At and above the cap we expect detection
    # to degrade gracefully -- still asserted to be > 0 because each
    # 1KB chunk plants two contradictions and the parser sees at least
    # the first chunk.
    if [ "$CONFLICTS" != "?" ] && \
       awk -v c="$CONFLICTS" 'BEGIN { exit !(c+0 >= 1) }'; then
        ok "size=${n}/detection>=1"
    else
        fail "size=${n}/detection>=1" "found $CONFLICTS contradictions"
    fi

    # If sentences hit the cap, note it (not a failure -- documented limit)
    if [ "$SENTS" != "?" ] && [ "$SENTS" -ge 1024 ]; then
        note "size=${n}: hit 1024-sentence cap (graceful)"
    fi
done

printf "\nContext-stress summary: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" \
    "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
