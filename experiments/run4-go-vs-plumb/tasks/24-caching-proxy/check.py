import sys, os, time, json, threading, socketserver, http.client
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
import checklib
from checklib import Checks, free_port
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer



def _close_req(self, method, path, body=None, headers=None, timeout=10, _req=checklib.Server.req):
    # Ask the server to close each connection, so thousands of requests do
    # not leave the client's ephemeral ports in TIME_WAIT.
    h = dict(headers or {})
    h.setdefault("Connection", "close")
    return _req(self, method, path, body, h, timeout)


checklib.Server.req = _close_req

c = Checks()

# ---- the check's own upstream; counts requests per raw path+query ----
lock = threading.Lock()
counts = {}
BIN = bytes(range(256)) * 8192  # 2 MiB
JSON_BODY = json.dumps({"name": "Grüße \"Ü\" 🎉", "n": 1}, ensure_ascii=False).encode()


class Upstream(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def answer(self, status, body, ctype="text/plain; charset=utf-8"):
        try:
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        with lock:
            n = counts[self.path] = counts.get(self.path, 0) + 1
        path = self.path.split("?", 1)[0]
        if path == "/api/item":
            return self.answer(200, ("item " + self.path.split("?", 1)[1]).encode())
        if path == "/api/slow":
            time.sleep(0.3)
            return self.answer(200, b"slow " + self.path.encode())
        if path == "/api/slowfail":
            time.sleep(0.3)
            return self.answer(503, b"down")
        if path == "/api/hang":
            time.sleep(2.0)
            return self.answer(200, b"late")
        if path == "/api/flaky":
            return self.answer(500, b"oops") if n == 1 else self.answer(200, b"recovered")
        if path == "/api/missing":
            return self.answer(404, b"nope")
        if path == "/api/bin":
            return self.answer(200, BIN, "application/octet-stream")
        if path == "/api/json":
            return self.answer(200, JSON_BODY, "application/json")
        if path.startswith("/api/echo/"):
            return self.answer(200, self.path.encode())
        self.answer(418, b"unexpected " + self.path.encode())

    def do_POST(self):
        with lock:
            counts["POST " + self.path] = counts.get("POST " + self.path, 0) + 1
        self.answer(200, b"posted")


class UpstreamServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 256

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


uport = free_port()
usrv = UpstreamServer(("127.0.0.1", uport), Upstream)
threading.Thread(target=usrv.serve_forever, daemon=True).start()
UP = f"http://127.0.0.1:{uport}/api"


def hits(p):
    with lock:
        return counts.get("/api" + p, 0)


p = c.server(env={"UPSTREAM": UP, "CACHE_TTL_MS": "1500", "CACHE_MAX_ENTRIES": "3", "UPSTREAM_TIMEOUT_MS": "500"})


def get(path, srv=None, method="GET"):
    """Raw GET: (status, body bytes, headers)."""
    srv = srv or p
    conn = http.client.HTTPConnection("127.0.0.1", srv.port, timeout=10)
    conn.request(method, path, headers={"Connection": "close"})
    r = conn.getresponse()
    body = r.read()
    conn.close()
    return r.status, body, {k.lower(): v for k, v in r.getheaders()}


def parallel(fn, items, workers=32):
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(fn, items))


@c.test("miss then hit")
def _():
    a, b = get("/item?id=1"), get("/item?id=1")
    ok = a[:2] == (200, b"item id=1") and a[2].get("x-cache") == "MISS" and a[2].get("content-type", "").startswith("text/plain")
    ok = ok and b[:2] == (200, b"item id=1") and b[2].get("x-cache") == "HIT" and hits("/item?id=1") == 1
    return ok, (a, b)


@c.test("query string is part of the key")
def _():
    a = get("/item?id=2")
    ok = a[:2] == (200, b"item id=2") and a[2].get("x-cache") == "MISS" and hits("/item?id=2") == 1
    return ok, a


@c.test("path and query are forwarded exactly")
def _():
    path = "/echo/a%20b/%C3%BC%2Fx?q=x%2By&z=%22%27&e"
    a = get(path)
    return a[:2] == (200, ("/api" + path).encode()), a


@c.test("content type and unicode body pass through")
def _():
    a, b = get("/json"), get("/json")
    ok = a[:2] == (200, JSON_BODY) and a[2].get("content-type") == "application/json"
    ok = ok and b[:2] == (200, JSON_BODY) and b[2].get("content-type") == "application/json" and b[2].get("x-cache") == "HIT"
    return ok, (a[2], b[2])


@c.test("binary body is byte-exact")
def _():
    a, b = get("/bin"), get("/bin")
    ok = a[0] == 200 and a[1] == BIN and b[1] == BIN and b[2].get("x-cache") == "HIT" and hits("/bin") == 1
    return ok, (a[0], len(a[1]), len(b[1]), hits("/bin"))


@c.test("4xx passes through and is not cached")
def _():
    a, b = get("/missing"), get("/missing")
    ok = a[:2] == (404, b"nope") and b[:2] == (404, b"nope") and hits("/missing") == 2
    ok = ok and a[2].get("x-cache") == "MISS" and b[2].get("x-cache") == "MISS"
    return ok, (a, b)


@c.test("5xx becomes 502 and is not cached")
def _():
    a, b, d = get("/flaky"), get("/flaky"), get("/flaky")
    ok = a[0] == 502 and json.loads(a[1]) == {"error": "upstream error", "status": 500}
    ok = ok and b[:2] == (200, b"recovered") and b[2].get("x-cache") == "MISS"
    ok = ok and d[2].get("x-cache") == "HIT" and hits("/flaky") == 2
    return ok, (a, b, d)


@c.test("upstream timeout is 504")
def _():
    t0 = time.time()
    a = get("/hang")
    took = time.time() - t0
    ok = a[0] == 504 and json.loads(a[1]) == {"error": "upstream timeout"} and 0.4 <= took < 1.5
    return ok, (a, took)


@c.test("parallel identical requests are coalesced")
def _():
    rs = parallel(lambda i: get("/slow?k=1"), range(20), workers=20)
    ok = all(r[:2] == (200, b"slow /api/slow?k=1") for r in rs) and hits("/slow?k=1") == 1
    ok = ok and [r[2].get("x-cache") for r in rs].count("MISS") == 1
    return ok, ([r[:2] for r in rs][:3], hits("/slow?k=1"))


@c.test("coalesced errors are shared, then retried")
def _():
    rs = parallel(lambda i: get("/slowfail"), range(10), workers=10)
    first = hits("/slowfail")
    again = get("/slowfail")
    ok = all(r[0] == 502 and json.loads(r[1]) == {"error": "upstream error", "status": 503} for r in rs)
    ok = ok and first == 1 and again[0] == 502 and hits("/slowfail") == 2
    return ok, (rs[:2], first, hits("/slowfail"))


@c.test("entries expire after the TTL")
def _():
    a = get("/item?id=ttl")
    time.sleep(0.5)
    b = get("/item?id=ttl")
    time.sleep(1.3)
    d = get("/item?id=ttl")
    xs = [r[2].get("x-cache") for r in (a, b, d)]
    return xs == ["MISS", "HIT", "MISS"] and hits("/item?id=ttl") == 2, (xs, hits("/item?id=ttl"))


@c.test("least recently used entry is evicted")
def _():
    for k in "abc":
        get(f"/item?lru={k}")
    get("/item?lru=a")           # a is now the most recent
    get("/item?lru=d")           # evicts b
    seq = [(k, get(f"/item?lru={k}")[2].get("x-cache")) for k in "acdb"]
    n = {k: hits(f"/item?lru={k}") for k in "abcd"}
    ok = seq == [("a", "HIT"), ("c", "HIT"), ("d", "HIT"), ("b", "MISS")] and n == {"a": 1, "b": 2, "c": 1, "d": 1}
    return ok, (seq, n)


@c.test("other methods are 405 and not forwarded")
def _():
    a = get("/item?id=1", method="POST")
    b = get("/item?id=1", method="DELETE")
    with lock:
        posted = counts.get("POST /api/item?id=1", 0)
    return a[0] == 405 and b[0] == 405 and json.loads(a[1]) == {"error": "method not allowed"} and posted == 0, (a, b)


@c.test("unreachable upstream is 502")
def _():
    dead = c.server(env={"UPSTREAM": f"http://127.0.0.1:{free_port()}/api", "UPSTREAM_TIMEOUT_MS": "1000"})
    a = get("/item?id=1", dead)
    b = get("/item?id=1", dead)
    ok = a[0] == 502 and json.loads(a[1]) == {"error": "upstream unavailable"} and b[0] == 502
    return ok and a[2].get("x-cache") == "MISS" and b[2].get("x-cache") == "MISS", (a, b)


@c.test("parallel load: one upstream request per key")
def _():
    big = c.server(env={"UPSTREAM": UP, "CACHE_TTL_MS": "60000", "CACHE_MAX_ENTRIES": "100"})
    paths = [f"/slow?load={i % 10}" for i in range(400)]
    rs = parallel(lambda q: (q, get(q, big)), paths)
    ok = all(r[:2] == (200, ("slow /api" + q).encode()) for q, r in rs)
    n = [hits(f"/slow?load={i}") for i in range(10)]
    misses = sum(1 for _, r in rs if r[2].get("x-cache") == "MISS")
    return ok and n == [1] * 10 and misses == 10, (n, misses)


c.finish()
usrv.shutdown()
