#!/usr/bin/env python3
"""
CrewAI-style pipeline WITHOUT API calls.
Uses the same pattern but with local functions instead of LLM calls.
This is what CrewAI actually does, minus the LLM.

Task: Fetch Doctor Who data from Wikipedia, extract facts, verify them.
"""
import urllib.request
import json
import time
import hashlib

t_start = time.perf_counter_ns()

# --- Agent 1: Researcher ---
# Fetch data (what CrewAI's researcher agent would do after LLM decides to search)
url = "https://en.wikipedia.org/api/rest_v1/page/summary/Doctor_Who"
try:
    with urllib.request.urlopen(url, timeout=10) as resp:
        wiki_data = json.loads(resp.read().decode())
        extract = wiki_data.get("extract", "")
except Exception as e:
    extract = f"fetch failed: {e}"

# --- Agent 2: Fact Checker ---
# Extract claims (what CrewAI's fact_checker would do)
claims = []
if "BBC" in extract:
    claims.append({"claim": "Doctor Who is associated with BBC", "found": True})
if "1963" in extract:
    claims.append({"claim": "Doctor Who dates to 1963", "found": True})
if "Sydney Newman" in extract:
    claims.append({"claim": "Sydney Newman involved", "found": True})
if "Television Centre" in extract:
    claims.append({"claim": "Television Centre mentioned", "found": True})

# "Verification" = checking if the text contains the keywords
# This is literally all CrewAI can do without an ontology
verified_count = sum(1 for c in claims if c["found"])

# --- Agent 3: Reporter ---
report = f"Found {len(claims)} claims, {verified_count} verified (keyword match only)"

t_total = (time.perf_counter_ns() - t_start) / 1_000_000

# --- Output ---
print(f"=== CrewAI-style (Python, local execution) ===")
print(f"Extract: {extract[:150]}...")
print(f"Claims: {len(claims)}")
for c in claims:
    print(f"  [{'OK' if c['found'] else 'NO'}] {c['claim']}")
print(f"Report: {report}")
print(f"Time: {t_total:.0f}ms")
print(f"Verification: keyword matching (no ontology, no hash, no signature)")
print(f"Provenance: none")
print(f"Hash of output: none")
print()
