#!/usr/bin/env bash
# Hook: PostToolUse → feed tool outputs to tardygrada via daemon monitor command (async)
# Uses socket when daemon is running (fast), falls back to CLI binary.

TARDY="${TARDY_BIN:-tardygrada}"
SOCK="/tmp/tardygrada.sock"

# Bail silently if dependencies missing (this hook is async, don't block)
command -v jq >/dev/null 2>&1 || exit 0

# Extract tool name and response from stdin JSON
INPUT=$(cat)
TOOL_NAME=$(echo "$INPUT" | jq -r '.tool_name // empty' 2>/dev/null)
TOOL_RESPONSE=$(echo "$INPUT" | jq -r '(.tool_response | if type == "object" then tostring else . end) // empty' 2>/dev/null)

# Skip TodoWrite/ToolSearch entirely — pure noise
case "$TOOL_NAME" in
  TodoWrite|ToolSearch)
    exit 0
    ;;
esac

# For Read/Grep/Glob: store metadata only, not full content
case "$TOOL_NAME" in
  Read|Grep|Glob)
    TOOL_INPUT=$(echo "$INPUT" | jq -r '(.tool_input | if type == "object" then tostring else . end) // empty' 2>/dev/null)
    MONITOR_TEXT="[$TOOL_NAME] input: ${TOOL_INPUT:0:200}"
    ;;
  *)
    if [ -z "$TOOL_RESPONSE" ] || [ "$TOOL_RESPONSE" = "null" ]; then
      exit 0
    fi
    # Don't prefix with tool name — it confuses triple decomposition
    MONITOR_TEXT="${TOOL_RESPONSE:0:2048}"
    ;;
esac

# Escape text for JSON embedding
ESCAPED=$(printf '%s' "$MONITOR_TEXT" | jq -Rs '.')
ESCAPED="${ESCAPED:1:${#ESCAPED}-2}"

# Fast path: daemon socket
if [ -S "$SOCK" ] && command -v socat >/dev/null 2>&1; then
  RESPONSE=$(printf '{"cmd":"monitor","text":"%s","wing":"claude-session"}\n' "$ESCAPED" | \
    socat -t3 - UNIX-CONNECT:"$SOCK" 2>/dev/null) || true

  if [ -n "$RESPONSE" ]; then
    CONTRADICTIONS=$(echo "$RESPONSE" | jq -r '.contradictions // 0' 2>/dev/null)
    if [ "$CONTRADICTIONS" -gt 0 ] 2>/dev/null; then
      jq -n --arg ctx "[TARDYGRADA] Tool output from $TOOL_NAME contradicts session history ($CONTRADICTIONS conflict(s))" \
        '{hookSpecificOutput:{hookEventName:"PostToolUse",additionalContext:$ctx}}'
    fi
    exit 0
  fi
fi

# Fallback: CLI binary
if command -v "$TARDY" >/dev/null 2>&1; then
  RESPONSE=$("$TARDY" monitor "$MONITOR_TEXT" 2>/dev/null) || true
  if [ -n "$RESPONSE" ]; then
    CONTRADICTIONS=$(echo "$RESPONSE" | jq -r '.contradictions // 0' 2>/dev/null)
    if [ "$CONTRADICTIONS" -gt 0 ] 2>/dev/null; then
      jq -n --arg ctx "[TARDYGRADA] Tool output from $TOOL_NAME contradicts session history ($CONTRADICTIONS conflict(s))" \
        '{hookSpecificOutput:{hookEventName:"PostToolUse",additionalContext:$ctx}}'
    fi
  fi
fi
