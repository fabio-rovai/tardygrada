#!/usr/bin/env bash
#
# Tardygrada — 60-second guided tour.
#
# Walks a new user through every working feature of the repo as of v2.0.5.
# Prints what is happening, runs a real command, shows the output, moves on.
# No external dependencies (assumes the binary is already built).
#
# Run from the repo root:
#     ./demo/demo.sh
#
# Or with --pause to step through one section at a time:
#     ./demo/demo.sh --pause

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/tardygrada"

PAUSE=0
if [ "${1:-}" = "--pause" ]; then PAUSE=1; fi

# ---------- styling ----------
BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

section() {
    printf "\n${BOLD}${CYAN}━━ %s ━━${NC}\n" "$1"
    printf "${DIM}%s${NC}\n\n" "$2"
}

run() {
    printf "${YELLOW}\$ %s${NC}\n" "$1"
    eval "$1"
}

next() {
    if [ "$PAUSE" = "1" ]; then
        printf "\n${DIM}-- press enter to continue --${NC}"
        read -r _
    fi
}

# ---------- preflight ----------

if [ ! -x "$BIN" ]; then
    printf "${YELLOW}Binary not built. Running 'make' first...${NC}\n"
    (cd "$ROOT" && make >/dev/null 2>&1) || {
        printf "Build failed. Run 'make' manually first.\n"; exit 1;
    }
fi

# Ensure the daemon is running so `tardy run` and verify-doc go through
# the BFT 3-pass + ontology grounding path. Remember if we started it
# so we can leave the system as we found it.
DEMO_DAEMON_STARTED=0
if [ -S /tmp/tardygrada.sock ] && \
   echo '{"cmd":"status"}' | nc -U /tmp/tardygrada.sock 2>/dev/null | \
   grep -q '"ok":true'; then
    : # daemon already up
else
    nohup "$BIN" daemon start >/dev/null 2>&1 &
    disown 2>/dev/null || true
    sleep 1
    DEMO_DAEMON_STARTED=1
fi

cleanup() {
    if [ "$DEMO_DAEMON_STARTED" = "1" ]; then
        "$BIN" daemon stop >/dev/null 2>&1 || true
    fi
    rm -f /tmp/demo-doc-*.md
}
trap cleanup EXIT

# ============================================================
# 1. The binary
# ============================================================

section "1. The binary" \
    "Tardygrada is a single ~350 KB C binary, MIT-licensed, MCP-native."

run "ls -lh '$BIN' | awk '{print \$5, \$NF}'"
run "file '$BIN' 2>/dev/null | head -1 || echo '(skipped: file(1) unavailable)'"
next

# ============================================================
# 2. Daemon status
# ============================================================

section "2. Daemon status" \
    "The daemon mediates verification, holds the memory palace, and serves MCP."

run "echo '{\"cmd\":\"status\"}' | nc -U /tmp/tardygrada.sock"
next

# ============================================================
# 3. Headline grounding (the school metaphor in action)
# ============================================================

section "3. Headline grounding" \
    "The bundled tests/wikidata_common.nt has ~64 facts. Datalog rules derive locatedIn from capitalOf, etc."

run "$BIN run 'Paris is in France'"
run "$BIN run 'Tokyo is in Japan'"
run "$BIN run 'Doctor Who was created at BBC Television Centre'"
next

# ============================================================
# 4. Computational and constant verification
# ============================================================

section "4. Computational claims" \
    "Arithmetic and fundamental constants ground via tardy_inference_compute (no ontology needed)."

run "$BIN run '5 + 5 = 10'"
run "$BIN run 'The speed of light is 299792458 meters per second'"
next

# ============================================================
# 5. Honest gap: ungrounded claims do NOT silently pass
# ============================================================

section "5. Ungrounded claims return ontology_gap" \
    "Tardygrada does not invent evidence. With no grounding source, it admits it."

run "$BIN run 'The cat is invisible'"
run "$BIN run 'Quantum spaghetti is delicious'"
next

# ============================================================
# 6. Document verification (catches contradictions)
# ============================================================

section "6. Document verification" \
    "verify-doc finds contradictions across pairs of sentences, with line numbers."

DEMO_DOC="/tmp/demo-doc-$$.md"
cat > "$DEMO_DOC" <<'EOF'
# Status memo

The system is offline.

The system is online.

The team confirmed the rollout was successful.
EOF

printf "${DIM}Created %s with planted contradictions.${NC}\n\n" "$DEMO_DOC"
# Run via the daemon path (returns JSON), then pull out the 'report'
# field and unescape it so the CONFLICT block prints cleanly.
printf "${YELLOW}\$ %s verify-doc %s${NC}\n" "$BIN" "$DEMO_DOC"
"$BIN" verify-doc "$DEMO_DOC" 2>/dev/null | \
    python3 -c '
import json, sys
try:
    j = json.load(sys.stdin)
    print(j.get("report", "").strip())
    n = j.get("contradictions", "?")
    print("  -> contradictions:", n)
except Exception:
    pass
' 2>/dev/null || true
next

# ============================================================
# 7. Specialization curriculum (the school metaphor)
# ============================================================

section "7. The school metaphor — a curriculum file" \
    "examples/code-review.tardy is a verifiable program: a CodeReviewer agent with three receive() slots."

run "head -30 '$ROOT/examples/code-review.tardy'"
printf "\n${DIM}A generic agent loads this and behaves as a code reviewer with @sovereign trust\nand pipeline.min_passing_layers = 5. Run as MCP server:${NC}\n"
printf "    ${YELLOW}\$ ./tardygrada examples/code-review.tardy${NC}\n"
next

# ============================================================
# 8. Run the smoke regression suite
# ============================================================

section "8. Regression coverage" \
    "make test runs 23 assertions across 10 axes. SMOKE_QUICK=1 skips the slow benches."

run "SMOKE_QUICK=1 make -C '$ROOT' test 2>&1 | tail -15"

# ============================================================
# Wrap up
# ============================================================

printf "\n${BOLD}${GREEN}━━ Tour complete ━━${NC}\n\n"
printf "Next steps:\n"
printf "  ${CYAN}make test${NC}                full smoke suite (~25s)\n"
printf "  ${CYAN}cat README.md${NC}            full feature list and the school metaphor walkthrough\n"
printf "  ${CYAN}cat CHANGELOG.md${NC}         what changed in v2.0\n"
printf "  ${CYAN}cat SECURITY.md${NC}          reporting + trust boundaries\n"
printf "\n"
