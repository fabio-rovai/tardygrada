#!/usr/bin/env bash
# Hook: PostToolUse → feed tool outputs to tardygrada palace (async)
# Fixes: C2 (JSON injection), H1 (jq check), H2 (race conditions), H3 (cleanup),
#        H5 (unquoted TARDY), M4 (Read/Grep metadata), M5 (timeouts)

TARDY="${TARDY_BIN:-tardygrada}"
TMP="/tmp/targy-monitor"
mkdir -p "$TMP"

# Bail silently if dependencies missing (this hook is async, don't block)
command -v jq >/dev/null 2>&1 || exit 0
command -v "$TARDY" >/dev/null 2>&1 || exit 0

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

# For Read/Grep/Glob: store metadata only (filename, match count), not full content
case "$TOOL_NAME" in
  Read|Grep|Glob)
    # Extract just the file path or pattern, not the full content
    TOOL_INPUT=$(echo "$INPUT" | jq -r '(.tool_input | if type == "object" then tostring else . end) // empty' 2>/dev/null)
    SUMMARY="[$TOOL_NAME] input: ${TOOL_INPUT:0:200}"
    "$TARDY" remember claude-session -- "$SUMMARY" >/dev/null 2>&1 || true
    exit 0
    ;;
esac

if [ -z "$TOOL_RESPONSE" ] || [ "$TOOL_RESPONSE" = "null" ]; then
  exit 0
fi

# Use up to 2KB for verification, 500 chars for palace storage
VERIFY_TEXT="${TOOL_RESPONSE:0:2048}"
STORE_TEXT="${TOOL_RESPONSE:0:500}"

# Verify tool output against palace (unique temp file)
TMPFILE=$(mktemp "$TMP/tool.XXXXXX.md")
printf '[%s] %s\n' "$TOOL_NAME" "$VERIFY_TEXT" > "$TMPFILE"
RESULT=$(timeout 5 "$TARDY" verify-doc "$TMPFILE" 2>/dev/null) || true
CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT" || true)
rm -f "$TMPFILE"

# Store in palace
"$TARDY" remember claude-session -- "[$TOOL_NAME] $STORE_TEXT" >/dev/null 2>&1 || true

if [ "$CONFLICTS" -gt 0 ]; then
  DETAILS=$(echo "$RESULT" | grep "CONFLICT\|->.*conflict" | head -3)
  # Use jq for safe JSON construction
  jq -n --arg ctx "[TARDYGRADA] Tool output from $TOOL_NAME contradicts session history ($CONFLICTS conflict(s)):
$DETAILS" \
    '{hookSpecificOutput:{hookEventName:"PostToolUse",additionalContext:$ctx}}'
fi
