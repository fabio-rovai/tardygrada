#!/usr/bin/env python3
"""
Tardygrada dashboard — HTTP proxy.

Serves dashboard/index.html and exposes a JSON API that proxies
commands to the running tardygrada daemon over its Unix socket.

The dashboard is read-only by default for verify-style commands; it
can also submit claims and see the verification pipeline result.

Run from the repo root:
    python3 dashboard/server.py
    # then open http://127.0.0.1:8765 in a browser

Override the port:
    TARDY_DASHBOARD_PORT=9000 python3 dashboard/server.py
"""
import http.server
import json
import os
import re
import socket
import socketserver
import sys
import time
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(ROOT)
DAEMON_SOCK = os.environ.get("TARDY_DAEMON_SOCKET", "/tmp/tardygrada.sock")
PORT = int(os.environ.get("TARDY_DASHBOARD_PORT", "8765"))
ONTOLOGY_PATH = os.path.join(REPO_ROOT, "tests", "wikidata_common.nt")

# ----------------------------------------------------------------------
# Ontology parser — reads tests/wikidata_common.nt and returns a tree:
#   {
#     "name": "ontology",
#     "children": [
#       { "name": "capitalOf",
#         "children": [
#           {"name": "Paris -> France", "subject": "Paris", "object": "France",
#            "value": 1 }, ...
#         ] },
#       { "name": "creator", ...},
#       ...
#     ]
#   }
# Cached on first read; falls back gracefully if the file is missing.
# ----------------------------------------------------------------------

_ONTO_CACHE = {"mtime": 0, "tree": None}
IRI_RE = re.compile(r"<([^>]+)>")


def _local_name(iri):
    """<http://schema.org/capitalOf> -> capitalOf"""
    if "#" in iri:
        return iri.rsplit("#", 1)[-1]
    return iri.rsplit("/", 1)[-1]


def load_ontology_tree():
    """Parse the bundled .nt file into a treemap-ready tree.
    Skips the parse if the file hasn't changed since last load."""
    try:
        st = os.stat(ONTOLOGY_PATH)
    except FileNotFoundError:
        return {"name": "ontology", "children": [],
                "error": f"file not found: {ONTOLOGY_PATH}"}
    if _ONTO_CACHE["tree"] is not None and st.st_mtime == _ONTO_CACHE["mtime"]:
        return _ONTO_CACHE["tree"]

    by_pred = {}
    total = 0
    with open(ONTOLOGY_PATH) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            iris = IRI_RE.findall(line)
            if len(iris) < 3:
                continue
            s, p, o = iris[0], iris[1], iris[2]
            sn = _local_name(s)
            pn = _local_name(p)
            on = _local_name(o)
            by_pred.setdefault(pn, []).append({
                "name": f"{sn} → {on}",
                "subject": sn,
                "object": on,
                "value": 1,
            })
            total += 1

    children = []
    for pred, kids in sorted(by_pred.items(),
                             key=lambda kv: -len(kv[1])):
        children.append({
            "name": pred,
            "predicate": pred,
            "count": len(kids),
            "children": kids,
        })

    tree = {
        "name": "ontology",
        "total": total,
        "predicate_count": len(by_pred),
        "children": children,
    }
    _ONTO_CACHE["mtime"] = st.st_mtime
    _ONTO_CACHE["tree"] = tree
    return tree


def daemon_send(payload, timeout=10):
    """Send a JSON command to the daemon and return the parsed response.
    Returns a dict with {"ok": false, "error": "..."} on failure."""
    if not os.path.exists(DAEMON_SOCK):
        return {"ok": False, "error": "daemon not running"}
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(DAEMON_SOCK)
        msg = json.dumps(payload).encode() + b"\n"
        s.sendall(msg)
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if b"\n" in buf:
                break
        line = buf.split(b"\n", 1)[0]
        try:
            return json.loads(line.decode())
        except Exception as e:
            return {"ok": False, "error": f"invalid daemon response: {e}",
                    "raw": line.decode(errors="replace")}
    except socket.timeout:
        return {"ok": False, "error": "daemon timed out"}
    except Exception as e:
        return {"ok": False, "error": str(e)}
    finally:
        s.close()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Quieter log line — only print successful API hits
        pass

    def _json(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except FileNotFoundError:
            return self.send_error(404)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            return self._file(os.path.join(ROOT, "index.html"), "text/html")
        if u.path == "/api/status":
            return self._json(200, daemon_send({"cmd": "status"}))
        if u.path == "/api/run":
            qs = parse_qs(u.query)
            claim = (qs.get("claim", [""])[0] or "").strip()
            if not claim:
                return self._json(400, {"ok": False, "error": "empty claim"})
            t0 = time.monotonic()
            resp = daemon_send({"cmd": "run", "claim": claim})
            resp["_ms"] = int((time.monotonic() - t0) * 1000)
            return self._json(200, resp)
        if u.path == "/api/recall":
            qs = parse_qs(u.query)
            wing = qs.get("wing", ["claude-session"])[0]
            return self._json(200, daemon_send({
                "cmd": "recall", "wing": wing, "limit": 50
            }))
        if u.path == "/api/ontology":
            return self._json(200, load_ontology_tree())
        return self.send_error(404)


class ReusableTCPServer(socketserver.TCPServer):
    """Set SO_REUSEADDR before bind() so quick restarts after Ctrl+C
    don't hit 'Address already in use' for ~60s. Standard practice on
    development servers."""
    allow_reuse_address = True


def main():
    if not os.path.exists(DAEMON_SOCK):
        print(f"[!] Daemon not running at {DAEMON_SOCK}", file=sys.stderr)
        print("    Start it first:  ./tardygrada daemon start", file=sys.stderr)
        # Continue anyway — dashboard will show "daemon offline" until it appears
    print(f"[tardygrada-dashboard] serving http://127.0.0.1:{PORT}/")
    print(f"[tardygrada-dashboard] daemon socket: {DAEMON_SOCK}")
    print("[tardygrada-dashboard] Ctrl+C to stop")
    with ReusableTCPServer(("127.0.0.1", PORT), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[tardygrada-dashboard] stopped")


if __name__ == "__main__":
    main()
