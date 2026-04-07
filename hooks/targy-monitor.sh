#!/usr/bin/env bash
# Tardygrada monitor hook for Claude Code
TARDY="${TARDY_BIN:-tardygrada}"
WING="claude-session"
TMP="/tmp/targy-monitor"
mkdir -p "$TMP"

case "$1" in
  input)
    echo "$2" > "$TMP/input.md"
    RESULT=$($TARDY verify-doc "$TMP/input.md" 2>/dev/null)
    CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT")
    if [ "$CONFLICTS" -gt 0 ]; then
      echo "[targy] INPUT has $CONFLICTS contradiction(s) with session history:"
      echo "$RESULT" | grep "CONFLICT\|->.*conflict"
    fi
    $TARDY remember "$WING" "$2" >/dev/null 2>&1
    ;;
  output)
    echo "$2" > "$TMP/output.md"
    RESULT=$($TARDY verify-doc "$TMP/output.md" 2>/dev/null)
    CONFLICTS=$(echo "$RESULT" | grep -c "CONFLICT")
    if [ "$CONFLICTS" -gt 0 ]; then
      echo "[targy] OUTPUT has $CONFLICTS internal contradiction(s):"
      echo "$RESULT" | grep "CONFLICT\|->.*conflict"
    fi
    $TARDY remember "$WING" "$2" >/dev/null 2>&1
    ;;
  status)
    $TARDY recall "$WING" 2>/dev/null
    ;;
esac
