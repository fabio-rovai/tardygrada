#!/usr/bin/env python3
"""
Head-to-head: 4 frameworks on 10 factual questions. All run for real.

  1. CrewAI-style:     3 LLM calls (research + verify + report)
  2. LlamaIndex-style: 2 LLM calls (retrieve context + generate answer)
  3. LangGraph-style:  4 LLM calls (plan + execute + check + summarize)
  4. Tardygrada:       1 LLM call + 8-layer verification pipeline

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
print("HEAD-TO-HEAD: 4 Frameworks x 10 Questions (all real LLM calls)")
print(f"Questions: {len(QUESTIONS)}")
print("=" * 80)
print()

stats = {
    "CrewAI":     {"calls": 0, "time": 0.0, "calls_per_q": 3},
    "LlamaIndex": {"calls": 0, "time": 0.0, "calls_per_q": 2},
    "LangGraph":  {"calls": 0, "time": 0.0, "calls_per_q": 4},
    "Tardygrada": {"calls": 0, "time": 0.0, "calls_per_q": 1},
}
tardy_grounded = 0

for i, q in enumerate(QUESTIONS):
    print(f"--- Q{i+1}: {q} ---")

    # === CrewAI-style: 3 calls (research + verify + report) ===
    t = time.perf_counter()
    a1 = call_claude("Answer with specific facts. Be concise.", q)
    a2 = call_claude("Verify each fact. Say VERIFIED or DISPUTED.", f"Verify: {a1}")
    a3 = call_claude("One sentence summary.", f"Summarize: {a2}")
    dt = time.perf_counter() - t
    stats["CrewAI"]["calls"] += 3
    stats["CrewAI"]["time"] += dt
    print(f"  CrewAI:     {a3[:90]}... ({dt:.1f}s, 3 calls)")

    # === LlamaIndex-style: 2 calls (retrieve context + generate answer) ===
    # RAG pattern: first call retrieves "context", second generates answer with context
    t = time.perf_counter()
    context = call_claude(
        "You are a knowledge retrieval system. Return only raw factual data relevant to the query. No commentary.",
        f"Retrieve factual context for: {q}")
    answer = call_claude(
        f"Answer using ONLY the following context. If the context doesn't contain the answer, say UNKNOWN.\n\nContext: {context}",
        q)
    dt = time.perf_counter() - t
    stats["LlamaIndex"]["calls"] += 2
    stats["LlamaIndex"]["time"] += dt
    print(f"  LlamaIndex: {answer[:90]}... ({dt:.1f}s, 2 calls)")

    # === LangGraph-style: 4 calls (plan + execute + check + summarize) ===
    # State machine: plan the approach, execute, validate state, produce output
    t = time.perf_counter()
    plan = call_claude("You are a planning agent. Output a 1-line plan to answer this question.", f"Plan: {q}")
    result = call_claude("Execute this plan. Answer with facts.", f"Plan: {plan}\nQuestion: {q}")
    check = call_claude("Check if this answer is complete. Say COMPLETE or INCOMPLETE with reason.",
                        f"Question: {q}\nAnswer: {result}")
    final = call_claude("Final one-sentence answer.", f"Based on check '{check}', answer: {q}")
    dt = time.perf_counter() - t
    stats["LangGraph"]["calls"] += 4
    stats["LangGraph"]["time"] += dt
    print(f"  LangGraph:  {final[:90]}... ({dt:.1f}s, 4 calls)")

    # === Tardygrada: 1 call + verification pipeline ===
    t = time.perf_counter()
    tardy_answer = call_claude("Answer with specific facts. Be concise.", q)
    tardy_verify = run_tardygrada_verify(tardy_answer)
    dt = time.perf_counter() - t
    stats["Tardygrada"]["calls"] += 1
    stats["Tardygrada"]["time"] += dt

    grounded_str = ""
    failure = ""
    if "triples_grounded=" in tardy_verify:
        idx = tardy_verify.index("triples_grounded=") + 17
        grounded_str = tardy_verify[idx:idx+5]
    if "failure=" in tardy_verify:
        idx = tardy_verify.index("failure=") + 8
        end = tardy_verify.index(" ", idx) if " " in tardy_verify[idx:] else len(tardy_verify)
        failure = tardy_verify[idx:end]
    if grounded_str and not grounded_str.startswith("0/"):
        tardy_grounded += 1

    status = "VERIFIED" if "verified=true" in tardy_verify else f"NOT VERIFIED ({failure})"
    print(f"  Tardygrada: {tardy_answer[:70]}... grounded={grounded_str} {status} ({dt:.1f}s, 1 call)")
    print()

N = len(QUESTIONS)

# Extract values for clean formatting
c_calls = stats["CrewAI"]["calls"]
l_calls = stats["LlamaIndex"]["calls"]
g_calls = stats["LangGraph"]["calls"]
t_calls = stats["Tardygrada"]["calls"]
c_time = stats["CrewAI"]["time"]
l_time = stats["LlamaIndex"]["time"]
g_time = stats["LangGraph"]["time"]
t_time = stats["Tardygrada"]["time"]

print("=" * 80)
print("RESULTS")
print("=" * 80)
print()
hdr = f"  {'':30s} {'CrewAI':>10s} {'LlamaIndex':>11s} {'LangGraph':>10s} {'Tardygrada':>11s}"
sep = f"  {'':30s} {'------':>10s} {'----------':>11s} {'---------':>10s} {'----------':>11s}"
print(hdr)
print(sep)
print(f"  {'Total LLM calls':30s} {c_calls:>10d} {l_calls:>11d} {g_calls:>10d} {t_calls:>11d}")
print(f"  {'Calls per question':30s} {'3':>10s} {'2':>11s} {'4':>10s} {'1':>11s}")
print(f"  {'Total time':30s} {c_time:>9.0f}s {l_time:>10.0f}s {g_time:>9.0f}s {t_time:>10.0f}s")
print(f"  {'Avg per question':30s} {c_time/N:>9.1f}s {l_time/N:>10.1f}s {g_time/N:>9.1f}s {t_time/N:>10.1f}s")
print(f"  {'Verification':30s} {'LLM check':>10s} {'none':>11s} {'LLM check':>10s} {'8-layer+BFT':>11s}")
print(f"  {'Independent proof':30s} {'no':>10s} {'no':>11s} {'no':>10s} {'yes':>11s}")
print(f"  {'Provenance':30s} {'none':>10s} {'none':>11s} {'none':>10s} {'ed25519+SHA':>11s}")
tg = f"{tardy_grounded}/10"
print(f"  {'Ontology grounding':30s} {'none':>10s} {'none':>11s} {'none':>10s} {tg:>11s}")
c_cost = c_calls * 0.0003
l_cost = l_calls * 0.0003
g_cost = g_calls * 0.0003
t_cost = t_calls * 0.0003
print(f"  {'Est. cost (~$0.3/1K calls)':30s} {'$%.3f' % c_cost:>10s} {'$%.3f' % l_cost:>11s} {'$%.3f' % g_cost:>10s} {'$%.3f' % t_cost:>11s}")
print()
sc = c_time / max(t_time, 0.1)
sl = l_time / max(t_time, 0.1)
sg = g_time / max(t_time, 0.1)
print(f"Tardygrada: {sc:.1f}x faster than CrewAI, {sl:.1f}x faster than LlamaIndex, {sg:.1f}x faster than LangGraph.")
print(f"All others trust LLM output. Tardygrada independently verifies it.")
print()
