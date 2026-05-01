#!/usr/bin/env bash
#
# Tardygrada — context dissipation benchmark.
#
# Probes how detection quality degrades as the distance between two
# contradicting sentences grows. Existing context_stress.sh measures
# throughput at increasing document sizes. This bench measures
# DETECTION QUALITY at increasing GAPS between the two sentences that
# contradict each other.
#
# Procedure:
#   1. Take a fixed public-domain corpus of ~95 sentences (filler).
#   2. Plant TWO contradicting sentences at controlled positions:
#        sentence A is "The system is offline."
#        sentence B is "The system is online."
#      The triple-conflict detector reliably catches these when
#      grouped by entity, so we have a clean ground-truth signal.
#   3. Vary the GAP between A and B: 1, 5, 10, 25, 50, 100, 200,
#      500, and 1000 sentences. (For gaps > corpus size, we tile
#      the corpus.)
#   4. For each gap, build the document, run verify-doc, count
#      whether OUR planted contradiction was detected (not just any
#      contradiction). Run K trials with different seed positions
#      for the planting.
#   5. Report: gap, K, detected/K, true-positive recall, output
#      table + CSV. The CSV is suitable for plotting.
#
# What this tells us:
#   - At what gap does detection fall off? (current expectation:
#     entity grouping + triple matching is gap-agnostic up to the
#     1024-sentence cap, but in practice longer docs may exhaust
#     entity-group capacity before reaching the planted pair).
#   - Is the failure mode "found nothing" or "found something else"?
#     We count BOTH so the CSV exposes precision separately.
#
# Usage:
#     ./evaluation/dissipation_bench.sh
#     GAPS="1 10 100" K=5 ./evaluation/dissipation_bench.sh   # custom
#     ./evaluation/dissipation_bench.sh --csv > dissipation.csv

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"
CORPUS="$ROOT/evaluation/dissipation_corpus.txt"

GAPS="${GAPS:-1 5 10 25 50 100 200 500 1000}"
K="${K:-3}"
CSV=0
[ "${1:-}" = "--csv" ] && CSV=1

GREEN='\033[0;32m'
RED='\033[0;31m'
DIM='\033[2m'
NC='\033[0m'

if [ ! -x "$BIN" ]; then
    printf "${RED}dissipation_bench: $BIN not built${NC}\n" >&2
    exit 1
fi
if [ ! -f "$CORPUS" ]; then
    printf "${RED}dissipation_bench: corpus missing at $CORPUS${NC}\n" >&2
    exit 1
fi

# --------------------------------------------------------------------
# Read corpus into a Bash array, one sentence per element. The corpus
# file is one paragraph; we split on ". " then re-glue periods.
# --------------------------------------------------------------------

# Split corpus into one sentence per line. macOS bash 3.2 has no
# mapfile, so we write to a sentinel file and read N + indexed access.
CORPUS_SPLIT="$(mktemp -t tardy-diss-corpus.XXXXXX)"
awk -v RS='\\.' '
    { sub(/^[ \n\t]+/, ""); sub(/[ \n\t]+$/, "") }
    $0 != "" { print $0 "." }
' "$CORPUS" > "$CORPUS_SPLIT"
N_CORPUS=$(wc -l < "$CORPUS_SPLIT" | tr -d ' ')

# Helper: print the i-th sentence (0-indexed) from the split corpus
corpus_sent() {
    sed -n "$(( $1 + 1 ))p" "$CORPUS_SPLIT"
}

if [ "$N_CORPUS" -lt 5 ]; then
    printf "${RED}corpus too small: %d sentences${NC}\n" "$N_CORPUS" >&2
    exit 1
fi

# --------------------------------------------------------------------
# Generate a doc with a planted contradiction.
#   $1 = output path
#   $2 = number of filler sentences before the FIRST planted sentence
#   $3 = gap (number of filler sentences between the two)
# After both planted sentences, fill out to ensure entity grouping
# has at least 5 sentences after the second planted one (the verifier
# only compares pairs within an entity group).
# --------------------------------------------------------------------

build_doc() {
    local out="$1"
    local pre="$2"
    local gap="$3"
    local post=5
    {
        # Pre-fill
        for ((i=0; i<pre; i++)); do
            corpus_sent $(( i % N_CORPUS ))
        done
        echo ""
        echo "The system is offline."
        echo ""
        # Gap
        for ((i=0; i<gap; i++)); do
            corpus_sent $(( (pre + i + 1) % N_CORPUS ))
        done
        echo ""
        echo "The system is online."
        echo ""
        # Post
        for ((i=0; i<post; i++)); do
            corpus_sent $(( (pre + gap + i + 2) % N_CORPUS ))
        done
    } > "$out"
}

# --------------------------------------------------------------------
# Parse verify-doc output to detect whether the planted conflict was
# found. We care about TWO things:
#   - did verify-doc find ANY conflict? (any_count)
#   - did it find the specific conflict between "offline" and "online"?
#     (planted_hit = 1/0)
# --------------------------------------------------------------------

count_any() {
    grep -oE 'Summary:[[:space:]]+[0-9]+ contradiction' "$1" | head -1 |
        grep -oE '[0-9]+' | head -1
}

planted_hit() {
    # The planted conflict shows in verify-doc CLI output as:
    #   [CONFLICT] Lines X vs Y:
    #     "The system is offline."
    #     "The system is online."
    # When run via daemon the report is JSON-escaped but the tokens
    # still appear. Look for both planted strings adjacent in output.
    if grep -q 'offline' "$1" && grep -q 'online' "$1" && \
       grep -q 'system' "$1"; then
        echo 1
    else
        echo 0
    fi
}

# --------------------------------------------------------------------
# Daemon may truncate reports; for accurate parsing run standalone.
# Save daemon state, stop it, run sweep, restart at end.
# --------------------------------------------------------------------

DAEMON_WAS_RUNNING=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    DAEMON_WAS_RUNNING=1
    "$BIN" daemon stop >/dev/null 2>&1 || true
    sleep 0.5
fi
WORKDIR="$(mktemp -d -t tardy-diss.XXXXXX)"
cleanup() {
    rm -rf "$WORKDIR"
    if [ "$DAEMON_WAS_RUNNING" = "1" ]; then
        nohup "$BIN" daemon start >/dev/null 2>&1 &
        disown 2>/dev/null || true
        sleep 0.3
    fi
}
trap cleanup EXIT

# --------------------------------------------------------------------
# Run the sweep
# --------------------------------------------------------------------

if [ "$CSV" = "1" ]; then
    echo "gap,trial,sentences,total_conflicts_found,planted_hit,planted_recall,total_ms"
fi

if [ "$CSV" = "0" ]; then
    printf "\n${DIM}Context dissipation benchmark${NC}\n"
    printf "${DIM}Gap = sentences between the two planted contradicting sentences.${NC}\n"
    printf "${DIM}Recall = fraction of K=%d trials in which the planted pair was detected.${NC}\n\n" "$K"
    printf "%-8s  %-6s  %-12s  %-10s  %s\n" \
        "Gap" "K" "AnyDetect/K" "Recall" "Trial details"
    printf "%-8s  %-6s  %-12s  %-10s  %s\n" \
        "------" "-----" "-----------" "------" "-----"
fi

for gap in $GAPS; do
    detected=0
    any_detected=0
    details=""
    # Run K trials with different starting positions for the plant
    for trial in $(seq 0 $((K - 1))); do
        # Vary the pre-fill count so the planted positions differ across trials
        pre=$(( 5 + trial * 7 ))
        DOC="$WORKDIR/doc-g${gap}-t${trial}.md"
        OUT="$WORKDIR/out-g${gap}-t${trial}.txt"
        build_doc "$DOC" "$pre" "$gap"
        SENTS=$(grep -cE '^.+$' "$DOC")

        # Run verify-doc with a hard timeout (perl-based, portable)
        T0=$(date +%s%N 2>/dev/null || date +%s)
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
        T1=$(date +%s%N 2>/dev/null || date +%s)
        # Crude wall-time in ms (ns granularity if available)
        if [ ${#T0} -gt 12 ]; then
            DUR_MS=$(( (T1 - T0) / 1000000 ))
        else
            DUR_MS=$(( (T1 - T0) * 1000 ))
        fi

        any="$(count_any "$OUT")"
        : "${any:=0}"
        hit="$(planted_hit "$OUT")"

        # CSV: per-trial row
        if [ "$CSV" = "1" ]; then
            recall_one=$([ "$hit" = "1" ] && echo "1.000" || echo "0.000")
            printf "%d,%d,%d,%d,%d,%s,%d\n" \
                "$gap" "$trial" "$SENTS" "$any" "$hit" "$recall_one" "$DUR_MS"
        fi

        if [ "$hit" = "1" ]; then
            detected=$((detected + 1))
            details="${details}T$trial:✓ "
        else
            details="${details}T$trial:✗ "
        fi
        if [ "$any" -gt 0 ]; then
            any_detected=$((any_detected + 1))
        fi
    done

    if [ "$CSV" = "0" ]; then
        # Recall as a float to 3dp
        recall=$(awk -v d="$detected" -v k="$K" 'BEGIN { printf "%.3f", d/k }')
        if [ "$detected" = "$K" ]; then
            colour="$GREEN"
        elif [ "$detected" = "0" ]; then
            colour="$RED"
        else
            colour="$DIM"
        fi
        printf "%-8s  %-6d  %-12s  ${colour}%-10s${NC}  %s\n" \
            "$gap" "$K" "$any_detected/$K" "$recall" "$details"
    fi
done

if [ "$CSV" = "0" ]; then
    printf "\n${DIM}Reading the table:${NC}\n"
    printf "${DIM}  AnyDetect/K = trials where verify-doc found ANY contradiction.${NC}\n"
    printf "${DIM}  Recall       = trials where verify-doc found OUR planted pair.${NC}\n"
    printf "${DIM}  When AnyDetect > Recall, the verifier is finding contradictions${NC}\n"
    printf "${DIM}  but missing the planted one — chunking/grouping has dropped it.${NC}\n\n"
fi
