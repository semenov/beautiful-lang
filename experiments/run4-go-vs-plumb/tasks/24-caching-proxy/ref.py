#!/usr/bin/env python3
import http.client, json, os, socket, socketserver, threading, time
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

UP = urlsplit(os.environ["UPSTREAM"])
PREFIX = UP.path.rstrip("/")
TTL = int(os.environ.get("CACHE_TTL_MS", "60000")) / 1000
MAX = int(os.environ.get("CACHE_MAX_ENTRIES", "1000"))
TIMEOUT = int(os.environ.get("UPSTREAM_TIMEOUT_MS", "5000")) / 1000

lock = threading.Lock()
cache = OrderedDict()   # key -> (fresh_until, answer)
inflight = {}           # key -> [Event, answer]

JSON = "application/json"


def err(status, obj):
    return (status, json.dumps(obj).encode(), JSON)


def fetch(target):
    """One upstream request; returns (status, body, content type, cacheable)."""
    try:
        conn = http.client.HTTPConnection(UP.hostname, UP.port or 80, timeout=TIMEOUT)
        deadline = time.monotonic() + TIMEOUT
        conn.request("GET", PREFIX + target)
        r = conn.getresponse()
        body = r.read()
        conn.close()
        if time.monotonic() > deadline:
            return err(504, {"error": "upstream timeout"}) + (False,)
    except socket.timeout:
        return err(504, {"error": "upstream timeout"}) + (False,)
    except OSError:
        return err(502, {"error": "upstream unavailable"}) + (False,)
    if r.status >= 500:
        return err(502, {"error": "upstream error", "status": r.status}) + (False,)
    return (r.status, body, r.getheader("Content-Type"), r.status == 200)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, answer, xcache=None):
        status, body, ctype = answer[:3]
        self.send_response(status)
        if ctype:
            self.send_header("Content-Type", ctype)
        if xcache:
            self.send_header("X-Cache", xcache)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def not_allowed(self):
        self.send(err(405, {"error": "method not allowed"}))

    do_POST = do_PUT = do_DELETE = do_PATCH = do_HEAD = not_allowed

    def do_GET(self):
        key = self.path
        with lock:
            hit = cache.get(key)
            if hit and hit[0] > time.monotonic():
                cache.move_to_end(key)
                return self.send(hit[1], "HIT")
            waiting = inflight.get(key)
            if waiting is None:
                inflight[key] = mine = [threading.Event(), None]
        if waiting is not None:
            waiting[0].wait()
            return self.send(waiting[1], "HIT")
        answer = fetch(key)
        with lock:
            if answer[3]:
                cache[key] = (time.monotonic() + TTL, answer)
                cache.move_to_end(key)
                while len(cache) > MAX:
                    cache.popitem(last=False)
            mine[1] = answer
            del inflight[key]
        mine[0].set()
        self.send(answer, "MISS")


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
