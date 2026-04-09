#!/usr/bin/env bash
# Hook: SessionStart → auto-activate tardygrada monitoring
# New session = clean palace. No carryover from previous conversations.

TARDY="${TARDY_BIN:-tardygrada}"
PALACE_FILE="/tmp/tardygrada-palace.dat"

# HARD GATE: check dependencies
for DEP in "$TARDY" jq; do
  if ! command -v "$DEP" >/dev/null 2>&1; then
    jq -n --arg msg "[TARDYGRADA] Missing dependency: $DEP. Install it to enable monitoring." \
      '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:$msg}}' 2>/dev/null \
      || echo "{\"hookSpecificOutput\":{\"hookEventName\":\"SessionStart\",\"additionalContext\":\"[TARDYGRADA] Missing dependency: $DEP\"}}"
    exit 0
  fi
done

# ---- STEP 1: Kill any existing daemon hard (it caches palace in memory) ----
"$TARDY" daemon stop >/dev/null 2>&1 || true
# Force kill if graceful stop didn't work (daemon may ignore stop if busy)
pkill -f "tardygrada daemon" >/dev/null 2>&1 || true
sleep 0.2

# ---- STEP 2: Wipe ALL state for a truly clean session ----
rm -f "$PALACE_FILE" 2>/dev/null || true
rm -f /tmp/tardygrada-daemon.sock /tmp/tardygrada-daemon.pid /tmp/tardygrada.sock 2>/dev/null || true
rm -rf /tmp/tardygrada-persist 2>/dev/null || true
rm -f /tmp/targy-monitor/*.md 2>/dev/null || true

# ---- STEP 3: Start fresh daemon with empty palace ----
"$TARDY" daemon start >/dev/null 2>&1 || true
sleep 0.3

DAEMON_OK=false
if "$TARDY" daemon status >/dev/null 2>&1; then
  DAEMON_OK=true
fi

# ---- STEP 4: Record session start as first fact ----
"$TARDY" remember claude-session -- "Session started at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >/dev/null 2>&1 || true

if [ "$DAEMON_OK" = true ]; then
  jq -n '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:"[TARDYGRADA] Monitoring ACTIVE. Fresh palace. Daemon running. Every claim will be verified. You MUST run targy-monitor.sh output before completing any response with factual claims."}}'
else
  jq -n '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:"[TARDYGRADA] WARNING: Daemon failed to start. Monitoring is DEGRADED — verify-doc still works but palace persistence may be limited."}}'
fi
