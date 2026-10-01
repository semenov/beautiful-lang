#!/usr/bin/env python3
import json, math, os, socketserver, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def fail(msg):
    sys.stderr.write(f"error: {msg}\n")
    sys.exit(2)


try:
    cap_s = os.environ.get("CAPACITY", "10")
    if not cap_s.isdigit() or int(cap_s) < 1:
        fail("CAPACITY must be a whole number >= 1")
    CAP = int(cap_s)
    RATE = float(os.environ.get("REFILL_PER_SEC", "1"))
    if not (RATE > 0 and math.isfinite(RATE)):
        fail("REFILL_PER_SEC must be > 0")
except ValueError:
    fail("REFILL_PER_SEC must be a number")

lock = threading.Lock()
buckets = {}  # key -> [tokens, last time]
stats = {"allowed": 0, "denied": 0}


def take(key):
    now = time.monotonic()
    with lock:
        b = buckets.get(key)
        if b is None:
            b = buckets[key] = [float(CAP), now]
        b[0] = min(CAP, b[0] + (now - b[1]) * RATE)
        b[1] = now
        if b[0] >= 1:
            b[0] -= 1
            stats["allowed"] += 1
            return True, int(math.floor(b[0])), 0
        stats["denied"] += 1
        wait = max(1, math.ceil((1 - b[0]) / RATE))
        return False, 0, wait


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj, headers=()):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        path = self.path.split("?", 1)[0]
        if path == "/check":
            if method != "POST":
                return self.send(405, {"error": "method not allowed"})
            try:
                obj = json.loads(body)
            except ValueError:
                return self.send(400, {"error": "invalid JSON"})
            if not isinstance(obj, dict):
                return self.send(400, {"error": "invalid JSON"})
            key = obj.get("key")
            if not isinstance(key, str) or key == "":
                return self.send(400, {"error": "invalid key"})
            ok, rem, wait = take(key)
            if ok:
                return self.send(200, {"allowed": True, "remaining": rem})
            return self.send(429, {"allowed": False, "remaining": 0, "retry_after": wait},
                             [("Retry-After", str(wait))])
        if path == "/stats":
            if method != "GET":
                return self.send(405, {"error": "method not allowed"})
            with lock:
                return self.send(200, {"allowed": stats["allowed"], "denied": stats["denied"], "keys": len(buckets)})
        self.send(404, {"error": "not found"})

    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")
    def do_DELETE(self): self.handle_any("DELETE")


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
