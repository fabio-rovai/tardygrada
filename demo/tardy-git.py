#!/usr/bin/env python3
"""
tardy-git: a verifiable wrapper around the git CLI.

This is the first concrete demo of "tardygrada terraforms software":
a real piece of legacy software (git) wrapped behind the verification
gate so an LLM (or a human) can drive it without footguns.

Architecture:

    user / LLM
        │
        ▼
    tardy-git <subcommand> <args>
        │
        ├─► build a CLAIM about the operation's safety
        │      e.g. "main is a protected_branch"
        ├─► tardygrada daemon: cmd=run, claim=...
        │      └─► VERIFIED  → operation is unsafe, REJECT
        │      └─► ontology_gap → operation is fine, EXECUTE
        ▼
    git <subcommand> <args>          (only if gate passes)

The gate is data-driven: tests/git_ontology.nt lists protected branches,
protected refs, and destructive operations. Adding a new protected
branch is a one-line edit in that file (or a runtime submit-fact),
not a code change in the adapter.

Currently gated subcommands:

    branch -d/-D <name>     reject if <name> is_a protected_branch
    push --force ...        reject (push_force is_a destructive_op)
    push -f ...             alias of push --force
    reset --hard ...        reject (reset_hard is_a destructive_op)
    clean -f / -fd          reject (clean_force is_a destructive_op)
    checkout -- <path>      reject (checkout_discard is_a destructive_op)

Read-only subcommands (status, log, diff, branch with no args, ...)
pass through unverified.

Use:
    ./demo/tardy-git.py status                  # passthrough
    ./demo/tardy-git.py branch -D feature-x     # gate: feature-x not protected → execute
    ./demo/tardy-git.py branch -D main          # gate: main is_a protected_branch → REJECT
    ./demo/tardy-git.py push --force origin x   # gate: push_force is destructive → REJECT
    ./demo/tardy-git.py --explain branch -D main  # show the verification trace

The tardygrada daemon must be running:
    ./tardygrada daemon start
"""
import argparse
import json
import os
import socket
import subprocess
import sys

SOCK = "/tmp/tardygrada.sock"

GREEN  = "\033[0;32m"
YELLOW = "\033[1;33m"
RED    = "\033[0;31m"
DIM    = "\033[2m"
CYAN   = "\033[0;36m"
NC     = "\033[0m"


def daemon_query(claim):
    """Send a run command, return parsed JSON or None on failure."""
    if not os.path.exists(SOCK):
        return None
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(SOCK)
        s.sendall((json.dumps({"cmd": "run", "claim": claim}) + "\n").encode())
        s.shutdown(socket.SHUT_WR)
        buf = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        s.close()
        return json.loads(buf.decode().strip())
    except Exception:
        return None


def claim_grounded(claim):
    """True if the claim verifies AND there's at least one grounded
    triple (so we don't false-positive on the verifier's other passes)."""
    resp = daemon_query(claim)
    if not resp:
        return False, resp
    if resp.get("result") != "VERIFIED":
        return False, resp
    for t in resp.get("triples", []):
        if t.get("status") == "grounded":
            return True, resp
    return False, resp


# ----------------------------------------------------------------------
# Per-subcommand gates. Each returns (allowed, reason, response_json).
# ----------------------------------------------------------------------

def gate_branch(args):
    """git branch -d/-D <name> ..."""
    delete = False
    targets = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a in ("-d", "-D", "--delete"):
            delete = True
        elif a in ("-r", "--remotes", "-a", "--all", "-v", "-vv",
                   "--list", "--show-current", "-q", "--quiet"):
            pass
        elif a == "--force":
            delete = True
        elif a.startswith("-"):
            # unknown flag — pass through, treat as read-only
            pass
        else:
            targets.append(a)
    if not delete or not targets:
        return True, "read-only branch operation", None
    for tgt in targets:
        ok, resp = claim_grounded(f"{tgt} is a protected_branch")
        if ok:
            return False, f"{tgt} is a protected_branch", resp
    return True, "no protected branches in target list", None


def gate_push(args):
    """git push ..."""
    if any(a in ("--force", "-f") for a in args):
        ok, resp = claim_grounded("push_force is a destructive_op")
        if ok:
            return False, "push --force is a destructive_op", resp
    if "--force-with-lease" in args:
        # safer variant — let it through
        return True, "push --force-with-lease is the safe variant", None
    return True, "non-destructive push", None


def gate_reset(args):
    if "--hard" in args:
        ok, resp = claim_grounded("reset_hard is a destructive_op")
        if ok:
            return False, "reset --hard is a destructive_op", resp
    return True, "non-destructive reset", None


def gate_clean(args):
    if any(a in ("-f", "--force", "-fd", "-df", "-fdx", "-fX") for a in args):
        ok, resp = claim_grounded("clean_force is a destructive_op")
        if ok:
            return False, "clean -f is a destructive_op", resp
    return True, "non-destructive clean (would dry-run)", None


def gate_checkout(args):
    # `git checkout -- <path>` discards working-tree changes
    if "--" in args:
        ok, resp = claim_grounded("checkout_discard is a destructive_op")
        if ok:
            return False, "checkout -- discards working-tree changes", resp
    return True, "non-destructive checkout", None


GATES = {
    "branch":   gate_branch,
    "push":     gate_push,
    "reset":    gate_reset,
    "clean":    gate_clean,
    "checkout": gate_checkout,
}


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        add_help=False,  # let `git --help` flow through
        description="Verifiable git wrapper")
    ap.add_argument("--explain", action="store_true",
        help="print the verification trace before running")
    ap.add_argument("--allow-unsafe", action="store_true",
        help="ignore the gate (use with care)")
    ap.add_argument("--help-tardy", action="store_true",
        help="show this wrapper's help")
    args, rest = ap.parse_known_args()

    if args.help_tardy or not rest:
        print(__doc__)
        return 0

    subcmd = rest[0]
    sub_args = rest[1:]

    gate = GATES.get(subcmd)
    if gate is None:
        # Not a gated subcommand — pass through unverified.
        if args.explain:
            print(f"{DIM}[tardy-git] {subcmd} is read-only or "
                  f"un-gated; passing through{NC}", file=sys.stderr)
        return subprocess.call(["git"] + rest)

    allowed, reason, resp = gate(sub_args)

    if args.explain:
        print(f"{DIM}[tardy-git] gate: {subcmd}{NC}", file=sys.stderr)
        print(f"{DIM}[tardy-git] reason: {reason}{NC}", file=sys.stderr)
        if resp:
            for t in resp.get("triples", []):
                print(f"{DIM}[tardy-git] triple: {t}{NC}", file=sys.stderr)

    if not allowed and not args.allow_unsafe:
        print(f"{RED}REJECTED{NC} by tardygrada: {reason}", file=sys.stderr)
        if resp and resp.get("triples"):
            for t in resp["triples"]:
                if t.get("status") == "grounded":
                    print(f"  {DIM}grounded:{NC} "
                          f"{t['s']} {t['p']} {t['o']}  "
                          f"({DIM}tier={t.get('tier','?')}){NC}",
                          file=sys.stderr)
        print(f"  {DIM}override with --allow-unsafe if you really mean it"
              f"{NC}", file=sys.stderr)
        return 2

    if args.explain:
        print(f"{GREEN}[tardy-git] PASS{NC} → executing git "
              f"{' '.join(rest)}", file=sys.stderr)
    return subprocess.call(["git"] + rest)


if __name__ == "__main__":
    sys.exit(main())
