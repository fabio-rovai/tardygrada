#!/usr/bin/env bash
# Hook: UserPromptSubmit → BLOCKING tardygrada verification
# Fixes: C2 (JSON injection), H1 (jq check), H2 (race conditions), H4 (missing output),
#        H5 (unquoted TARDY), M5 (timeouts), L1 (relative path check)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARDY="${TARDY_BIN:-tardygrada}"
TMP="/tmp/targy-monitor"
mkdir -p "$TMP"

# HARD GATE: check jq
if ! command -v jq >/dev/null 2>&1; then
  echo '{"continue":false,"stopReason":"[TARDYGRADA] jq not found on PATH. Session blocked. Install jq first."}'
  exit 0
fi

# HARD GATE: check tardygrada binary
if [ -n "${TARDY_BIN:-}" ] && [[ "$TARDY" == */* ]]; then
  # Relative or absolute path — use -x test
  if [ ! -x "$TARDY" ]; then
    jq -n --arg r "[TARDYGRADA] Binary not found at $TARDY. Session blocked." '{continue:false,stopReason:$r}'
    exit 0
  fi
else
  if ! command -v "$TARDY" >/dev/null 2>&1; then
    echo '{"continue":false,"stopReason":"[TARDYGRADA] Binary not found on PATH. Session blocked. Install: https://github.com/fabio-rovai/tardygrada"}'
    exit 0
  fi
fi

# HARD GATE: daemon must be running
if ! "$TARDY" daemon status >/dev/null 2>&1; then
  "$TARDY" daemon start >/dev/null 2>&1 || true
  if ! "$TARDY" daemon status >/dev/null 2>&1; then
    echo '{"continue":false,"stopReason":"[TARDYGRADA] Daemon failed to start. Session blocked. Run: tardygrada daemon start"}'
    exit 0
  fi
fi

# Extract user message from stdin JSON
MESSAGE=$(jq -r '.prompt // empty' 2>/dev/null)

if [ -z "$MESSAGE" ]; then
  echo '{"continue":true}'
  exit 0
fi

# Verify against palace (use unique temp file to avoid race conditions)
TMPFILE=$(mktemp "$TMP/input.XXXXXX.md")
# Use full message up to 4KB for verification (not 500 chars)
printf '%s\n' "${MESSAGE:0:4096}" > "$TMPFILE"

RESULT=$(timeout 5 "$TARDY" verify-doc "$TMPFILE" 2>/dev/null) || true
CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT" || true)

# Store in palace (truncated for storage, not for verification)
"$TARDY" remember claude-session -- "${MESSAGE:0:500}" >/dev/null 2>&1 || true

# Clean up temp file
rm -f "$TMPFILE"

if [ "$CONFLICTS" -gt 0 ]; then
  DETAILS=$(echo "$RESULT" | grep "CONFLICT\|->.*conflict" | head -5)
  # Use jq to build JSON safely — no injection possible
  jq -n --arg ctx "[TARDYGRADA WARNING] User input has $CONFLICTS contradiction(s) with session history:
$DETAILS
You MUST flag this to the user before responding." \
    '{hookSpecificOutput:{hookEventName:"UserPromptSubmit",additionalContext:$ctx}}'
else
  echo '{"continue":true}'
fi
