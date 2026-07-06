#!/usr/bin/env python3
"""stdio framing shim: newline-delimited JSON  <->  Content-Length (LSP) framing.

`tardy mcp-bridge` speaks LSP-style Content-Length framing, but the MCP stdio
spec (and every standard client — Continue, Claude Code) uses newline-delimited
JSON. Without translation, Continue sends a newline-framed initialize, tardy
never sees a complete message, and its tools silently never load.

This process is what Continue launches. It:
  - reads newline-delimited JSON from Continue (our stdin)  -> writes Content-Length frames to tardy
  - reads Content-Length frames from tardy (its stdout)     -> writes newline-delimited JSON to Continue

Pure stdlib. Exits when either side closes.
"""
import os
import subprocess
import sys
import threading

TARDY = os.environ.get(
    "TARDY_BIN", os.path.expanduser("~/qwen-skills/vendor/tardygrada/tardygrada")
)


def newline_to_framed(src, dst):
    """Continue -> tardy: each newline-delimited JSON line becomes a framed message."""
    for line in src:
        line = line.strip()
        if not line:
            continue
        body = line.encode("utf-8")
        dst.write(f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8") + body)
        dst.flush()
    try:
        dst.close()
    except Exception:
        pass


def framed_to_newline(src, dst):
    """tardy -> Continue: parse Content-Length frames, emit one JSON per line."""
    buf = b""
    while True:
        # read headers up to the blank line
        while b"\r\n\r\n" not in buf:
            chunk = src.read(1)
            if not chunk:
                return
            buf += chunk
        header, _, rest = buf.partition(b"\r\n\r\n")
        length = 0
        for h in header.split(b"\r\n"):
            if h.lower().startswith(b"content-length:"):
                length = int(h.split(b":", 1)[1].strip())
        while len(rest) < length:
            chunk = src.read(length - len(rest))
            if not chunk:
                return
            rest += chunk
        body, buf = rest[:length], rest[length:]
        dst.write(body.decode("utf-8", "replace") + "\n")
        dst.flush()


def main():
    proc = subprocess.Popen(
        [TARDY, "mcp-bridge"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,  # let tardy's diagnostics flow to the MCP log
    )
    t_in = threading.Thread(
        target=newline_to_framed, args=(sys.stdin, proc.stdin), daemon=True
    )
    t_out = threading.Thread(
        target=framed_to_newline, args=(proc.stdout, sys.stdout), daemon=True
    )
    t_in.start()
    t_out.start()
    proc.wait()


if __name__ == "__main__":
    main()
