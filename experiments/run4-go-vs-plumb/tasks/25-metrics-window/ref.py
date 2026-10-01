#!/usr/bin/env python3
import json, math, os, socketserver, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

WINDOW = int(os.environ.get("WINDOW_MS", "60000"))
lock = threading.Lock()
series = {}                 # name -> list of (ts, value)
state = {"accepted": 0, "dropped": 0, "now": None}


def valid(p):
    if not isinstance(p, dict):
        return False
    n, v, t = p.get("name"), p.get("value"), p.get("ts")
    if not isinstance(n, str) or n == "":
        return False
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
        return False
    return isinstance(t, int) and not isinstance(t, bool) and t >= 0


def aggregate(name):
    with lock:
        now = state["now"]
        vals = [v for t, v in series.get(name, []) if now is not None and now - WINDOW < t <= now]
    if not vals:
        return {"name": name, "count": 0, "sum": 0, "min": None, "max": None, "p50": None, "p99": None}
    vals = sorted(float(v) for v in vals)
    n = len(vals)
    rank = lambda p: vals[max(1, math.ceil(p * n / 100)) - 1]
    return {"name": name, "count": n, "sum": sum(vals), "min": vals[0],
            "max": vals[-1], "p50": rank(50), "p99": rank(99)}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def route(self, method):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        path, _, query = self.path.partition("?")
        want = {"/metrics": "POST", "/aggregate": "GET", "/stats": "GET"}.get(path)
        if want is None:
            return self.send(404, {"error": "not found"})
        if method != want:
            return self.send(405, {"error": "method not allowed"})
        if path == "/stats":
            with lock:
                return self.send(200, dict(state))
        if path == "/aggregate":
            q = parse_qs(query, keep_blank_values=True)
            if "name" not in q:
                return self.send(400, {"error": "missing name"})
            return self.send(200, aggregate(q["name"][0]))
        try:
            body = json.loads(raw)
        except ValueError:
            return self.send(400, {"error": "invalid JSON"})
        items = body if isinstance(body, list) else [body]
        if not all(valid(p) for p in items):
            return self.send(400, {"error": "invalid sample"})
        acc = drop = 0
        with lock:
            for p in items:
                t = p["ts"]
                state["now"] = t if state["now"] is None else max(state["now"], t)
                if t <= state["now"] - WINDOW:
                    drop += 1
                else:
                    acc += 1
                    series.setdefault(p["name"], []).append((t, p["value"]))
            state["accepted"] += acc
            state["dropped"] += drop
        self.send(200, {"accepted": acc, "dropped": drop})

    def do_GET(self): self.route("GET")
    def do_POST(self): self.route("POST")
    def do_PUT(self): self.route("PUT")
    def do_DELETE(self): self.route("DELETE")


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
