#!/usr/bin/env python3
"""
Promotion ladder: @verified -> @sovereign after K independent confirmations.

When a fact is freshly accepted via demo/learn-validated.py it lands in
tests/learned_ontology.nt. That file represents the @verified tier:
the fact passed the K-of-N validator gate at SUBMIT time, but it has
not been independently re-confirmed since. This script does the
periodic re-confirmation pass that promotes facts to @sovereign.

Architecture:

    tests/wikidata_common.nt    — bundled corpus, treated as @sovereign
                                   on load (curated, hand-vetted)
    tests/learned_ontology.nt   — @verified tier (passed initial K-of-N
                                   gate; persisted, durable)
    tests/sovereign_ontology.nt — @sovereign tier (reconfirmed K_PROMOTE
                                   times AFTER initial acceptance)
    tests/promote_state.json    — per-fact confirmation counters

The daemon already loads both wikidata_common.nt and learned_ontology.nt
on startup. This script adds a new tier by also writing accepted-and-
re-confirmed facts to tests/sovereign_ontology.nt — currently a separate
file that documents the gradation. (Loading sovereign_ontology.nt at
daemon start is a separate change; right now the file exists as
out-of-band durable state.)

Procedure each run:

  for each fact in learned_ontology.nt:
      run cross-validation (same K-of-N gate as the initial accept)
      if all validators say YES:
          increment counter[fact]
          if counter[fact] >= K_PROMOTE:
              move fact from learned -> sovereign
              remove from counter
      else:
          this is a real signal — add to rejection memory and remove
          from learned (the original validation may have been wrong,
          or facts can rot if the world changes)

Use:
    MOCK_VALIDATOR=1 ./demo/promote.py
    K_PROMOTE=3 ./demo/promote.py        # need 3 independent confirmations
    DRY_RUN=1 ./demo/promote.py          # report what would happen, no writes

This is meant to run periodically (e.g. weekly cron, or after every
significant learning batch). It cannot run alongside an active
learn-validated.py session because both want to write learned_ontology.
"""
import json
import os
import sys
import time
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LEARNED   = os.path.join(ROOT, "tests", "learned_ontology.nt")
SOVEREIGN = os.path.join(ROOT, "tests", "sovereign_ontology.nt")
STATE     = os.path.join(ROOT, "tests", "promote_state.json")
REJECTED_LOG = os.path.join(ROOT, "tests", "rejected_log.jsonl")

# Reuse the validator from learn-validated. Keeping it inline so this
# script stays self-contained even if learn-validated changes.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

GREEN  = "\033[0;32m"
YELLOW = "\033[1;33m"
RED    = "\033[0;31m"
DIM    = "\033[2m"
CYAN   = "\033[0;36m"
NC     = "\033[0m"


# ----------------------------------------------------------------------
# Mock oracle (same set as learn-validated). Replace with real Anthropic
# call by setting ANTHROPIC_API_KEY; falls back to interactive otherwise.
# ----------------------------------------------------------------------

MOCK_KNOWN_WRONG = {
    ("SamAltman", "founder", "Anthropic"),
    ("MarkZuckerberg", "founder", "Twitter"),
    ("ElonMusk", "founder", "Microsoft"),
    ("BillGates", "founder", "Apple"),
    ("LarryPage", "founder", "Facebook"),
}


def validate_mock(subj, pred, obj):
    return "NO" if (subj, pred, obj) in MOCK_KNOWN_WRONG else "YES"


def validate_with_anthropic(subj, pred, obj):
    import subprocess
    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        return "ERR"
    body = {
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 8, "temperature": 0,
        "system": ("You evaluate whether a (subject, predicate, object) "
                   "triple is factually correct. Answer with exactly one "
                   "word: YES or NO. If unsure, NO. Do not explain."),
        "messages": [{"role": "user",
                      "content": f"Triple: {subj} {pred} {obj}\nAnswer:"}],
    }
    try:
        out = subprocess.check_output([
            "curl", "-s", "-X", "POST",
            "https://api.anthropic.com/v1/messages",
            "-H", "Content-Type: application/json",
            "-H", "anthropic-version: 2023-06-01",
            "-H", f"x-api-key: {api_key}",
            "-d", json.dumps(body),
        ], timeout=20)
        resp = json.loads(out.decode())
        text = (resp.get("content", [{}])[0].get("text", "") or "").strip().upper()
        if text.startswith("YES"): return "YES"
        if text.startswith("NO"):  return "NO"
        return "ERR"
    except Exception:
        return "ERR"


def validate(subj, pred, obj):
    if os.environ.get("MOCK_VALIDATOR") == "1":
        return validate_mock(subj, pred, obj)
    if os.environ.get("ANTHROPIC_API_KEY"):
        v = validate_with_anthropic(subj, pred, obj)
        if v in ("YES", "NO"):
            return v
    # Interactive fallback
    while True:
        ans = input(f"  re-validate {subj} {pred} {obj}? [y/n]: ").strip().lower()
        if ans in ("y", "yes"): return "YES"
        if ans in ("n", "no"):  return "NO"


# ----------------------------------------------------------------------
# N-Triples I/O
# ----------------------------------------------------------------------

NT_RE = re.compile(
    r'^<[^>]*org/(?P<s>[^>]+)>\s+<[^>]*org/(?P<p>[^>]+)>\s+'
    r'<[^>]*org/(?P<o>[^>]+)>\s*\.\s*$'
)


def parse_nt_lines(path):
    if not os.path.exists(path):
        return []
    out = []
    with open(path) as f:
        for line in f:
            m = NT_RE.match(line.strip())
            if m:
                out.append((m.group("s"), m.group("p"), m.group("o")))
    return out


def write_nt_lines(path, triples):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        for (s, p, o) in triples:
            f.write(
                f"<http://tardygrada.org/{s}> "
                f"<http://schema.org/{p}> "
                f"<http://tardygrada.org/{o}> .\n"
            )


def append_nt_lines(path, triples):
    if not triples:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a") as f:
        for (s, p, o) in triples:
            f.write(
                f"<http://tardygrada.org/{s}> "
                f"<http://schema.org/{p}> "
                f"<http://tardygrada.org/{o}> .\n"
            )


def load_state():
    if not os.path.exists(STATE):
        return {}
    try:
        with open(STATE) as f:
            return json.load(f)
    except Exception:
        return {}


def save_state(state):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)


def fact_key(s, p, o):
    return f"{s}|{p}|{o}"


def remember_rejection(subj, pred, obj, reason):
    rec = {
        "subject": subj, "predicate": pred, "object": obj,
        "reason": reason, "ts": int(time.time()),
        "source": "promote.py",
    }
    os.makedirs(os.path.dirname(REJECTED_LOG), exist_ok=True)
    with open(REJECTED_LOG, "a") as f:
        f.write(json.dumps(rec) + "\n")


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

def main():
    K_PROMOTE = int(os.environ.get("K_PROMOTE", "2"))
    DRY_RUN = os.environ.get("DRY_RUN") == "1"

    learned = parse_nt_lines(LEARNED)
    sovereign = set(parse_nt_lines(SOVEREIGN))
    state = load_state()

    if not learned:
        print(f"{DIM}{LEARNED} is empty — nothing to promote.{NC}")
        return 0

    backend = ("mock-oracle" if os.environ.get("MOCK_VALIDATOR") == "1"
               else "Anthropic API" if os.environ.get("ANTHROPIC_API_KEY")
               else "interactive")
    print(f"{DIM}Promotion pass: K_PROMOTE={K_PROMOTE}, "
          f"backend={backend}, dry_run={DRY_RUN}{NC}")
    print(f"{DIM}{LEARNED}: {len(learned)} candidates{NC}")
    print(f"{DIM}{SOVEREIGN}: {len(sovereign)} already promoted{NC}\n")

    new_learned = []
    promoted = []
    rejected = []

    for (s, p, o) in learned:
        # Skip facts already in sovereign tier (defensive — shouldn't
        # normally happen, but if state was edited externally, we
        # tolerate the duplicate).
        if (s, p, o) in sovereign:
            promoted.append((s, p, o))
            continue
        v = validate(s, p, o)
        key = fact_key(s, p, o)
        if v == "YES":
            n = state.get(key, 0) + 1
            state[key] = n
            if n >= K_PROMOTE:
                promoted.append((s, p, o))
                state.pop(key, None)
                print(f"  {GREEN}↑ promoted{NC}   {s} {p} {o} "
                      f"{DIM}[reconfirmed {n}/{K_PROMOTE}]{NC}")
            else:
                new_learned.append((s, p, o))
                print(f"  {DIM}· counted{NC}    {s} {p} {o} "
                      f"{DIM}[{n}/{K_PROMOTE}]{NC}")
        else:
            # Validator now says NO. Original acceptance was wrong, or
            # the fact has rotted. Either way, demote: remove from
            # learned and add to rejection memory.
            rejected.append((s, p, o))
            state.pop(key, None)
            print(f"  {RED}✗ demoted{NC}    {s} {p} {o} "
                  f"{DIM}[validator now says NO]{NC}")

    if not DRY_RUN:
        # Rewrite learned: only facts still under-confirmation
        write_nt_lines(LEARNED, new_learned)
        # Append promoted to sovereign
        already = set((s, p, o) for (s, p, o) in parse_nt_lines(SOVEREIGN))
        novel = [t for t in promoted if t not in already]
        append_nt_lines(SOVEREIGN, novel)
        # Persist counter state
        save_state(state)
        # Log demotions
        for (s, p, o) in rejected:
            remember_rejection(s, p, o, reason="re-validation said NO")

    print(f"\n{DIM}=== Promotion summary ==={NC}")
    print(f"  {GREEN}promoted{NC}    {len([t for t in promoted if t in (set(promoted) - sovereign)])}")
    print(f"  {DIM}counted{NC}     {len(new_learned)} (still under K_PROMOTE)")
    print(f"  {RED}demoted{NC}     {len(rejected)}")
    print(f"  total       {len(learned)}")
    if DRY_RUN:
        print(f"  {YELLOW}dry-run — no files written{NC}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
