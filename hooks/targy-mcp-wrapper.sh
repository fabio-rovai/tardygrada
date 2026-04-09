#!/usr/bin/env bash
# Wrapper: adapts between newline-delimited JSON (Qwen Code) and
# Content-Length framed JSON-RPC (tardygrada mcp-bridge)
#
# Qwen Code sends: {"jsonrpc":"2.0",...}\n
# tardygrada expects: Content-Length: N\r\n\r\n{"jsonrpc":"2.0",...}
# tardygrada returns: Content-Length: N\r\n\r\n{"jsonrpc":"2.0",...}
# Qwen Code expects: {"jsonrpc":"2.0",...}\n

TARDY="/Users/fabio/projects/tardygrada/tardygrada"

# Ensure daemon is running
"$TARDY" daemon status >/dev/null 2>&1 || "$TARDY" daemon start >/dev/null 2>&1
for i in 1 2 3 4 5; do
  "$TARDY" daemon status >/dev/null 2>&1 && break
  sleep 0.3
done

# Use named pipes for bidirectional translation
FIFO_IN=$(mktemp -u /tmp/targy-mcp-in.XXXXXX)
FIFO_OUT=$(mktemp -u /tmp/targy-mcp-out.XXXXXX)
mkfifo "$FIFO_IN" "$FIFO_OUT"
trap 'rm -f "$FIFO_IN" "$FIFO_OUT"; kill $(jobs -p) 2>/dev/null' EXIT

# Start the bridge with pipes
"$TARDY" mcp-bridge < "$FIFO_IN" > "$FIFO_OUT" &
BRIDGE_PID=$!

# Background: read bridge output (Content-Length framed) → stdout (newline JSON)
(
  while IFS= read -r header; do
    # Skip empty lines and Content-Length header
    header=$(echo "$header" | tr -d '\r')
    if [[ "$header" == Content-Length:* ]]; then
      len="${header#Content-Length: }"
      # Read the blank line
      IFS= read -r blank
      # Read exactly len bytes
      body=$(dd bs=1 count="$len" 2>/dev/null)
      printf '%s\n' "$body"
    fi
  done < "$FIFO_OUT"
) &

# Foreground: read stdin (newline JSON) → bridge input (Content-Length framed)
while IFS= read -r line; do
  [ -z "$line" ] && continue
  len=${#line}
  printf 'Content-Length: %d\r\n\r\n%s' "$len" "$line"
done > "$FIFO_IN"

wait "$BRIDGE_PID" 2>/dev/null
