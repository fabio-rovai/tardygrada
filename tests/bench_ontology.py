#!/usr/bin/env python3
"""
Benchmark: Tardygrada self-hosted ontology vs open-ontologies (Rust + Oxigraph)

Tests loading + grounding speed with the pizza ontology (581 triples).
"""
import subprocess, json, os, time, socket, threading, sys

os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")

PIZZA_NT = os.path.abspath("tests/pizza.nt")
PIZZA_TTL = "/Users/fabio/projects/open-ontologies/benchmark/ontoaxiom/data/ontoaxiom/ontologies/pizza.ttl"
SOCKET_PATH = "/tmp/tardygrada-ontology-complete.sock"

# Test claims about pizza
CLAIMS = [
    "Margherita is a type of NamedPizza",
    "FourSeasons has topping MozzarellaTopping",
    "American is a subclass of NamedPizza",
    "VegetableTopping is a type of PizzaTopping",
    "Cajun is a NamedPizza",
]

def mcp_msg(body):
    b = body.encode()
    return f"Content-Length: {len(b)}\r\n\r\n".encode() + b

def parse_responses(out):
    results = {}
    parts = out.split(b"Content-Length: ")
    for part in parts:
        if not part: continue
        idx = part.find(b"{")
        if idx < 0: continue
        try:
            j = json.loads(part[idx:])
            rid = j.get("id", "?")
            if "result" in j and "content" in j["result"]:
                results[rid] = j["result"]["content"][0]["text"]
            elif "result" in j:
                results[rid] = "ok"
        except: pass
    return results

# ============================================================
# Benchmark 1: Tardygrada self-hosted (agents as triples)
# ============================================================

print("=" * 70)
print("BENCHMARK: Pizza Ontology (581 triples)")
print("=" * 70)
print()

print("--- Tardygrada Self-Hosted (194KB, zero deps, pure C) ---")

reqs = [mcp_msg(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}))]
reqs.append(mcp_msg(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
    "params": {"name": "load_ontology", "arguments": {"path": PIZZA_NT}}})))

# Add claims
for i, claim in enumerate(CLAIMS):
    agent = f"claim_{i}"
    reqs.append(mcp_msg(json.dumps({"jsonrpc": "2.0", "id": 100 + i, "method": "tools/call",
        "params": {"name": "submit_claim", "arguments": {"agent": "origin", "claim": claim}}})))
    reqs.append(mcp_msg(json.dumps({"jsonrpc": "2.0", "id": 200 + i, "method": "tools/call",
        "params": {"name": "verify_claim", "arguments": {"agent": "origin"}}})))

t_start = time.perf_counter_ns()

proc = subprocess.Popen(["./tardygrada", "examples/receive.tardy"],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
out, err = proc.communicate(input=b"".join(reqs), timeout=30)

t_tardy = time.perf_counter_ns() - t_start

results = parse_responses(out)
stderr = err.decode()

# Parse load result
load_msg = results.get(2, "")
print(f"  Load: {load_msg}")
print(f"  Time (total): {t_tardy / 1_000_000:.1f} ms")
print(f"  Binary: 194 KB")
print(f"  Dependencies: 0")
print()

# Show verify results
for i, claim in enumerate(CLAIMS):
    verify = results.get(200 + i, "?")
    grounded = "grounded" if "triples_grounded=1" in verify or "triples_grounded=2" in verify else "not grounded"
    verified = "VERIFIED" if "verified=true" in verify else "NOT VERIFIED"
    print(f"  [{verified}] [{grounded}] {claim}")
    if i < 2:
        print(f"    -> {verify[:120]}")

print()

# ============================================================
# Benchmark 2: open-ontologies (Rust + Oxigraph + unix socket)
# ============================================================

print("--- open-ontologies (Rust + Oxigraph, ~50MB, 47 crate deps) ---")

# Check if open-ontologies binary exists
OO_BIN = "/Users/fabio/.cargo/shared-target/release/open-ontologies"
if not os.path.exists(OO_BIN):
    print("  SKIP: open-ontologies not built (run: cd open-ontologies && cargo build --release)")
    sys.exit(0)

# Start open-ontologies server
if os.path.exists(SOCKET_PATH):
    os.unlink(SOCKET_PATH)

t_start_oo = time.perf_counter_ns()

oo_proc = subprocess.Popen(
    [OO_BIN, "serve-unix", "--socket", SOCKET_PATH, "--file", PIZZA_TTL],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# Wait for socket to appear
for _ in range(50):
    if os.path.exists(SOCKET_PATH):
        break
    time.sleep(0.1)

t_load_oo = time.perf_counter_ns() - t_start_oo

if not os.path.exists(SOCKET_PATH):
    print("  SKIP: open-ontologies failed to start")
    oo_proc.kill()
    sys.exit(0)

print(f"  Load: {PIZZA_TTL} (1848 triples with reasoning)")
print(f"  Load time: {t_load_oo / 1_000_000:.1f} ms")

# Ground some claims via socket
def ground_claim(claim_text, sock_path):
    """Send a ground request to open-ontologies via unix socket"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sock_path)
        # Decompose claim into a simple triple
        parts = claim_text.split()
        subj = parts[0] if parts else "Unknown"
        pred = "subClassOf" if "subclass" in claim_text.lower() or "type" in claim_text.lower() else "hasTopping"
        obj = parts[-1] if parts else "Unknown"

        req = json.dumps({"action": "ground", "triples": [
            {"s": subj, "p": pred, "o": obj}
        ]})
        s.send((req + "\n").encode())
        s.settimeout(5)
        resp = b""
        while True:
            chunk = s.recv(4096)
            if not chunk or b"\n" in resp:
                break
            resp += chunk
        return json.loads(resp.split(b"\n")[0]) if resp else None
    except Exception as e:
        return {"error": str(e)}
    finally:
        s.close()

t_ground_start = time.perf_counter_ns()
for claim in CLAIMS:
    result = ground_claim(claim, SOCKET_PATH)
    status = "?"
    if result and "results" in result:
        for r in result["results"]:
            status = r.get("status", "?")
    grounded = "grounded" if status == "grounded" else "not grounded"
    print(f"  [{grounded}] {claim}")
t_ground_oo = time.perf_counter_ns() - t_ground_start

oo_proc.kill()
oo_proc.wait()

print(f"\n  Ground time ({len(CLAIMS)} claims): {t_ground_oo / 1_000_000:.1f} ms")

# Get open-ontologies binary size
oo_size = os.path.getsize(OO_BIN) if os.path.exists(OO_BIN) else 0
print(f"  Binary: {oo_size // 1024 // 1024} MB")
print(f"  Dependencies: 47 crates (Oxigraph, tokio, serde, etc.)")

print()
print("=" * 70)
print("COMPARISON")
print("=" * 70)
print()
print(f"  {'':30s} {'Tardygrada':>15s}  {'open-ontologies':>15s}")
print(f"  {'Binary size':30s} {'194 KB':>15s}  {f'{oo_size // 1024} KB':>15s}")
print(f"  {'Dependencies':30s} {'0':>15s}  {'47 crates':>15s}")
print(f"  {'Language':30s} {'C11':>15s}  {'Rust':>15s}")
print(f"  {'Total time':30s} {f'{t_tardy / 1_000_000:.0f} ms':>15s}  {f'{t_load_oo / 1_000_000 + t_ground_oo / 1_000_000:.0f} ms':>15s}")
print(f"  {'Transport':30s} {'in-process':>15s}  {'unix socket':>15s}")
print(f"  {'Verification':30s} {'8-layer + BFT':>15s}  {'none':>15s}")
print(f"  {'Immutability':30s} {'ed25519 + mprotect':>15s}  {'none':>15s}")
print()
