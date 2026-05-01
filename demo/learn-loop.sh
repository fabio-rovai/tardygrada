#!/usr/bin/env bash
#
# Tardygrada learn-loop demo — LLM proposes, Tardygrada filters, ontology
# accumulates.
#
# This is the smallest credible demo of "world-model growth from a
# generator" using only what's already in the repo. The generator role
# is played by anyone (a subagent, an LLM, a human) who emits N-Triples
# in the standard format. This script:
#
#   1. Shows what's currently in the ontology
#   2. Reads a candidate batch (N-Triples) from a file or stdin
#   3. Appends them to the live ontology (.nt file)
#   4. Restarts the daemon to load the new facts
#   5. Verifies each candidate as a natural-language claim
#   6. Reports accepted / conflict / gap counts
#
# Architecture this exercises:
#   - Generator (the LLM / subagent / you) produces candidate triples
#   - Tardygrada's BFT 3-pass + Datalog inference verifies each
#   - tardy_crdt_dry_merge logic (in self.c) flags structural conflicts
#   - The .nt file is the durable, human-readable accumulated knowledge
#
# Use:
#     ./demo/learn-loop.sh path/to/batch.nt
#     # or pipe candidates in:
#     cat my-batch.nt | ./demo/learn-loop.sh -
#
# Schema for each candidate line:
#     <http://tardygrada.org/SUBJECT> <http://schema.org/PRED> <http://tardygrada.org/OBJECT> .
#
# After running, the ontology is permanently updated. If you regret a
# learning batch, `git checkout tests/wikidata_common.nt` reverts it.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"
ONT="$ROOT/tests/wikidata_common.nt"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
DIM='\033[2m'
NC='\033[0m'

if [ ! -x "$BIN" ]; then
    printf "${RED}learn-loop: $BIN not built. Run 'make' first.${NC}\n" >&2
    exit 1
fi

if [ "${1:-}" = "-" ]; then
    BATCH_FILE="$(mktemp)"
    cat > "$BATCH_FILE"
    trap 'rm -f "$BATCH_FILE"' EXIT
elif [ -n "${1:-}" ] && [ -f "$1" ]; then
    BATCH_FILE="$1"
else
    cat <<EOF >&2
Tardygrada learn-loop. Usage:

    ./demo/learn-loop.sh path/to/batch.nt
    cat batch.nt | ./demo/learn-loop.sh -

Each line of the batch is one candidate triple in N-Triples form:

    <http://tardygrada.org/SUBJECT> <http://schema.org/PRED> <http://tardygrada.org/OBJECT> .

The script appends accepted facts to ${ONT}.
EOF
    exit 1
fi

CANDIDATE_COUNT=$(grep -cE '^<http' "$BATCH_FILE")
if [ "$CANDIDATE_COUNT" -eq 0 ]; then
    printf "${RED}No candidate triples in batch (need lines starting with <http).${NC}\n" >&2
    exit 1
fi

# 1. Show pre-state
PRE=$(wc -l < "$ONT" | tr -d ' ')
printf "${DIM}Ontology before:  %d facts at %s${NC}\n" "$PRE" "$ONT"
printf "${DIM}Candidates:       %d in %s${NC}\n\n" "$CANDIDATE_COUNT" "$BATCH_FILE"

# 2. Append the batch to the ontology
cat "$BATCH_FILE" >> "$ONT"
POST=$(wc -l < "$ONT" | tr -d ' ')
printf "${DIM}Appended. Ontology now %d facts.${NC}\n" "$POST"

# 3. Restart the daemon so it picks up the new facts
DAEMON_NEEDS_RESTART=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    DAEMON_NEEDS_RESTART=1
    "$BIN" daemon stop >/dev/null 2>&1 || true
    sleep 1
fi
nohup "$BIN" daemon start >/dev/null 2>&1 &
disown 2>/dev/null || true
sleep 2
LIVE_COUNT=$(echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
    grep -oE '"agents":[0-9]+' | cut -d: -f2)
printf "${DIM}Daemon up. Agents loaded: %s${NC}\n\n" "${LIVE_COUNT:-?}"

# 4. Verify each candidate as a natural-language claim. Choose the
#    phrasing based on the predicate (capitalOf -> "X is in Y", creator
#    -> "X created Y", etc.). Default fallback: "X PRED Y" raw.
phrase_for() {
    local subj="$1" pred="$2" obj="$3"
    case "$pred" in
        capitalOf|location|locatedIn) echo "$subj is in $obj" ;;
        creator)                       echo "$subj created $obj" ;;
        founder)                       echo "$subj founded $obj" ;;
        inventor)                      echo "$subj invented $obj" ;;
        discoverer)                    echo "$subj discovered $obj" ;;
        *)                             echo "$subj $pred $obj" ;;
    esac
}

ACCEPTED=0
CONFLICT=0
GAP=0
OTHER=0

printf "${DIM}Verifying each candidate against the live verifier...${NC}\n\n"

while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$line" in '#'*) continue ;; esac
    case "$line" in '<http'*) ;;
                    *) continue ;;
    esac
    SUBJ=$(echo "$line" | sed -E 's|^<[^>]*org/([^>]+)>.*|\1|')
    PRED=$(echo "$line" | sed -E 's|^<[^>]+>[[:space:]]+<[^>]*org/([^>]+)>.*|\1|')
    OBJ=$(echo "$line"  | sed -E 's|.*org/([^>]+)>[[:space:]]*\.$|\1|')

    CLAIM=$(phrase_for "$SUBJ" "$PRED" "$OBJ")
    RESPONSE=$("$BIN" run "$CLAIM" 2>&1 | grep -oE '"result":"[^"]+"' | head -1 | cut -d'"' -f4)

    case "$RESPONSE" in
        VERIFIED)
            ACCEPTED=$((ACCEPTED+1))
            printf "  ${GREEN}✓${NC} %-46s %s\n" "$CLAIM" "VERIFIED"
            ;;
        contradiction|inconsistency)
            CONFLICT=$((CONFLICT+1))
            printf "  ${RED}✗${NC} %-46s %s\n" "$CLAIM" "$RESPONSE"
            ;;
        ontology_gap)
            GAP=$((GAP+1))
            printf "  ${YELLOW}?${NC} %-46s %s\n" "$CLAIM" "ontology_gap"
            ;;
        *)
            OTHER=$((OTHER+1))
            printf "  ${YELLOW}?${NC} %-46s %s\n" "$CLAIM" "${RESPONSE:-empty}"
            ;;
    esac
done < "$BATCH_FILE"

printf "\n${DIM}=== Loop summary ===${NC}\n"
printf "  ${GREEN}Accepted (VERIFIED)${NC}   %d / %d\n" "$ACCEPTED" "$CANDIDATE_COUNT"
printf "  ${RED}Conflict${NC}             %d\n" "$CONFLICT"
printf "  ${YELLOW}Ontology gap${NC}         %d\n" "$GAP"
printf "  ${YELLOW}Other${NC}                %d\n" "$OTHER"
printf "  Ontology growth      %d -> %d (+%d)\n\n" \
       "$PRE" "$POST" $((POST - PRE))

# Exit non-zero if any candidate failed to verify (useful in CI / loops)
if [ "$ACCEPTED" -lt "$CANDIDATE_COUNT" ]; then
    exit 2
fi
exit 0
