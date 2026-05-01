#!/usr/bin/env python3
"""
Wikidata ingestor: pull facts from query.wikidata.org and submit them
into the running tardygrada daemon via the submit-fact path.

Built so the user can scale the ontology beyond ~180 hand-curated facts
toward 10K+ without writing N-Triples by hand. Each ingested fact lands
in the LEARNED tier (per Phase A), so the existing promotion ladder
(demo/promote.py) can lift it to SOVEREIGN after K_PROMOTE
re-confirmations. Wikidata is itself curated, but we still let it pass
through the validation pipeline rather than trusting it on faith — this
keeps the trust gradient honest and lets the K-of-N validator catch
genuine Wikidata errors.

Architecture:

    Wikidata SPARQL  ──►  preset query  ──►  (s, p, o) triples
                                                    │
                                                    ▼
                                          daemon submit-fact
                                                    │
                                                    ▼
                                  LEARNED tier  (tests/learned_ontology.nt)

Use:
    ./demo/wikidata-ingest.py capitals          # ~200 capital cities
    ./demo/wikidata-ingest.py founders --limit 500
    ./demo/wikidata-ingest.py authors --limit 1000
    ./demo/wikidata-ingest.py --list            # show all presets
    ./demo/wikidata-ingest.py --dry-run capitals

The daemon must be running (./tardygrada daemon start). Ingestion is
incremental — duplicates and derivable facts are reported but skipped,
so re-running is safe.
"""
import argparse
import json
import os
import re
import socket
import ssl
import sys
import time
import urllib.parse
import urllib.request

SOCK = "/tmp/tardygrada.sock"
USER_AGENT = "tardygrada-wikidata-ingest/1.0 (https://github.com/fabio-rovai)"
SPARQL_ENDPOINT = "https://query.wikidata.org/sparql"

GREEN  = "\033[0;32m"
YELLOW = "\033[1;33m"
RED    = "\033[0;31m"
DIM    = "\033[2m"
CYAN   = "\033[0;36m"
NC     = "\033[0m"


# ----------------------------------------------------------------------
# Preset SPARQL queries.
# Each preset returns a list of (s_label, p, o_label) tuples to ingest.
# We always project ?subjLabel/?objLabel via the wikibase label service
# so we get clean English names rather than Q-numbers.
# ----------------------------------------------------------------------

PRESETS = {
    "capitals": {
        "predicate": "capitalOf",
        "description": "city is capital of country (wdt:P1376)",
        "sparql": """
            SELECT DISTINCT ?subjLabel ?objLabel WHERE {
              ?subj wdt:P1376 ?obj .
              ?subj rdfs:label ?subjLabel .
              ?obj  rdfs:label ?objLabel .
              FILTER(LANG(?subjLabel) = "en")
              FILTER(LANG(?objLabel)  = "en")
            }
            LIMIT %(limit)d
        """,
    },
    "founders": {
        "predicate": "founder",
        "description": "company founded by person (wdt:P112)",
        "sparql": """
            SELECT DISTINCT ?subjLabel ?objLabel WHERE {
              ?subj wdt:P112 ?obj .
              ?subj wdt:P31/wdt:P279* wd:Q4830453 .  # business
              ?subj rdfs:label ?subjLabel .
              ?obj  rdfs:label ?objLabel .
              FILTER(LANG(?subjLabel) = "en")
              FILTER(LANG(?objLabel)  = "en")
            }
            LIMIT %(limit)d
        """,
    },
    "authors": {
        "predicate": "author",
        "description": "book written by person (wdt:P50)",
        "sparql": """
            SELECT DISTINCT ?subjLabel ?objLabel WHERE {
              ?subj wdt:P50 ?obj .
              ?subj wdt:P31/wdt:P279* wd:Q571 .  # book
              ?subj rdfs:label ?subjLabel .
              ?obj  rdfs:label ?objLabel .
              FILTER(LANG(?subjLabel) = "en")
              FILTER(LANG(?objLabel)  = "en")
            }
            LIMIT %(limit)d
        """,
    },
    "composers": {
        "predicate": "composer",
        "description": "musical work composed by person (wdt:P86)",
        "sparql": """
            SELECT DISTINCT ?subjLabel ?objLabel WHERE {
              ?subj wdt:P86 ?obj .
              ?subj rdfs:label ?subjLabel .
              ?obj  rdfs:label ?objLabel .
              FILTER(LANG(?subjLabel) = "en")
              FILTER(LANG(?objLabel)  = "en")
            }
            LIMIT %(limit)d
        """,
    },
    "countries": {
        "predicate": "locatedIn",
        "description": "city located in country (wdt:P17, instance of city)",
        "sparql": """
            SELECT DISTINCT ?subjLabel ?objLabel WHERE {
              ?subj wdt:P31/wdt:P279* wd:Q515 .   # city
              ?subj wdt:P17 ?obj .
              ?subj rdfs:label ?subjLabel .
              ?obj  rdfs:label ?objLabel .
              FILTER(LANG(?subjLabel) = "en")
              FILTER(LANG(?objLabel)  = "en")
            }
            LIMIT %(limit)d
        """,
    },
}


# ----------------------------------------------------------------------
# Label normalisation. The daemon stores facts under CamelCase local
# names ("UnitedStates", "EiffelTower"). Wikidata labels arrive as
# free-text English ("United States", "Eiffel Tower"). Strip
# punctuation, collapse whitespace, capitalise each word, drop spaces.
# Reject labels that, after cleaning, are empty or contain non-ASCII —
# we keep the bundled corpus ASCII-only for now to avoid IRI escaping
# headaches.
# ----------------------------------------------------------------------

LABEL_OK = re.compile(r"^[A-Za-z0-9]+$")

def to_local_name(label):
    if not label:
        return None
    # Reject labels with non-ASCII letters rather than silently dropping
    # diacritics ("Brač" → "Bra", "Baden-Württemberg" → "BadenWrttemberg").
    # The bundled corpus is ASCII-only; we'd rather skip a fact than
    # store a mangled name that won't match queries downstream.
    if not all(ord(c) < 128 for c in label):
        return None
    s = re.sub(r"[^A-Za-z0-9 \-_]", "", label).strip()
    if not s:
        return None
    parts = [p for p in re.split(r"[\s\-_]+", s) if p]
    if not parts:
        return None
    name = "".join(p[:1].upper() + p[1:] for p in parts)
    return name if LABEL_OK.match(name) else None


# ----------------------------------------------------------------------
# Wikidata SPARQL fetch.
# ----------------------------------------------------------------------

def fetch_sparql(query, timeout=60):
    url = SPARQL_ENDPOINT + "?" + urllib.parse.urlencode({
        "query": query, "format": "json"
    })
    req = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT,
        "Accept": "application/sparql-results+json",
    })
    # macOS Python from python.org commonly lacks system CA roots
    # (the "Install Certificates.command" never ran). Try the default
    # verified context first; fall back to an unverified context with a
    # clear warning so the demo still works on a fresh laptop.
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.URLError as e:
        if not isinstance(getattr(e, "reason", None), ssl.SSLError):
            raise
        print(f"{YELLOW}SSL verify failed; retrying without "
              f"verification.{NC}", file=sys.stderr)
        print(f"{DIM}Install certs properly via:  "
              f"/Applications/Python\\ 3*/Install\\ Certificates.command"
              f"{NC}", file=sys.stderr)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
            return json.loads(r.read().decode())


def extract_triples(sparql_json, predicate):
    out = []
    for binding in sparql_json.get("results", {}).get("bindings", []):
        s_raw = binding.get("subjLabel", {}).get("value", "")
        o_raw = binding.get("objLabel",  {}).get("value", "")
        s = to_local_name(s_raw)
        o = to_local_name(o_raw)
        if s and o and s != o:
            out.append((s, predicate, o, s_raw, o_raw))
    return out


# ----------------------------------------------------------------------
# Daemon submit-fact via Unix socket. Same protocol as demo/learn-mcp.py
# but skipping the MCP framing — straight JSON over the daemon socket.
# ----------------------------------------------------------------------

def submit_fact(s, p, o):
    sk = socket.socket(socket.AF_UNIX)
    sk.connect(SOCK)
    msg = json.dumps({
        "cmd": "submit-fact",
        "subject": s, "predicate": p, "object": o,
    }) + "\n"
    sk.sendall(msg.encode())
    sk.shutdown(socket.SHUT_WR)
    buf = b""
    while True:
        chunk = sk.recv(4096)
        if not chunk:
            break
        buf += chunk
    sk.close()
    try:
        return json.loads(buf.decode().strip())
    except Exception:
        return {"ok": False, "status": "parse_error", "raw": buf.decode()}


def chip(status):
    if status == "accepted":   return f"{GREEN}✓ accepted {NC}"
    if status == "duplicate":  return f"{DIM}· duplicate{NC}"
    if status == "derived":    return f"{DIM}· derived  {NC}"
    if status == "conflict":   return f"{RED}✗ conflict {NC}"
    return f"{YELLOW}? {status:9s}{NC}"


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("preset", nargs="?",
        help="preset name (capitals, founders, authors, composers, countries)")
    ap.add_argument("--limit", type=int, default=200,
        help="max triples to fetch from SPARQL (default 200)")
    ap.add_argument("--dry-run", action="store_true",
        help="fetch and print, do not submit")
    ap.add_argument("--list", action="store_true",
        help="list available presets and exit")
    ap.add_argument("--throttle", type=float, default=0.0,
        help="seconds to sleep between submits (default 0)")
    args = ap.parse_args()

    if args.list:
        for name, p in PRESETS.items():
            print(f"  {CYAN}{name:12s}{NC} {p['description']}")
        return 0

    if not args.preset:
        ap.print_help()
        return 2

    if args.preset not in PRESETS:
        print(f"{RED}unknown preset:{NC} {args.preset}", file=sys.stderr)
        print(f"available: {', '.join(PRESETS.keys())}", file=sys.stderr)
        return 2

    preset = PRESETS[args.preset]
    query = preset["sparql"] % {"limit": args.limit}
    pred  = preset["predicate"]

    print(f"{DIM}Wikidata SPARQL: {preset['description']}{NC}")
    print(f"{DIM}Predicate:       {pred}{NC}")
    print(f"{DIM}Limit:           {args.limit}{NC}")
    print(f"{DIM}Endpoint:        {SPARQL_ENDPOINT}{NC}\n")

    t0 = time.time()
    try:
        result = fetch_sparql(query)
    except Exception as e:
        print(f"{RED}SPARQL fetch failed:{NC} {e}", file=sys.stderr)
        return 1
    triples = extract_triples(result, pred)
    print(f"{DIM}Fetched {len(triples)} usable triples in "
          f"{time.time() - t0:.1f}s{NC}\n")

    if args.dry_run:
        for s, p, o, s_raw, o_raw in triples[:50]:
            print(f"  {DIM}{s_raw} → {s}, {o_raw} → {o}{NC}")
        if len(triples) > 50:
            print(f"  {DIM}... and {len(triples) - 50} more{NC}")
        return 0

    # Confirm daemon is up.
    if not os.path.exists(SOCK):
        print(f"{RED}daemon socket not found:{NC} {SOCK}", file=sys.stderr)
        print("start it with: ./tardygrada daemon start", file=sys.stderr)
        return 1

    counts = {"accepted": 0, "duplicate": 0, "derived": 0,
              "conflict": 0, "other": 0}
    for i, (s, p, o, s_raw, o_raw) in enumerate(triples, 1):
        try:
            resp = submit_fact(s, p, o)
        except Exception as e:
            print(f"  {RED}submit error:{NC} {e}", file=sys.stderr)
            counts["other"] += 1
            continue
        status = resp.get("status", "other")
        counts[status] = counts.get(status, 0) + 1
        if status not in counts:
            counts["other"] += 1
        if i <= 20 or status == "conflict":
            print(f"  {chip(status)} {s} {p} {o}")
        if args.throttle:
            time.sleep(args.throttle)

    if len(triples) > 20:
        print(f"  {DIM}... ({len(triples) - 20} more, see counts below){NC}")

    print(f"\n{DIM}=== Ingestion summary ==={NC}")
    print(f"  {GREEN}accepted{NC}   {counts['accepted']}")
    print(f"  {DIM}duplicate{NC}  {counts['duplicate']}")
    print(f"  {DIM}derived{NC}    {counts['derived']}")
    print(f"  {RED}conflict{NC}   {counts['conflict']}")
    if counts["other"]:
        print(f"  {YELLOW}other{NC}      {counts['other']}")
    print(f"  total      {sum(counts.values())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
