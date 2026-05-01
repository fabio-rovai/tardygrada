#!/usr/bin/env python3
"""
Tardygrada learn-loop, driven through MCP.

Same flow as demo/learn-loop.sh, but using the standard MCP transport
(JSON-RPC 2.0 over stdio with Content-Length framing) instead of the
daemon's Unix socket. This is how Claude Code, Cursor, Qwen, or any
other MCP-aware agent would drive ontology growth.

Reads candidate triples from a file (one (subject, predicate, object)
per line, comma-separated) or from stdin. For each candidate it calls
the `submit_fact` MCP tool against `tardygrada mcp-bridge`. The
verifier dry-merges into the live ontology and returns one of:

    accepted   — new, no conflict; written to learned_ontology.nt
                 and added to live Datalog
    duplicate  — already known
    derived    — already derivable from existing rules
    conflict   — violates a functional dependency (rejected)

Usage:
    ./demo/learn-mcp.py path/to/candidates.csv
    ./demo/learn-mcp.py -                 # read from stdin
    ./demo/learn-mcp.py --inline          # use bundled sample batch

Each input line: subject,predicate,object   (comments start with #)

Examples:
    Bern,capitalOf,Switzerland
    Amsterdam,capitalOf,Netherlands
    Anthropic,founder,DarioAmodei      # already in ontology -> duplicate
    Bratislava,capitalOf,Slovakia      # already added in v2.0.12 -> duplicate
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tardygrada")

INLINE_SAMPLE = """
# Sample batch — exercises every outcome class.
# Predicate is the schema.org local name (capitalOf, creator, founder, ...).
Berlin,capitalOf,Germany
Lyon,capitalOf,France
Toronto,capitalOf,Canada
Edinburgh,capitalOf,Scotland
Bonn,capitalOf,Germany
Auckland,capitalOf,NewZealand
Liverpool,location,UnitedKingdom
Cambridge,location,UnitedKingdom
"""


# ----------------------------------------------------------------------
# Minimal MCP client: spawn `tardygrada mcp-bridge`, speak JSON-RPC 2.0
# with Content-Length framing on stdio.
# ----------------------------------------------------------------------

class MCPClient:
    def __init__(self, cmd):
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self._next_id = 1

    def _send(self, body):
        msg = json.dumps(body).encode()
        framed = b"Content-Length: %d\r\n\r\n%s" % (len(msg), msg)
        self.proc.stdin.write(framed)
        self.proc.stdin.flush()

    def _recv(self):
        # Read headers until \r\n\r\n
        hdr = b""
        while True:
            ch = self.proc.stdout.read(1)
            if not ch:
                raise RuntimeError("MCP server closed unexpectedly")
            hdr += ch
            if hdr.endswith(b"\r\n\r\n"):
                break
        n = 0
        for line in hdr.decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                n = int(line.split(":", 1)[1].strip())
        body = self.proc.stdout.read(n)
        return json.loads(body.decode())

    def call(self, method, params):
        rid = self._next_id
        self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": rid,
                    "method": method, "params": params})
        # Loop until we see our response
        while True:
            resp = self._recv()
            if resp.get("id") == rid:
                return resp

    def call_tool(self, name, args):
        return self.call("tools/call",
                         {"name": name, "arguments": args})

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


# ----------------------------------------------------------------------
# Loop driver
# ----------------------------------------------------------------------

GREEN = "\033[0;32m"
YELLOW = "\033[1;33m"
RED = "\033[0;31m"
DIM = "\033[2m"
NC = "\033[0m"


def parse_candidates(text):
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 3:
            print(f"{YELLOW}skip malformed:{NC} {raw!r}", file=sys.stderr)
            continue
        out.append(tuple(parts))
    return out


def status_to_chip(status):
    if status == "accepted":
        return f"{GREEN}✓ accepted {NC}"
    if status == "duplicate":
        return f"{DIM}· duplicate{NC}"
    if status == "derived":
        return f"{DIM}· derived  {NC}"
    if status == "conflict":
        return f"{RED}✗ conflict {NC}"
    return f"{YELLOW}? {status:9s}{NC}"


def extract_status(tool_response):
    """The submit_fact tool returns the daemon's JSON wrapped inside
    the MCP tool result envelope. Walk both layers."""
    if "error" in tool_response:
        return "tool_error", tool_response["error"].get("message", "?")
    res = tool_response.get("result", {})
    content = res.get("content", [])
    if content and isinstance(content[0], dict):
        text = content[0].get("text", "")
        try:
            inner = json.loads(text)
            return inner.get("status", "?"), inner
        except Exception:
            return "parse_error", text
    return "no_content", res


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    if argv[1] == "--inline":
        text = INLINE_SAMPLE
    elif argv[1] == "-":
        text = sys.stdin.read()
    else:
        with open(argv[1]) as f:
            text = f.read()

    candidates = parse_candidates(text)
    if not candidates:
        print(f"{RED}no candidates parsed{NC}", file=sys.stderr)
        return 1

    if not os.path.exists(BIN):
        print(f"{RED}{BIN} not built. Run 'make' first.{NC}", file=sys.stderr)
        return 1
    if not os.path.exists("/tmp/tardygrada.sock"):
        print(f"{RED}daemon not running. Start it: ./tardygrada daemon start{NC}",
              file=sys.stderr)
        return 1

    print(f"{DIM}Spawning MCP bridge...{NC}")
    cli = MCPClient([BIN, "mcp-bridge"])

    # Initialize handshake
    init_resp = cli.call("initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "tardygrada-learn-mcp", "version": "0.1"},
    })
    server = init_resp.get("result", {}).get("serverInfo", {})
    print(f"{DIM}Connected to MCP server: "
          f"{server.get('name', '?')} {server.get('version', '?')}{NC}\n")

    # Optional: list tools so we surface submit_fact availability
    tools_resp = cli.call("tools/list", {})
    tools = [t["name"] for t in tools_resp.get("result", {}).get("tools", [])]
    if "submit_fact" not in tools:
        print(f"{RED}submit_fact tool not advertised. Got: {tools}{NC}",
              file=sys.stderr)
        cli.close()
        return 2

    counts = {"accepted": 0, "duplicate": 0, "derived": 0,
              "conflict": 0, "other": 0}

    print(f"{DIM}Submitting {len(candidates)} candidates "
          f"via MCP submit_fact...{NC}\n")

    t0 = time.monotonic()
    for (subj, pred, obj) in candidates:
        resp = cli.call_tool("submit_fact", {
            "subject": subj, "predicate": pred, "object": obj,
        })
        status, _detail = extract_status(resp)
        counts[status] = counts.get(status, 0) + 1 \
            if status in counts else counts["other"] + 1
        if status not in counts:
            counts["other"] += 1
        chip = status_to_chip(status)
        print(f"  {chip} {subj} {pred} {obj}")

    elapsed_ms = int((time.monotonic() - t0) * 1000)

    print()
    print(f"{DIM}=== MCP learn-loop summary ==={NC}")
    print(f"  {GREEN}accepted{NC}   {counts['accepted']}")
    print(f"  duplicate  {counts['duplicate']}")
    print(f"  derived    {counts['derived']}")
    print(f"  {RED}conflict{NC}   {counts['conflict']}")
    print(f"  {YELLOW}other{NC}      {counts['other']}")
    print(f"  total      {len(candidates)} in {elapsed_ms}ms"
          f" ({elapsed_ms/max(1,len(candidates)):.1f}ms/call)")

    cli.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
