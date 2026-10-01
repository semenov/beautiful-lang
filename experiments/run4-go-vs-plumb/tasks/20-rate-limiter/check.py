import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
import checklib
from checklib import Checks
from concurrent.futures import ThreadPoolExecutor



def _close_req(self, method, path, body=None, headers=None, timeout=10, _req=checklib.Server.req):
    # Ask the server to close each connection, so thousands of requests do
    # not leave the client's ephemeral ports in TIME_WAIT.
    h = dict(headers or {})
    h.setdefault("Connection", "close")
    return _req(self, method, path, body, h, timeout)


checklib.Server.req = _close_req

c = Checks()


def check(s, key):
    return s.post("/check", {"key": key})


def parallel(fn, items, workers=64):
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(fn, items))


# Slow refill: one token per 5 seconds.
s = c.server(env={"CAPACITY": "5", "REFILL_PER_SEC": "0.2"})


@c.test("first checks count down remaining")
def _():
    rs = [check(s, "alpha") for _ in range(5)]
    got = [(r.status, r.json()) for r in rs]
    want = [(200, {"allowed": True, "remaining": n}) for n in (4, 3, 2, 1, 0)]
    return got == want, got


@c.test("empty bucket is denied with Retry-After")
def _():
    r = check(s, "alpha")
    j = r.json() or {}
    ok = (r.status == 429 and j.get("allowed") is False and j.get("remaining") == 0
          and j.get("retry_after") == 5 and r.headers.get("retry-after") == "5")
    return ok, (r, r.headers)


@c.test("denied check takes no token")
def _():
    r = check(s, "alpha")
    return r.status == 429 and (r.json() or {}).get("retry_after") == 5, r


@c.test("keys are independent and case-sensitive")
def _():
    got = [(k, check(s, k).status) for k in ("Alpha", "alpha ", "beta")]
    return all(st == 200 for _, st in got), got


@c.test("unicode and quote keys")
def _():
    keys = ["ключ-🔑", "a\"b'c; DROP TABLE keys;--", "ключ-🔑 "]
    got = []
    for k in keys:
        got.append((k, [check(s, k).json() for _ in range(2)]))
    ok = all(rs == [{"allowed": True, "remaining": 4}, {"allowed": True, "remaining": 3}] for _, rs in got)
    return ok, got


@c.test("stats after sequential checks")
def _():
    r = s.get("/stats")
    want = {"allowed": 5 + 3 + 6, "denied": 2, "keys": 7}
    return r.status == 200 and r.json() == want, r


@c.test("bad requests are 400 and not counted")
def _():
    bad = ["not json", "[1,2]", "{}", '{"key": 5}', '{"key": ""}', '{"key": null}']
    got = [s.post("/check", b, headers={"Content-Type": "application/json"}) for b in bad]
    st = s.get("/stats").json()
    ok = all(r.status == 400 and "error" in (r.json() or {}) for r in got)
    ok = ok and st == {"allowed": 14, "denied": 2, "keys": 7}
    return ok, (got, st)


@c.test("wrong method and unknown path")
def _():
    a, b, d = s.get("/check"), s.post("/stats", {}), s.get("/nope")
    return (a.status, b.status, d.status) == (405, 405, 404), (a, b, d)


# Essentially no refill: one token per 1000 seconds.
p = c.server(env={"CAPACITY": "50", "REFILL_PER_SEC": "0.001"})


@c.test("parallel burst on one key: exactly capacity allowed")
def _():
    rs = parallel(lambda i: check(p, "hot"), range(200))
    allowed = [r for r in rs if r.status == 200]
    denied = [r for r in rs if r.status == 429]
    rem = sorted(r.json()["remaining"] for r in allowed)
    ok = len(allowed) == 50 and len(denied) == 150 and rem == list(range(50))
    ok = ok and all(r.headers.get("retry-after") == str(r.json()["retry_after"]) for r in denied)
    return ok, (len(allowed), len(denied), rem[:60])


@c.test("parallel checks on many keys are independent")
def _():
    keys = [f"user-{i}" for i in range(10) for _ in range(60)]
    rs = parallel(lambda k: (k, check(p, k).status), keys)
    per = {}
    for k, st in rs:
        per.setdefault(k, [0, 0])[0 if st == 200 else 1] += 1
    ok = all(v == [50, 10] for v in per.values()) and len(per) == 10
    return ok, per


@c.test("stats add up exactly after parallel load")
def _():
    r = p.get("/stats")
    return r.status == 200 and r.json() == {"allowed": 550, "denied": 250, "keys": 11}, r


# Fast refill: one token per 250 ms.
f = c.server(env={"CAPACITY": "2", "REFILL_PER_SEC": "4"})


@c.test("tokens refill over time")
def _():
    a = [check(f, "r").status for _ in range(3)]
    third = check(f, "r")
    time.sleep(0.4)
    b = check(f, "r").status
    ok = a == [200, 200, 429] and third.headers.get("retry-after") == "1" and b == 200
    return ok, (a, third, b)


@c.test("refill never exceeds capacity")
def _():
    time.sleep(1.2)
    got = [check(f, "r").json() for _ in range(3)]
    want = [{"allowed": True, "remaining": 1}, {"allowed": True, "remaining": 0}]
    return got[:2] == want and got[2].get("allowed") is False, got


@c.test("invalid configuration exits with code 2")
def _():
    out = []
    for env in ({"CAPACITY": "0"}, {"CAPACITY": "abc"}, {"REFILL_PER_SEC": "-1"}, {"REFILL_PER_SEC": "0"}):
        r = c.run([], env=dict(env, PORT="1"), timeout=5)
        out.append((env, r.code, r.err[:80]))
    return all(code == 2 and err.startswith("error:") for _, code, err in out), out


c.finish()
