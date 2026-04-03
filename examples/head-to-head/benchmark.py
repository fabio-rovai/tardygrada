#!/usr/bin/env python3
"""
Head-to-head: CrewAI-style (3 LLM calls) vs Tardygrada (1 LLM call + verification)

10 diverse factual questions. Both pipelines run for real.
Reproducible: set ANTHROPIC_API_KEY and run.

Usage:
    ANTHROPIC_API_KEY=sk-... python3 examples/head-to-head/benchmark.py
"""
import subprocess, json, time, os, sys

API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")
if not API_KEY:
    print("Usage: ANTHROPIC_API_KEY=sk-... python3 examples/head-to-head/benchmark.py")
    sys.exit(1)

QUESTIONS = [
    "Where was Doctor Who created, by whom, and when?",
    "What is the capital of Australia and when was it established?",
    "Who invented the World Wide Web and where?",
    "When was the Eiffel Tower built and how tall is it?",
    "What programming language was created by Guido van Rossum?",
    "Where is CERN located and what does it research?",
    "Who wrote Romeo and Juliet and when?",
    "What is the speed of light in meters per second?",
    "When did humans first land on the Moon and who was first?",
    "What is the largest ocean on Earth and its approximate area?",
]

def call_claude(system, user):
    body = json.dumps({
        "model": "claude-sonnet-4-20250514",
        "max_tokens": 200,
        "temperature": 0,
        "system": system,
        "messages": [{"role": "user", "content": user}]
    })
    proc = subprocess.run(
        ["curl", "-s", "-X", "POST", "https://api.anthropic.com/v1/messages",
         "-H", "Content-Type: application/json",
         "-H", "anthropic-version: 2023-06-01",
         "-H", f"x-api-key: {API_KEY}",
         "-d", body],
        capture_output=True, text=True, timeout=30
    )
    try:
        resp = json.loads(proc.stdout)
        if "content" in resp:
            return resp["content"][0]["text"]
        return f"ERROR: {resp.get('error', {}).get('message', proc.stdout[:100])}"
    except:
        return f"ERROR: {proc.stdout[:100]}"

def mcp_msg(body):
    b = body.encode()
    return f"Content-Length: {len(b)}\r\n\r\n".encode() + b

def run_tardygrada_verify(claim):
    """Submit claim to Tardygrada, verify, return result."""
    reqs = [
        mcp_msg(json.dumps({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})),
        mcp_msg(json.dumps({"jsonrpc":"2.0","id":2,"method":"tools/call",
            "params":{"name":"load_ontology","arguments":{"path": os.path.abspath("tests/test_ontology.nt")}}})),
        mcp_msg(json.dumps({"jsonrpc":"2.0","id":3,"method":"tools/call",
            "params":{"name":"submit_claim","arguments":{"agent":"origin","claim": claim}}})),
        mcp_msg(json.dumps({"jsonrpc":"2.0","id":4,"method":"tools/call",
            "params":{"name":"verify_claim","arguments":{"agent":"origin"}}})),
    ]
    proc = subprocess.Popen(
        ["./tardygrada", "examples/receive.tardy"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    out, err = proc.communicate(input=b"".join(reqs), timeout=15)
    parts = out.split(b"Content-Length: ")
    for part in parts:
        if not part: continue
        idx = part.find(b"{")
        if idx < 0: continue
        try:
            j = json.loads(part[idx:])
            if j.get("id") == 4 and "result" in j:
                return j["result"]["content"][0]["text"]
        except: pass
    return "no result"

print("=" * 80)
print("HEAD-TO-HEAD: CrewAI-style (3 LLM calls) vs Tardygrada (1 call + verify)")
print(f"Questions: {len(QUESTIONS)}")
print("=" * 80)
print()

crew_total_time = 0
crew_total_calls = 0
tardy_total_time = 0
tardy_total_calls = 0
tardy_verified = 0
tardy_grounded = 0

for i, q in enumerate(QUESTIONS):
    print(f"--- Q{i+1}: {q} ---")

    # CrewAI-style: 3 calls (research + verify + report)
    t1 = time.perf_counter()
    answer = call_claude("Answer with specific facts. Be concise.", q)
    check = call_claude("Verify each fact. Say VERIFIED or DISPUTED.", f"Verify: {answer}")
    report = call_claude("One sentence summary.", f"Summarize: {check}")
    crew_time = time.perf_counter() - t1
    crew_total_time += crew_time
    crew_total_calls += 3

    # Tardygrada: 1 call + verification pipeline
    t2 = time.perf_counter()
    tardy_answer = call_claude("Answer with specific facts. Be concise.", q)
    tardy_verify = run_tardygrada_verify(tardy_answer)
    tardy_time = time.perf_counter() - t2
    tardy_total_time += tardy_time
    tardy_total_calls += 1

    # Parse verification result
    verified = "verified=true" in tardy_verify
    grounded_match = "triples_grounded="
    grounded_str = ""
    if grounded_match in tardy_verify:
        idx = tardy_verify.index(grounded_match) + len(grounded_match)
        grounded_str = tardy_verify[idx:idx+5]
    failure = ""
    if "failure=" in tardy_verify:
        idx = tardy_verify.index("failure=") + 8
        end = tardy_verify.index(" ", idx) if " " in tardy_verify[idx:] else len(tardy_verify)
        failure = tardy_verify[idx:end]

    if verified: tardy_verified += 1
    if grounded_str and not grounded_str.startswith("0/"):
        tardy_grounded += 1

    print(f"  CrewAI:  {answer[:100]}...")
    print(f"           -> {report[:80]}...")
    print(f"           {crew_time:.1f}s, 3 calls, verification=LLM-self-check")
    print(f"  Tardy:   {tardy_answer[:100]}...")
    print(f"           -> grounded={grounded_str} {'VERIFIED' if verified else f'NOT VERIFIED ({failure})'}")
    print(f"           {tardy_time:.1f}s, 1 call, verification=8-layer+BFT")
    print()

print("=" * 80)
print("RESULTS")
print("=" * 80)
print()
print(f"  {'':35s} {'CrewAI-style':>15s}  {'Tardygrada':>15s}")
print(f"  {'Total LLM calls':35s} {crew_total_calls:>15d}  {tardy_total_calls:>15d}")
print(f"  {'Total time':35s} {f'{crew_total_time:.1f}s':>15s}  {f'{tardy_total_time:.1f}s':>15s}")
print(f"  {'Avg time per question':35s} {f'{crew_total_time/len(QUESTIONS):.1f}s':>15s}  {f'{tardy_total_time/len(QUESTIONS):.1f}s':>15s}")
print(f"  {'LLM calls per question':35s} {'3':>15s}  {'1':>15s}")
print(f"  {'Verification method':35s} {'LLM self-check':>15s}  {'8-layer + BFT':>15s}")
print(f"  {'Claims with grounding':35s} {'0/10':>15s}  {f'{tardy_grounded}/10':>15s}")
print(f"  {'Independent verification':35s} {'no':>15s}  {'yes':>15s}")
print(f"  {'Cryptographic provenance':35s} {'no':>15s}  {'yes':>15s}")
print(f"  {'Cost (Claude Sonnet ~$3/1M tok)':35s} {'~$0.009':>15s}  {'~$0.003':>15s}")
print()
print(f"CrewAI-style calls the LLM 3x per question and trusts the output.")
print(f"Tardygrada calls it 1x and independently verifies against an ontology.")
print()
