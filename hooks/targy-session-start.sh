#!/usr/bin/env bash
# Hook: SessionStart → auto-activate tardygrada monitoring
# Fixes: M1 (false daemon claim), H5 (unquoted TARDY), H1 (jq check)

TARDY="${TARDY_BIN:-tardygrada}"

# HARD GATE: check dependencies
for DEP in "$TARDY" jq; do
  if ! command -v "$DEP" >/dev/null 2>&1; then
    jq -n --arg msg "[TARDYGRADA] Missing dependency: $DEP. Install it to enable monitoring." \
      '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:$msg}}' 2>/dev/null \
      || echo "{\"hookSpecificOutput\":{\"hookEventName\":\"SessionStart\",\"additionalContext\":\"[TARDYGRADA] Missing dependency: $DEP\"}}"
    exit 0
  fi
done

# Start daemon if not running, verify it actually started
DAEMON_OK=false
if "$TARDY" daemon status >/dev/null 2>&1; then
  DAEMON_OK=true
else
  "$TARDY" daemon start >/dev/null 2>&1 || true
  if "$TARDY" daemon status >/dev/null 2>&1; then
    DAEMON_OK=true
  fi
fi

# Clean up stale temp files from previous sessions
rm -f /tmp/targy-monitor/*.md 2>/dev/null || true

# Store session start fact
"$TARDY" remember claude-session -- "Session started at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >/dev/null 2>&1 || true

if [ "$DAEMON_OK" = true ]; then
  jq -n '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:"[TARDYGRADA] Monitoring ACTIVE. Daemon running. Every claim will be verified against the palace. You MUST run targy-monitor.sh output before completing any response with factual claims."}}'
else
  jq -n '{hookSpecificOutput:{hookEventName:"SessionStart",additionalContext:"[TARDYGRADA] WARNING: Daemon failed to start. Monitoring is DEGRADED — verify-doc still works but palace persistence may be limited."}}'
fi
