#!/usr/bin/env bash
# Hook: Stop → verify palace is active, clean up temp files
# Fixes: M2 (silent failure), H5 (unquoted TARDY), H3 (cleanup), L4 (consistent JSON)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARDY="${TARDY_BIN:-tardygrada}"
TARDY_MONITOR="$SCRIPT_DIR/targy-monitor.sh"
TMP="/tmp/targy-monitor"

# Clean up temp files from this session
rm -f "$TMP"/*.md 2>/dev/null || true

# Check palace status
if command -v "$TARDY" >/dev/null 2>&1; then
  PALACE_CHECK=$("$TARDY_MONITOR" status 2>&1) || true

  if echo "$PALACE_CHECK" | grep -q "facts in wing:claude-session"; then
    FACT_COUNT=$(echo "$PALACE_CHECK" | grep -o '[0-9]* facts' | head -1)
    # Use jq for consistent JSON if available, fallback to echo
    if command -v jq >/dev/null 2>&1; then
      jq -n --arg msg "[Tardygrada] Session complete. Palace has $FACT_COUNT in claude-session wing." \
        '{hookSpecificOutput:{hookEventName:"Stop",additionalContext:$msg}}'
    else
      echo "{\"hookSpecificOutput\":{\"hookEventName\":\"Stop\",\"additionalContext\":\"[Tardygrada] Session complete.\"}}"
    fi
  else
    if command -v jq >/dev/null 2>&1; then
      jq -n '{hookSpecificOutput:{hookEventName:"Stop",additionalContext:"[Tardygrada] WARNING: No facts found in palace. Monitoring may not have been active this session."}}'
    fi
  fi
fi
