import sys, os, time, json, hmac, hashlib, threading, socketserver
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
SECRET = "s3cr3t-ключ"

# ---- the check's own target: flaky, sometimes slow ----
lock = threading.Lock()
hits = []          # dicts: t, id, path, body, sig, ctype, status
tries = {}         # event id -> attempts seen


class Target(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        t = time.time()
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        eid = self.headers.get("X-Event-Id")
        with lock:
            tries[eid] = k = tries.get(eid, 0) + 1
        try:
            p = json.loads(body).get("payload") or {}
            mode = p.get("mode", "ok") if isinstance(p, dict) else "ok"
        except Exception:
            mode = "ok"
        h = {"t": t, "id": eid, "path": self.path, "body": body, "k": k,
             "sig": self.headers.get("X-Signature"), "ctype": self.headers.get("Content-Type")}
        status, delay = 200, 0
        if mode == "fail":
            status = 500
        elif mode.startswith("fail"):
            status = 500 if k <= int(mode[4:]) else 200
        elif mode == "hang":
            delay = 1.0
        elif mode == "hang1" and k == 1:
            delay = 0.8
        elif mode == "flaky":
            delay = 0.02
            status = 503 if (p["n"] % 3 == 0 and k == 1) else 204
        h["status"] = status
        with lock:
            hits.append(h)
        time.sleep(delay)
        try:
            self.send_response(status)
            self.send_header("Content-Length", "0")
            self.end_headers()
        except OSError:
            pass  # the relay gave up waiting


class TargetServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 256

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


tport = free_port()
tsrv = TargetServer(("127.0.0.1", tport), Target)
threading.Thread(target=tsrv.serve_forever, daemon=True).start()

s = c.server(env={"TARGET_URL": f"http://127.0.0.1:{tport}/hooks/in", "SECRET": SECRET,
                  "MAX_ATTEMPTS": "4", "BACKOFF_MS": "50", "ATTEMPT_TIMEOUT_MS": "300"})


def send(body):
    raw = body if isinstance(body, str) else json.dumps(body)
    r = s.post("/events", raw.encode(), headers={"Content-Type": "application/json"})
    assert r.status == 202, r
    return r.json()["id"]


def status(i):
    return s.get(f"/events/{i}").json()


def wait_final(ids, timeout=8):
    """Statuses of ids once none is pending (or at the timeout)."""
    end = time.time() + timeout
    final = {}
    while time.time() < end:
        for i in ids:
            if i not in final:
                x = status(i)
                if x["status"] != "pending":
                    final[i] = x
                else:
                    break  # poll the rest later
        if len(final) == len(ids):
            break
        time.sleep(0.05)
    return [final.get(i) or status(i) for i in ids]


def hits_for(i):
    with lock:
        return [h for h in hits if h["id"] == str(i)]


def sign(body):
    return "sha256=" + hmac.new(SECRET.encode(), body, hashlib.sha256).hexdigest()


@c.test("delivers the exact body, signed")
def _():
    raw = '{"source": "shop",   "payload": {"mode": "ok", "note": "Grüße \\"x\\" 🎉; DROP TABLE"}}'
    t0 = time.time()
    i = send(raw)
    st = wait_final([i], 3)[0]
    hs = hits_for(i)
    ok = i == 1 and st == {"id": 1, "source": "shop", "status": "delivered", "attempts": 1} and len(hs) == 1
    h = hs[0] if hs else {}
    ok = ok and h.get("body") == raw.encode() and h.get("sig") == sign(raw.encode())
    ok = ok and h.get("path") == "/hooks/in" and (h.get("ctype") or "").startswith("application/json")
    ok = ok and h.get("t", 99e9) - t0 < 0.5
    return ok, (st, hs)


@c.test("retries with backoff until success")
def _():
    i = send({"source": "retry", "payload": {"mode": "fail2"}})
    st = wait_final([i], 4)[0]
    hs = hits_for(i)
    ts = [h["t"] for h in hs]
    ok = st["status"] == "delivered" and st["attempts"] == 3 and len(hs) == 3
    ok = ok and ts[1] - ts[0] >= 0.045 and ts[2] - ts[1] >= 0.095 and ts[2] - ts[0] < 1.0
    ok = ok and all(h["sig"] == sign(h["body"]) and h["body"] == hs[0]["body"] for h in hs)
    return ok, (st, [round(t - ts[0], 3) for t in ts])


@c.test("gives up after MAX_ATTEMPTS")
def _():
    i = send({"source": "doomed", "payload": {"mode": "fail"}})
    st = wait_final([i], 4)[0]
    time.sleep(0.5)
    hs = hits_for(i)
    ts = [h["t"] for h in hs]
    ok = st == {"id": i, "source": "doomed", "status": "failed", "attempts": 4} and len(hs) == 4
    ok = ok and ts[3] - ts[2] >= 0.195
    return ok, (st, len(hs))


@c.test("a slow answer counts as a failed attempt")
def _():
    i = send({"source": "slowpoke", "payload": {"mode": "hang1"}})
    st = wait_final([i], 4)[0]
    return st["status"] == "delivered" and st["attempts"] == 2 and len(hits_for(i)) == 2, st


@c.test("same source is delivered in order, one at a time")
def _():
    a = send({"source": "orders", "payload": {"mode": "fail2"}})
    b = send({"source": "orders", "payload": {"mode": "fail"}})
    d = send({"source": "orders", "payload": {"mode": "ok"}})
    sts = wait_final([a, b, d], 6)
    with lock:
        seq = [int(h["id"]) for h in hits if h["id"] in (str(a), str(b), str(d))]
    got = [(x["status"], x["attempts"]) for x in sts]
    ok = got == [("delivered", 3), ("failed", 4), ("delivered", 1)] and seq == [a] * 3 + [b] * 4 + [d]
    return ok, (got, seq)


@c.test("a stuck source does not block other sources")
def _():
    stuck = send({"source": "stuck", "payload": {"mode": "hang"}})
    time.sleep(0.1)
    free = send({"source": "free", "payload": {"mode": "ok"}})
    t0 = time.time()
    while time.time() - t0 < 1.0 and status(free)["status"] == "pending":
        time.sleep(0.02)
    took = time.time() - t0
    mid = status(stuck)
    fin = wait_final([stuck], 5)[0]
    ok = status(free)["status"] == "delivered" and took < 0.5 and mid["status"] == "pending"
    ok = ok and fin["status"] == "failed" and fin["attempts"] == 4
    return ok, (took, mid, fin)


@c.test("many sources in parallel, flaky target")
def _():
    sources = [f"src-{k}" for k in range(8)]
    order = {}

    def feed(src):
        order[src] = [send({"source": src, "payload": {"mode": "flaky", "n": n}}) for n in range(15)]
    with ThreadPoolExecutor(8) as ex:
        list(ex.map(feed, sources))
    all_ids = [i for src in sources for i in order[src]]
    sts = {x["id"]: x for x in wait_final(all_ids, 15)}
    problems = []
    for src in sources:
        ids = order[src]
        with lock:
            ok_seq = [int(h["id"]) for h in hits if h["id"] in {str(i) for i in ids} and h["status"] == 204]
            all_seq = [int(h["id"]) for h in hits if h["id"] in {str(i) for i in ids}]
        if ok_seq != ids or all_seq != sorted(all_seq):
            problems.append((src, ok_seq, all_seq))
        for n, i in enumerate(ids):
            want = ("delivered", 2 if n % 3 == 0 else 1)
            if (sts[i]["status"], sts[i]["attempts"], sts[i]["source"]) != want + (src,):
                problems.append(sts[i])
    with lock:
        bad_sig = [h for h in hits if h["id"] in {str(i) for i in all_ids} and h["sig"] != sign(h["body"])]
    ids_sorted = sorted(all_ids)
    ok = not problems and not bad_sig and ids_sorted == list(range(ids_sorted[0], ids_sorted[0] + 120))
    return ok, (problems[:5], len(bad_sig))


@c.test("payload can be any JSON value")
def _():
    raws = ['{"source":"kinds","payload":"just text"}', '{"source":"kinds","payload":[1,2,{"a":null}]}',
            '{"source":"kinds","payload":null,"extra":true}', '{"payload":42,"source":"kinds"}']
    ids = [send(r) for r in raws]
    sts = wait_final(ids, 3)
    bodies = [(hits_for(i) or [{}])[0].get("body") for i in ids]
    ok = all(x["status"] == "delivered" for x in sts) and bodies == [r.encode() for r in raws]
    return ok, (sts, bodies)


@c.test("large event body")
def _():
    raw = json.dumps({"source": "big", "payload": {"blob": "ü" * 300000, "mode": "ok"}}, ensure_ascii=False)
    i = send(raw)
    st = wait_final([i], 4)[0]
    hs = hits_for(i)
    ok = st["status"] == "delivered" and len(hs) == 1 and hs[0]["body"] == raw.encode()
    return ok and hs[0]["sig"] == sign(raw.encode()), (st, len(hs))


@c.test("invalid events are 400")
def _():
    bodies = ["not json", "[1]", '{"payload": 1}', '{"source": "", "payload": 1}',
              '{"source": 7, "payload": 1}', '{"source": "x"}']
    before = len(hits)
    rs = [s.post("/events", b, headers={"Content-Type": "application/json"}) for b in bodies]
    nxt = send({"source": "after", "payload": None})
    time.sleep(0.3)
    ok = all(r.status == 400 and "error" in (r.json() or {}) for r in rs)
    ok = ok and status(nxt)["status"] == "delivered" and len(hits) == before + 1
    return ok, (rs, nxt)


@c.test("unknown event and path are 404")
def _():
    rs = [s.get("/events/99999"), s.get("/events/abc"), s.get("/elsewhere")]
    return all(r.status == 404 for r in rs), rs


@c.test("connection refused counts as failed attempts")
def _():
    dead = c.server(env={"TARGET_URL": f"http://127.0.0.1:{free_port()}/x", "SECRET": "k",
                         "MAX_ATTEMPTS": "3", "BACKOFF_MS": "20", "ATTEMPT_TIMEOUT_MS": "300"})
    r = dead.post("/events", {"source": "a", "payload": {}})
    i = r.json()["id"]
    end = time.time() + 4
    while time.time() < end and dead.get(f"/events/{i}").json()["status"] == "pending":
        time.sleep(0.03)
    st = dead.get(f"/events/{i}").json()
    return st == {"id": 1, "source": "a", "status": "failed", "attempts": 3}, st


c.finish()
tsrv.shutdown()
