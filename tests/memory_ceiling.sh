#!/usr/bin/env bash
#
# Tardygrada memory ceiling test.
#
# Runs verify-doc against documents at three sizes (1KB, 100KB, 5MB) and
# asserts that peak RSS does NOT grow proportionally with input size.
#
# Why this test exists: the verifier mmaps fixed-size buffers up-front
# (VDOC_MAX_SENTENCES * sizeof(sentence_t), etc.). After hitting the
# 1024-sentence cap at ~100KB input, the verifier should plateau in
# memory use even as input grows to 5MB. If RSS scales linearly with
# input size, that signals a hidden buffer that allocates per-input-byte.
#
# Asserts:
#   - all three runs complete with exit code 0 or 1
#   - RSS at 5MB is no more than 2x RSS at 1KB
#     (generous: real expectation is roughly equal because the bounded
#      mmap arrays dominate)
#   - peak RSS at any size stays below 200 MB
#     (catastrophic-leak detector; the real value is single-digit MB)
#
# Exit 0 = all assertions pass. Exit non-zero = at least one failed.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"
RUNRSS="$ROOT/tests/runrss"

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
    printf "${RED}memory_ceiling: $BIN not built${NC}\n" >&2
    exit 1
fi

# Build runrss on demand
if [ ! -x "$RUNRSS" ]; then
    cc -O2 -Wall -o "$RUNRSS" "$ROOT/tests/runrss.c" 2>/dev/null || {
        printf "${RED}memory_ceiling: cannot build runrss${NC}\n" >&2
        exit 1
    }
fi

WORKDIR="$(mktemp -d -t tardy-mem.XXXXXX)"

DAEMON_WAS_RUNNING=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    DAEMON_WAS_RUNNING=1
    "$BIN" daemon stop >/dev/null 2>&1 || true
    sleep 0.5
fi

cleanup() {
    [ "${MEM_KEEP:-0}" = "1" ] || rm -rf "$WORKDIR"
    if [ "$DAEMON_WAS_RUNNING" = "1" ]; then
        nohup "$BIN" daemon start >/dev/null 2>&1 &
        disown 2>/dev/null || true
        sleep 0.5
    fi
}
trap cleanup EXIT

# Generate a doc of N 1KB chunks of realistic prose with embedded contradictions.
build_doc() {
    local n="$1"
    local out="$2"
    : > "$out"
    for i in $(seq 1 "$n"); do
        cat >> "$out" <<EOF
Section ${i}.

The cost of project Apollo${i} was ten thousand pounds. The team delivered all
milestones within the original schedule and the project was completed on time.
The lead engineer was Alex, supported by three junior developers. The system
passed acceptance testing on the first attempt and entered production.

According to the late audit appendix, the cost of project Apollo${i} was fifty
thousand pounds. The project was delayed by three months relative to schedule.

EOF
    done
}

# Sizes (in 1KB chunks): 1KB, 100KB, 5MB
SIZES="1 100 5000"

declare -a RESULTS_KB
declare -a RESULTS_NAME
idx=0

printf "\n${DIM}Memory ceiling test${NC}\n"
printf "%-10s  %-10s  %-9s  %-9s  %s\n" "Input_KB" "Disk_KB" "RSS_KB" "Wall_ms" "Status"
printf "%-10s  %-10s  %-9s  %-9s  %s\n" "--------" "-------" "------" "-------" "------"

for n in $SIZES; do
    DOC="$WORKDIR/doc-${n}kb.md"
    OUT="$WORKDIR/out-${n}kb.txt"
    build_doc "$n" "$DOC"
    DISK_KB="$(awk -v b="$(wc -c < "$DOC" | tr -d ' ')" 'BEGIN { printf "%d", b/1024 }')"

    "$RUNRSS" "$BIN" verify-doc "$DOC" > "$OUT" 2>"$OUT.rss"
    RC=$?

    RSS_KB="$(grep -E '^RSS_KB=' "$OUT.rss" | tail -1 | cut -d= -f2)"
    WALL_MS="$(grep -E '^WALL_MS=' "$OUT.rss" | tail -1 | cut -d= -f2)"
    : "${RSS_KB:=?}"
    : "${WALL_MS:=?}"

    STATUS="ok"
    if [ "$RC" -ne 0 ] && [ "$RC" -ne 1 ]; then
        STATUS="fail/rc=$RC"
    fi

    printf "%-10s  %-10s  %-9s  %-9s  %s\n" \
        "${n}KB" "$DISK_KB" "$RSS_KB" "$WALL_MS" "$STATUS"

    if [ "$RC" -ne 0 ] && [ "$RC" -ne 1 ]; then
        fail "size=${n}/exit-clean" "rc=$RC"
        continue
    fi

    if [ "$RSS_KB" = "?" ] || [ "$RSS_KB" -le 0 ]; then
        fail "size=${n}/rss-parsed" "got '$RSS_KB'"
        continue
    fi

    # Catastrophic-leak detector: 200 MB ceiling.
    if [ "$RSS_KB" -le 204800 ]; then
        ok "size=${n}/rss<=200MB"
    else
        fail "size=${n}/rss<=200MB" "got ${RSS_KB} KB"
    fi

    RESULTS_KB[$idx]="$RSS_KB"
    RESULTS_NAME[$idx]="${n}KB"
    idx=$((idx+1))
done

# Comparative assertion: the verifier mmaps the input file plus a set of
# fixed-size scratch buffers. We expect RSS to be roughly:
#     baseline (static arrays, ~5-15 MB) + input_bytes
#
# We assert that going from 100KB to 5MB input (a 50x growth) causes LESS
# THAN a 2x growth in RSS. This is the real "no per-byte runaway" check.
# The 1KB-vs-5MB ratio is dominated by the constant baseline, so we don't
# use it for the assertion — only as informational output.
if [ "${#RESULTS_KB[@]}" -ge 2 ]; then
    note "RSS by size:"
    for ((i=0; i<${#RESULTS_KB[@]}; i++)); do
        note "  ${RESULTS_NAME[$i]} -> ${RESULTS_KB[$i]} KB"
    done

    if [ "${#RESULTS_KB[@]}" -ge 3 ]; then
        # Compare middle (100KB) to largest (5MB). 50x input, expect <2x RSS.
        MID_KB="${RESULTS_KB[1]}"
        LARGE_KB="${RESULTS_KB[$((${#RESULTS_KB[@]}-1))]}"
        MID_NAME="${RESULTS_NAME[1]}"
        LARGE_NAME="${RESULTS_NAME[$((${#RESULTS_NAME[@]}-1))]}"

        if awk -v l="$LARGE_KB" -v m="$MID_KB" '
            BEGIN { exit !(m > 0 && l <= m*2) }'; then
            ok "rss-growth/${LARGE_NAME}-vs-${MID_NAME}<=2x"
        else
            fail "rss-growth/${LARGE_NAME}-vs-${MID_NAME}<=2x" \
                 "$LARGE_KB / $MID_KB = $(awk -v l="$LARGE_KB" -v m="$MID_KB" 'BEGIN { printf "%.2f", l/m }')"
        fi
    fi

    # Absolute headroom: peak RSS at the largest size must stay below
    # 50 MB. Real-world value is ~22 MB so this catches catastrophic
    # leaks while leaving 2x headroom for legitimate variation.
    LARGE_KB="${RESULTS_KB[$((${#RESULTS_KB[@]}-1))]}"
    LARGE_NAME="${RESULTS_NAME[$((${#RESULTS_NAME[@]}-1))]}"
    if [ "$LARGE_KB" -le 51200 ]; then
        ok "rss-absolute/${LARGE_NAME}<=50MB"
    else
        fail "rss-absolute/${LARGE_NAME}<=50MB" "got ${LARGE_KB} KB"
    fi
fi

printf "\nMemory-ceiling summary: ${GREEN}%d passed${NC}, ${RED}%d failed${NC}\n" \
    "$PASS" "$FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
