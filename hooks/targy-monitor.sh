#!/usr/bin/env bash
# Tardygrada monitor hook for Claude Code
# Fixes: C1 (shell injection), C3 (grep -c exit code), H5 (unquoted TARDY),
#        H2 (race conditions), H3 (temp cleanup), M5 (timeouts), M6 (input validation)

TARDY="${TARDY_BIN:-tardygrada}"
WING="claude-session"
TMP="/tmp/targy-monitor"
mkdir -p "$TMP"

case "$1" in
  input)
    TMPFILE=$(mktemp "$TMP/input.XXXXXX.md")
    printf '%s\n' "$2" > "$TMPFILE"
    RESULT=$(timeout 5 "$TARDY" verify-doc "$TMPFILE" 2>/dev/null) || true
    CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT" || true)
    if [ "$CONFLICTS" -gt 0 ]; then
      echo "[targy] INPUT has $CONFLICTS contradiction(s) with session history:"
      echo "$RESULT" | grep "CONFLICT\|->.*conflict"
    fi
    "$TARDY" remember "$WING" -- "$2" >/dev/null 2>&1 || true
    rm -f "$TMPFILE"
    ;;
  output)
    TMPFILE=$(mktemp "$TMP/output.XXXXXX.md")
    printf '%s\n' "$2" > "$TMPFILE"
    RESULT=$(timeout 5 "$TARDY" verify-doc "$TMPFILE" 2>/dev/null) || true
    CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT" || true)
    if [ "$CONFLICTS" -gt 0 ]; then
      echo "[targy] OUTPUT has $CONFLICTS internal contradiction(s):"
      echo "$RESULT" | grep "CONFLICT\|->.*conflict"
    fi
    "$TARDY" remember "$WING" -- "$2" >/dev/null 2>&1 || true
    rm -f "$TMPFILE"
    ;;
  status)
    "$TARDY" recall "$WING" 2>/dev/null
    ;;
  *)
    echo "Usage: targy-monitor.sh {input|output|status} [text]" >&2
    exit 1
    ;;
esac
