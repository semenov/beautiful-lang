#!/usr/bin/env python3
import hashlib, hmac, http.client, json, os, re, socketserver, threading, time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

TARGET = urlsplit(os.environ["TARGET_URL"])
TPATH = (TARGET.path or "/") + (("?" + TARGET.query) if TARGET.query else "")
SECRET = os.environ["SECRET"].encode()
MAX_ATTEMPTS = int(os.environ.get("MAX_ATTEMPTS", "5"))
BACKOFF = int(os.environ.get("BACKOFF_MS", "1000")) / 1000
TIMEOUT = int(os.environ.get("ATTEMPT_TIMEOUT_MS", "5000")) / 1000

lock = threading.Lock()
events = {}        # id -> {source, status, attempts, body}
queues = {}        # source -> deque of ids
active = set()     # sources with a running delivery thread
next_id = [1]


def attempt(i, body):
    sig = "sha256=" + hmac.new(SECRET, body, hashlib.sha256).hexdigest()
    deadline = time.monotonic() + TIMEOUT
    try:
        conn = http.client.HTTPConnection(TARGET.hostname, TARGET.port or 80, timeout=TIMEOUT)
        conn.request("POST", TPATH, body=body, headers={
            "Content-Type": "application/json", "X-Event-Id": str(i), "X-Signature": sig})
        r = conn.getresponse()
        r.read()
        conn.close()
        return 200 <= r.status < 300 and time.monotonic() <= deadline
    except OSError:
        return False


def deliver_source(src):
    while True:
        with lock:
            q = queues[src]
            if not q:
                active.discard(src)
                return
            i = q.popleft()
            ev = events[i]
        for k in range(1, MAX_ATTEMPTS + 1):
            with lock:
                ev["attempts"] = k
            if attempt(i, ev["body"]):
                with lock:
                    ev["status"] = "delivered"
                break
            if k == MAX_ATTEMPTS:
                with lock:
                    ev["status"] = "failed"
            else:
                time.sleep(BACKOFF * 2 ** (k - 1))


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

    def do_GET(self):
        m = re.fullmatch(r"/events/([1-9][0-9]*)", self.path.split("?", 1)[0])
        if m:
            with lock:
                ev = events.get(int(m.group(1)))
                if ev:
                    return self.send(200, {"id": int(m.group(1)), "source": ev["source"],
                                           "status": ev["status"], "attempts": ev["attempts"]})
        self.send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if self.path.split("?", 1)[0] != "/events":
            return self.send(404, {"error": "not found"})
        try:
            obj = json.loads(raw)
        except ValueError:
            obj = None
        if not isinstance(obj, dict):
            return self.send(400, {"error": "invalid JSON"})
        src = obj.get("source")
        if not isinstance(src, str) or src == "":
            return self.send(400, {"error": "invalid source"})
        if "payload" not in obj:
            return self.send(400, {"error": "missing payload"})
        with lock:
            i = next_id[0]
            next_id[0] += 1
            events[i] = {"source": src, "status": "pending", "attempts": 0, "body": raw}
            queues.setdefault(src, deque()).append(i)
            start = src not in active
            active.add(src)
        if start:
            threading.Thread(target=deliver_source, args=(src,), daemon=True).start()
        self.send(202, {"id": i})


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
