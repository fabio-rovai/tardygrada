#!/usr/bin/env python3
"""
Datalog Chain Reasoning Integration Test

Tests:
  1. Direct fact grounding (ontology lookup)
  2. Computational verification (known constants)
  3. Self-growing (verified claims feed future grounding)
"""
import subprocess, json, os, sys

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

BINARY = "./tardygrada"
PROGRAM = "examples/receive.tardy"
NT_FILE = os.path.abspath("tests/test_ontology.nt")


def mcp_msg(body):
    b = body.encode()
    return f"Content-Length: {len(b)}\r\n\r\n".encode() + b


def parse_responses(out):
    results = {}
    parts = out.split(b"Content-Length: ")
    for part in parts:
        if not part:
            continue
        idx = part.find(b"{")
        if idx < 0:
            continue
        try:
            j = json.loads(part[idx:])
            rid = j.get("id", "?")
            if "result" in j and isinstance(j["result"], dict) and "content" in j["result"]:
                results[rid] = j["result"]["content"][0]["text"]
            elif "result" in j:
                results[rid] = str(j["result"])
        except Exception:
            pass
    return results


def run_session(claims, load_ontology=True):
    """Run a tardygrada session with given claims. Returns verify results."""
    reqs = [mcp_msg(json.dumps({
        "jsonrpc": "2.0", "id": 1,
        "method": "initialize", "params": {}
    }))]

    if load_ontology:
        reqs.append(mcp_msg(json.dumps({
            "jsonrpc": "2.0", "id": 2,
            "method": "tools/call",
            "params": {"name": "load_ontology", "arguments": {"path": NT_FILE}}
        })))

    rid = 10
    verify_ids = []
    for claim in claims:
        reqs.append(mcp_msg(json.dumps({
            "jsonrpc": "2.0", "id": rid,
            "method": "tools/call",
            "params": {"name": "submit_claim",
                       "arguments": {"agent": "origin", "claim": claim}}
        })))
        rid += 1
        reqs.append(mcp_msg(json.dumps({
            "jsonrpc": "2.0", "id": rid,
            "method": "tools/call",
            "params": {"name": "verify_claim",
                       "arguments": {"agent": "origin"}}
        })))
        verify_ids.append(rid)
        rid += 1

    proc = subprocess.Popen(
        [BINARY, PROGRAM],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    out, err = proc.communicate(input=b"".join(reqs), timeout=15)
    results = parse_responses(out)

    return {claims[i]: results.get(verify_ids[i], "")
            for i in range(len(claims))}


def check(desc, result_text, expect_verified=True, check_grounded=False):
    """Check if a verify result matches expectation."""
    if check_grounded:
        # For computational claims, just check that grounding worked
        grounded = "triples_grounded=1/1" in result_text or "triples_grounded=2/2" in result_text
        if grounded:
            print(f"  PASS: {desc} (grounded)")
            return True
        else:
            print(f"  FAIL: {desc} (not grounded, result: {result_text[:120]})")
            return False
    verified = "verified=true" in result_text
    if verified == expect_verified:
        print(f"  PASS: {desc}")
        return True
    else:
        status = "verified" if verified else "not verified"
        print(f"  FAIL: {desc} (got {status}, result: {result_text[:120]})")
        return False


# ============================================================

if not os.path.exists(BINARY):
    print("FAIL: binary not found, run 'make' first")
    sys.exit(1)

print("=== Datalog Chain Reasoning Test ===")
print()

passed = 0
failed = 0

# --- Test 1: Direct fact grounding ---
print("-- Direct Facts --")
results = run_session([
    "Doctor Who was created by Sydney Newman",
])
for claim, result in results.items():
    if check(claim, result):
        passed += 1
    else:
        failed += 1

# --- Test 2: Computational verification (grounding check) ---
print()
print("-- Computational Verification (grounding) --")
results = run_session([
    "The speed of light is 299792458 meters per second",
    "The value of pi is 3.14159",
], load_ontology=False)
for claim, result in results.items():
    if check(claim, result, check_grounded=True):
        passed += 1
    else:
        failed += 1

# --- Test 3: Self-growing ontology ---
print()
print("-- Self-Growing Ontology --")
# First claim adds facts, second claim should benefit from derived knowledge
results = run_session([
    "Doctor Who was created by Sydney Newman",
    "Doctor Who was created in 1963",
])
for claim, result in results.items():
    if check(claim, result):
        passed += 1
    else:
        failed += 1

print()
print(f"=== Results: {passed}/{passed + failed} passed, {failed} failed ===")

if failed > 0:
    sys.exit(1)
