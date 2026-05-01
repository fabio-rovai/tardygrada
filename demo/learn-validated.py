#!/usr/bin/env python3
"""
Cross-validated LLM-driven learn-loop.

Builds on demo/learn-mcp.py with one big addition: multi-value predicates
get K-of-N validation BEFORE submission, so the verifier doesn't have to
catch semantic-but-not-structural mistakes (the "Anthropic founder
SamAltman" failure mode from v2.0.13 -> v2.0.14).

Predicate routing:

   FUNCTIONAL         (capitalOf, dateCreated, birthDate, ...)
       │ dry_merge in submit_fact already enforces uniqueness
       └─→ direct submit

   MULTI-VALUE        (founder, location, creator, knownFor, ...)
       │ dry_merge accepts permissively (multiple values are valid)
       │ → K-of-N validator pass first
       │ → only confirmed candidates reach submit_fact
       └─→ validate(N) → if YES_count >= K → submit

   UNKNOWN
       └─→ flagged, skipped, reported for triage

Validator backends (auto-detected, in this preference order):

   1. ANTHROPIC_API_KEY in env: call Claude Haiku via curl with a
      structured Y/N prompt. Cheapest, fastest, most reproducible.
   2. (no key): print the candidate and let the operator confirm
      interactively. Useful for human-in-the-loop bootstrapping.

Output classification:

   accepted    — passed validation + submit_fact returned 'accepted'
   duplicate   — submit_fact returned 'duplicate' (already known)
   derived     — submit_fact returned 'derived' (follows from rules)
   conflict    — submit_fact returned 'conflict' (functional dep)
   unconfirmed — multi-value validation failed (< K positive votes)
   skipped     — predicate not in either bucket; needs human review

Usage:
   ./demo/learn-validated.py path/to/batch.csv
   ./demo/learn-validated.py --inline                 # bundled sample
   K=2 N=2 ./demo/learn-validated.py batch.csv        # tighter
   K=1 N=1 ./demo/learn-validated.py batch.csv        # looser (demo)
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "tardygrada")

# Rejection memory — failed candidates persist here so the same wrong
# claim doesn't get re-validated on every run. JSONL one record per
# rejection, with provenance.
REJECTED_LOG = os.path.join(ROOT, "tests", "rejected_log.jsonl")

# ----------------------------------------------------------------------
# Predicate classification
# ----------------------------------------------------------------------

FUNCTIONAL_PREDS = {
    "capitalOf",
    "dateCreated",
    "birthDate",
    "birthPlace",
    "dateDiscovered",
    "dateInvented",
    "dateBorn",
}

MULTI_VALUE_PREDS = {
    "founder",
    "location",
    "knownFor",
    "creator",
    "inventor",
    "discoverer",
    "person",
    "research",
    "industry",
    "type",
    "origin",
    "locationCreated",
    "writer",
    "composer",
    "director",
    "starredIn",
    "produced",
}


# ----------------------------------------------------------------------
# Validator backend
# ----------------------------------------------------------------------

def validate_with_anthropic(subj, pred, obj, *, model="claude-haiku-4-5-20251001"):
    """Ask Claude (cheapest model) to confirm a triple. Returns "YES",
    "NO", or "ERR" on transport failure."""
    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        return "ERR"
    body = {
        "model": model,
        "max_tokens": 8,
        "temperature": 0,
        "system": (
            "You evaluate whether a (subject, predicate, object) triple is "
            "factually correct. Answer with exactly one word: YES or NO. "
            "If you are unsure, answer NO. Do not explain."
        ),
        "messages": [{"role": "user",
                      "content": f"Triple: {subj} {pred} {obj}\nAnswer:"}],
    }
    cmd = [
        "curl", "-s", "-X", "POST",
        "https://api.anthropic.com/v1/messages",
        "-H", "Content-Type: application/json",
        "-H", "anthropic-version: 2023-06-01",
        "-H", f"x-api-key: {api_key}",
        "-d", json.dumps(body),
    ]
    try:
        out = subprocess.check_output(cmd, timeout=20)
        resp = json.loads(out.decode())
        text = (resp.get("content", [{}])[0].get("text", "") or "").strip().upper()
        # Be tolerant: take the first YES / NO token in the answer
        if text.startswith("YES"):
            return "YES"
        if text.startswith("NO"):
            return "NO"
        return "ERR"
    except Exception:
        return "ERR"


def validate_interactive(subj, pred, obj):
    """Operator-in-the-loop fallback. Asks once, returns YES/NO."""
    while True:
        ans = input(f"  validate {subj} {pred} {obj}? [y/n]: ").strip().lower()
        if ans in ("y", "yes"): return "YES"
        if ans in ("n", "no"):  return "NO"


# Deterministic mock oracle. Used when MOCK_VALIDATOR=1 is set, so the
# demo can run end-to-end without an LLM in the loop. Real production
# never uses this — it's purely a test surface that lets us assert the
# K-of-N gate behaves correctly on a known-wrong fact like
# "SamAltman founder Anthropic" (which an LLM would also reject).
MOCK_KNOWN_WRONG = {
    ("SamAltman", "founder", "Anthropic"),       # Sam founded OpenAI, not Anthropic
    ("MarkZuckerberg", "founder", "Twitter"),     # Zuckerberg founded Facebook
    ("ElonMusk", "founder", "Microsoft"),         # Musk co-founded Tesla/SpaceX/etc., not MS
    ("BillGates", "founder", "Apple"),            # Gates is Microsoft
    ("LarryPage", "founder", "Facebook"),         # Page is Google
}


def validate_mock(subj, pred, obj):
    """Deterministic oracle for the demo. Answers NO for facts in the
    known-wrong set, YES otherwise."""
    if (subj, pred, obj) in MOCK_KNOWN_WRONG:
        return "NO"
    return "YES"


def cross_validate(subj, pred, obj, *, k, n):
    """Run N independent validators. Need at least K positive votes.
    Returns (passed: bool, votes: list[str]).

    Backend selection (in order):
      1. MOCK_VALIDATOR=1                — deterministic mock (demo)
      2. ANTHROPIC_API_KEY in env        — real Claude calls
      3. otherwise                       — interactive prompt
    """
    use_mock = os.environ.get("MOCK_VALIDATOR") == "1"
    have_api = bool(os.environ.get("ANTHROPIC_API_KEY"))
    votes = []
    for _ in range(n):
        if use_mock:
            v = validate_mock(subj, pred, obj)
        elif have_api:
            v = validate_with_anthropic(subj, pred, obj)
            if v == "ERR":
                v = validate_interactive(subj, pred, obj)
        else:
            v = validate_interactive(subj, pred, obj)
        votes.append(v)
    yes = sum(1 for v in votes if v == "YES")
    return yes >= k, votes


# ----------------------------------------------------------------------
# MCP client (same as learn-mcp.py — duplicated to keep this file self-
# contained and easy to read end-to-end)
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
        hdr = b""
        while True:
            ch = self.proc.stdout.read(1)
            if not ch:
                raise RuntimeError("MCP server closed")
            hdr += ch
            if hdr.endswith(b"\r\n\r\n"):
                break
        n = 0
        for line in hdr.decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                n = int(line.split(":", 1)[1].strip())
        return json.loads(self.proc.stdout.read(n).decode())

    def call(self, method, params):
        rid = self._next_id; self._next_id += 1
        self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            resp = self._recv()
            if resp.get("id") == rid: return resp

    def call_tool(self, name, args):
        return self.call("tools/call", {"name": name, "arguments": args})

    def close(self):
        try: self.proc.stdin.close(); self.proc.wait(timeout=2)
        except Exception: self.proc.kill()


# ----------------------------------------------------------------------
# Driver
# ----------------------------------------------------------------------

GREEN  = "\033[0;32m"
YELLOW = "\033[1;33m"
RED    = "\033[0;31m"
DIM    = "\033[2m"
CYAN   = "\033[0;36m"
NC     = "\033[0m"


INLINE_SAMPLE = """
# Mix of functional and multi-value, including some intentional mistakes.
# Functional (caught by dry_merge alone):
Quito,capitalOf,Ecuador
Bratislava,capitalOf,Slovakia
Lyon,capitalOf,France
# Multi-value (need cross-validation):
JeffBezos,founder,Amazon
SteveJobs,founder,Apple
SteveWozniak,founder,Apple
SamAltman,founder,Anthropic
SamAltman,founder,OpenAI
ElonMusk,founder,Tesla
JackDorsey,founder,Twitter
"""


def parse_candidates(text):
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 3:
            continue
        out.append(tuple(parts))
    return out


def load_rejection_memory():
    """Read REJECTED_LOG and return a set of (subj, pred, obj) tuples
    that have previously failed validation. Empty set if the file
    doesn't exist or is malformed."""
    rejected = set()
    if not os.path.exists(REJECTED_LOG):
        return rejected
    try:
        with open(REJECTED_LOG) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    rejected.add((rec["subject"], rec["predicate"],
                                  rec["object"]))
                except Exception:
                    continue
    except Exception:
        pass
    return rejected


def remember_rejection(subj, pred, obj, reason, votes):
    """Append a rejection record to REJECTED_LOG so this triple will
    be skipped on future runs. Records are JSONL so they're easy to
    grep, easy to amend, and survive arbitrary daemon state changes."""
    os.makedirs(os.path.dirname(REJECTED_LOG), exist_ok=True)
    rec = {
        "subject": subj, "predicate": pred, "object": obj,
        "reason": reason, "votes": votes,
        "ts": int(time.time()),
    }
    with open(REJECTED_LOG, "a") as f:
        f.write(json.dumps(rec) + "\n")


def chip(status):
    return {
        "accepted":     f"{GREEN}✓ accepted   {NC}",
        "duplicate":    f"{DIM}· duplicate  {NC}",
        "derived":      f"{DIM}· derived    {NC}",
        "conflict":     f"{RED}✗ conflict   {NC}",
        "unconfirmed":  f"{YELLOW}? unconfirmed{NC}",
        "skipped":      f"{YELLOW}· skipped    {NC}",
    }.get(status, f"{YELLOW}? {status:12s}{NC}")


def submit_via_mcp(cli, subj, pred, obj):
    resp = cli.call_tool("submit_fact",
                         {"subject": subj, "predicate": pred, "object": obj})
    if "error" in resp:
        return "error", resp["error"].get("message", "?")
    res = resp.get("result", {})
    content = res.get("content", [])
    if content and isinstance(content[0], dict):
        try:
            inner = json.loads(content[0].get("text", ""))
            return inner.get("status", "?"), inner
        except Exception:
            return "parse_error", content[0].get("text", "")
    return "no_content", res


def fetch_predicate_classes(cli):
    """Query the daemon's frame registry via the list_frames MCP tool
    and return (functional_set, multi_value_set). Falls back to the
    hardcoded constants if the daemon doesn't expose frames yet (older
    binary). Predicates with object_functional=true are functional
    (one value per subject); the rest of the framed predicates are
    multi-value."""
    try:
        resp = cli.call_tool("list_frames", {})
    except Exception:
        return set(FUNCTIONAL_PREDS), set(MULTI_VALUE_PREDS)
    res = resp.get("result", {})
    content = res.get("content", [])
    if not content or "text" not in content[0]:
        return set(FUNCTIONAL_PREDS), set(MULTI_VALUE_PREDS)
    try:
        inner = json.loads(content[0]["text"])
    except Exception:
        return set(FUNCTIONAL_PREDS), set(MULTI_VALUE_PREDS)

    functional = set()
    multi_value = set()
    for fr in inner.get("frames", []):
        pred = fr.get("predicate")
        if not pred:
            continue
        if fr.get("object_functional"):
            functional.add(pred)
        else:
            multi_value.add(pred)
    # Always merge with the hardcoded sets so we never narrow coverage.
    # The frame registry is the source-of-truth for what IS functional;
    # the hardcoded sets cover predicates the registry doesn't know about.
    functional |= FUNCTIONAL_PREDS
    multi_value = (multi_value | MULTI_VALUE_PREDS) - functional
    return functional, multi_value


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

    K = int(os.environ.get("K", "2"))
    N = int(os.environ.get("N", "2"))
    if K > N:
        print(f"{RED}K ({K}) cannot exceed N ({N}){NC}", file=sys.stderr)
        return 1

    if not os.path.exists(BIN):
        print(f"{RED}{BIN} not built. Run 'make' first.{NC}", file=sys.stderr)
        return 1
    if not os.path.exists("/tmp/tardygrada.sock"):
        print(f"{RED}daemon not running.{NC}", file=sys.stderr)
        return 1

    use_mock = os.environ.get("MOCK_VALIDATOR") == "1"
    have_api = bool(os.environ.get("ANTHROPIC_API_KEY"))
    backend = ("mock-oracle" if use_mock
               else "Anthropic API" if have_api
               else "interactive")
    print(f"{DIM}Cross-validation: K={K}-of-N={N}, backend={backend}{NC}")

    cli = MCPClient([BIN, "mcp-bridge"])
    init_resp = cli.call("initialize", {
        "protocolVersion": "2024-11-05", "capabilities": {},
        "clientInfo": {"name": "tardygrada-learn-validated", "version": "0.1"},
    })
    server = init_resp.get("result", {}).get("serverInfo", {})
    print(f"{DIM}MCP server: {server.get('name', '?')} {server.get('version', '?')}{NC}")

    # Auto-derive predicate classes from the live frame registry.
    functional_preds, multi_value_preds = fetch_predicate_classes(cli)
    print(f"{DIM}Predicate classes: {len(functional_preds)} functional, "
          f"{len(multi_value_preds)} multi-value (auto-derived from frame registry){NC}\n")

    counts = {k: 0 for k in
              ("accepted", "duplicate", "derived", "conflict",
               "unconfirmed", "skipped", "remembered", "other")}

    rejected_memory = load_rejection_memory()
    if rejected_memory:
        print(f"{DIM}Rejection memory: {len(rejected_memory)} prior failures loaded.{NC}")

    print(f"{DIM}Processing {len(candidates)} candidates...{NC}\n")
    t0 = time.monotonic()

    for (subj, pred, obj) in candidates:
        # Skip candidates already known to be wrong from a prior run.
        # Saves an LLM call per skip and amortizes the rejection cost
        # across all future learning batches.
        if (subj, pred, obj) in rejected_memory:
            counts["remembered"] += 1
            print(f"  {chip('remembered')}{CYAN}[??]{NC} {subj} {pred} {obj} "
                  f"{DIM}[in rejection memory]{NC}")
            continue

        if pred in functional_preds:
            # Direct submit; rely on dry_merge for the structural check.
            status, _ = submit_via_mcp(cli, subj, pred, obj)
            counts[status] = counts.get(status, 0) + 1
            print(f"  {chip(status)}{CYAN}[fn]{NC} {subj} {pred} {obj}")

        elif pred in multi_value_preds:
            # K-of-N validation gate.
            passed, votes = cross_validate(subj, pred, obj, k=K, n=N)
            if not passed:
                counts["unconfirmed"] += 1
                yes = sum(1 for v in votes if v == "YES")
                # Persist the rejection so we don't re-pay the validator
                # cost on the same wrong claim in future runs.
                remember_rejection(subj, pred, obj,
                                    reason=f"{yes}/{N} YES (K={K})",
                                    votes=votes)
                print(f"  {chip('unconfirmed')}{CYAN}[mv]{NC} {subj} {pred} {obj} "
                      f"{DIM}[{yes}/{N} YES, votes={','.join(votes)}, remembered]{NC}")
                continue
            status, _ = submit_via_mcp(cli, subj, pred, obj)
            counts[status] = counts.get(status, 0) + 1
            yes = sum(1 for v in votes if v == "YES")
            print(f"  {chip(status)}{CYAN}[mv]{NC} {subj} {pred} {obj} "
                  f"{DIM}[{yes}/{N} YES, then {status}]{NC}")

        else:
            counts["skipped"] += 1
            print(f"  {chip('skipped')}{CYAN}[??]{NC} {subj} {pred} {obj} "
                  f"{DIM}[predicate not classified]{NC}")

    elapsed_ms = int((time.monotonic() - t0) * 1000)
    print(f"\n{DIM}=== Validated learn-loop summary ==={NC}")
    for k in ("accepted", "duplicate", "derived", "conflict",
              "unconfirmed", "remembered", "skipped", "other"):
        v = counts.get(k, 0)
        if v == 0:
            continue
        col = (GREEN if k == "accepted"
               else RED if k == "conflict"
               else YELLOW if k in ("unconfirmed", "skipped", "other")
               else DIM)
        print(f"  {col}{k:12s}{NC} {v}")
    print(f"  {DIM}total{NC}        {len(candidates)} in {elapsed_ms}ms")
    cli.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
