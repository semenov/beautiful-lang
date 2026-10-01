#!/usr/bin/env python3
import json, os, re, signal, socketserver, sys, threading, time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WORKERS = int(os.environ.get("WORKERS", "2"))
GRACE = int(os.environ.get("SHUTDOWN_GRACE_MS", "5000")) / 1000

cond = threading.Condition()
jobs = {}          # id -> dict(status, result, ms, input, stop: Event)
queue = deque()
next_id = [1]
stopping = [False]


def worker():
    while True:
        with cond:
            while not stopping[0] and not queue:
                cond.wait()
            if stopping[0]:
                return
            j = jobs[queue.popleft()]
            j["status"] = "running"
        cancelled = j["stop"].wait(j["ms"] / 1000)
        with cond:
            if not cancelled and j["status"] == "running":
                j["status"] = "done"
                j["result"] = j["input"][::-1]
            cond.notify_all()


def view(i):
    j = jobs[i]
    return {"id": i, "status": j["status"], "result": j["result"]}


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

    def find(self, s):
        if not re.fullmatch(r"[1-9][0-9]*", s):
            return None
        i = int(s)
        return i if i in jobs else None

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/stats":
            with cond:
                st = {"queued": 0, "running": 0, "done": 0, "cancelled": 0}
                for j in jobs.values():
                    st[j["status"]] += 1
            return self.send(200, st)
        m = re.fullmatch(r"/jobs/([^/]+)", path)
        if m:
            with cond:
                i = self.find(m.group(1))
                if i is not None:
                    return self.send(200, view(i))
        self.send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        path = self.path.split("?", 1)[0]
        if path == "/jobs":
            try:
                obj = json.loads(raw)
            except ValueError:
                obj = None
            if not isinstance(obj, dict):
                return self.send(400, {"error": "invalid JSON"})
            ms, inp = obj.get("ms"), obj.get("input")
            if not (isinstance(ms, int) and not isinstance(ms, bool) and 0 <= ms <= 60000):
                return self.send(400, {"error": "invalid ms"})
            if not isinstance(inp, str):
                return self.send(400, {"error": "invalid input"})
            with cond:
                i = next_id[0]
                next_id[0] += 1
                jobs[i] = {"status": "queued", "result": None, "ms": ms, "input": inp, "stop": threading.Event()}
                queue.append(i)
                cond.notify_all()
            return self.send(202, {"id": i})
        m = re.fullmatch(r"/jobs/([^/]+)/cancel", path)
        if m:
            with cond:
                i = self.find(m.group(1))
                if i is not None:
                    j = jobs[i]
                    if j["status"] in ("done", "cancelled"):
                        return self.send(409, {"error": f"job is {j['status']}"})
                    if j["status"] == "queued":
                        queue.remove(i)
                    j["status"] = "cancelled"
                    j["stop"].set()
                    cond.notify_all()
                    return self.send(200, view(i))
        self.send(404, {"error": "not found"})


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


term = threading.Event()
signal.signal(signal.SIGTERM, lambda *a: term.set())
srv = Server(("127.0.0.1", int(os.environ["PORT"])), H)
for _ in range(WORKERS):
    threading.Thread(target=worker, daemon=True).start()
threading.Thread(target=srv.serve_forever, args=(0.05,), daemon=True).start()
while not term.wait(0.05):
    pass

t0 = time.monotonic()
with cond:
    stopping[0] = True
    running = [i for i, j in jobs.items() if j["status"] == "running"]
    queued = len(queue)
    cond.notify_all()
srv.shutdown()
srv.server_close()
with cond:
    while any(jobs[i]["status"] == "running" for i in running):
        left = GRACE - (time.monotonic() - t0)
        if left <= 0:
            break
        cond.wait(left)
    done = sum(1 for i in running if jobs[i]["status"] == "done")
    unfinished = sum(1 for i in running if jobs[i]["status"] == "running")
sys.stdout.write(f"shutdown: completed {done}, abandoned {queued + len(running) - done}\n")
sys.stdout.flush()
os._exit(1 if unfinished else 0)
