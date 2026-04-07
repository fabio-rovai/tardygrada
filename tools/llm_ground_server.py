#!/usr/bin/env python3
"""
Tardygrada — LLM Grounding Server

Listens on a Unix socket and answers grounding queries using the Anthropic API.
Each request is one line of JSON, each response is one line of JSON.

Usage:
    ANTHROPIC_API_KEY=... python3 tools/llm_ground_server.py

Protocol:
    {"action": "ground_triple", "subject": "Paris", "predicate": "is_capital_of", "object": "France"}
    -> {"grounded": true, "confidence": 0.95, "explanation": "Paris is the capital of France"}

    {"action": "ground_claim", "claim": "The population of Earth in 2000 was 6.126 billion"}
    -> {"grounded": true, "confidence": 0.9, "explanation": "Approximately correct"}
"""

import socket
import json
import os
import sys
import signal

try:
    import anthropic
except ImportError:
    print("Error: anthropic package not installed. Run: pip install anthropic", file=sys.stderr)
    sys.exit(1)

SOCKET_PATH = "/tmp/tardygrada-llm.sock"
MODEL = os.getenv("TARDY_LLM_MODEL", "claude-haiku-4-5-20251001")

client = anthropic.Anthropic()


def parse_llm_json(text):
    """Parse JSON from LLM response, handling markdown code blocks."""
    text = text.strip()
    # Handle ```json ... ``` wrapping
    if "```" in text:
        parts = text.split("```")
        for part in parts[1:]:
            cleaned = part.strip()
            if cleaned.startswith("json"):
                cleaned = cleaned[4:].strip()
            # Try to parse this block
            try:
                return json.loads(cleaned)
            except json.JSONDecodeError:
                continue
    # Try direct parse
    return json.loads(text)


def ground_triple(subject, predicate, obj):
    """Ask LLM if a triple is factual."""
    prompt = (
        f"Is this factual? {subject} {predicate} {obj}.\n"
        f"Respond with ONLY JSON (no markdown): "
        f'{{\"grounded\": true/false, \"confidence\": 0.0-1.0, \"explanation\": \"brief reason\"}}'
    )
    response = client.messages.create(
        model=MODEL,
        max_tokens=200,
        messages=[{"role": "user", "content": prompt}],
    )
    return parse_llm_json(response.content[0].text)


def ground_claim(claim):
    """Ask LLM if a claim is factually accurate."""
    prompt = (
        f'Is this claim factually accurate? "{claim}"\n'
        f"Respond with ONLY JSON (no markdown): "
        f'{{\"grounded\": true/false, \"confidence\": 0.0-1.0, \"explanation\": \"brief reason\"}}'
    )
    response = client.messages.create(
        model=MODEL,
        max_tokens=200,
        messages=[{"role": "user", "content": prompt}],
    )
    return parse_llm_json(response.content[0].text)


def handle_request(data):
    """Process a single JSON request and return a JSON response."""
    req = json.loads(data)
    action = req.get("action")

    if action == "ground_triple":
        return ground_triple(req["subject"], req["predicate"], req["object"])
    elif action == "ground_claim":
        return ground_claim(req["claim"])
    else:
        return {"error": f"unknown action: {action}"}


def cleanup(signum=None, frame=None):
    """Remove socket file on exit."""
    if os.path.exists(SOCKET_PATH):
        os.remove(SOCKET_PATH)
    sys.exit(0)


def main():
    # Clean up old socket
    if os.path.exists(SOCKET_PATH):
        os.remove(SOCKET_PATH)

    # Register cleanup
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(5)
    print(f"LLM grounding server listening on {SOCKET_PATH}")
    print(f"Model: {MODEL}")
    print("Waiting for connections...")

    try:
        while True:
            conn, _ = server.accept()
            try:
                data = conn.recv(8192).decode("utf-8").strip()
                if not data:
                    conn.close()
                    continue

                print(f"<- {data[:120]}{'...' if len(data) > 120 else ''}")
                result = handle_request(data)
                response = json.dumps(result) + "\n"
                print(f"-> {response.strip()[:120]}{'...' if len(response) > 120 else ''}")
                conn.send(response.encode("utf-8"))
            except Exception as e:
                error_response = json.dumps({"error": str(e)}) + "\n"
                print(f"!! Error: {e}", file=sys.stderr)
                try:
                    conn.send(error_response.encode("utf-8"))
                except Exception:
                    pass
            finally:
                conn.close()
    finally:
        cleanup()


if __name__ == "__main__":
    main()
