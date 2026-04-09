#!/usr/bin/env bash
# Hook: PostCompact → feed conversation summary to tardygrada daemon via monitor command
# This is the key hook that closes the output verification gap.
# Compaction summaries contain the full conversation condensed — tardygrada
# verifies it against the palace and stores it for future contradiction detection.

TARDY="${TARDY_BIN:-tardygrada}"
SOCK="/tmp/tardygrada.sock"

# Bail silently if deps missing (PostCompact is not blocking)
command -v jq >/dev/null 2>&1 || exit 0
command -v socat >/dev/null 2>&1 || { command -v "$TARDY" >/dev/null 2>&1 || exit 0; }

# Extract compaction summary from stdin JSON
# PostCompact sends: {"tool_response":{"summary":"condensed conversation..."}}
SUMMARY=$(jq -r '.tool_response.summary // empty' 2>/dev/null)

if [ -z "$SUMMARY" ]; then
  exit 0
fi

# Truncate to 4KB for verification
SUMMARY="${SUMMARY:0:4096}"

# Escape for JSON: newlines, quotes, backslashes, tabs
ESCAPED=$(printf '%s' "$SUMMARY" | jq -Rs '.')
# jq -Rs wraps in quotes, strip them for embedding
ESCAPED="${ESCAPED:1:${#ESCAPED}-2}"

# Try daemon socket first (fast path)
if [ -S "$SOCK" ]; then
  RESPONSE=$(printf '{"cmd":"monitor","text":"%s","wing":"claude-session"}\n' "$ESCAPED" | \
    socat -t5 - UNIX-CONNECT:"$SOCK" 2>/dev/null)

  if [ -n "$RESPONSE" ]; then
    CONTRADICTIONS=$(echo "$RESPONSE" | jq -r '.contradictions // 0' 2>/dev/null)
    if [ "$CONTRADICTIONS" -gt 0 ] 2>/dev/null; then
      jq -n --arg ctx "[TARDYGRADA] Compaction summary has $CONTRADICTIONS contradiction(s) with session history. Review the conversation for inconsistencies." \
        '{hookSpecificOutput:{hookEventName:"PostCompact",additionalContext:$ctx}}'
    fi
    exit 0
  fi
fi

# Fallback: use CLI monitor command
if command -v "$TARDY" >/dev/null 2>&1; then
  RESPONSE=$("$TARDY" monitor "$SUMMARY" 2>/dev/null)
  CONTRADICTIONS=$(echo "$RESPONSE" | jq -r '.contradictions // 0' 2>/dev/null)
  if [ "$CONTRADICTIONS" -gt 0 ] 2>/dev/null; then
    jq -n --arg ctx "[TARDYGRADA] Compaction summary has $CONTRADICTIONS contradiction(s) with session history." \
      '{hookSpecificOutput:{hookEventName:"PostCompact",additionalContext:$ctx}}'
  fi
fi
