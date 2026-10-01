import sys, os, random
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
import checklib
from checklib import Checks
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import quote



def _close_req(self, method, path, body=None, headers=None, timeout=10, _req=checklib.Server.req):
    # Ask the server to close each connection, so thousands of requests do
    # not leave the client's ephemeral ports in TIME_WAIT.
    h = dict(headers or {})
    h.setdefault("Connection", "close")
    return _req(self, method, path, body, h, timeout)


checklib.Server.req = _close_req

c = Checks()


def agg(s, name):
    r = s.get("/aggregate?name=" + quote(name, safe=""))
    assert r.status == 200, r
    return r.json()


def post(s, body, timeout=10):
    return s.post("/metrics", body, timeout=timeout)


def full(name, count, total, lo, hi, p50, p99):
    return {"name": name, "count": count, "sum": total, "min": lo, "max": hi, "p50": p50, "p99": p99}


def pts(name, values, ts):
    return [{"name": name, "value": v, "ts": ts} for v in values]


# ---- a long window: aggregates, validation, concurrency ----
s = c.server(env={"WINDOW_MS": "60000"})


@c.test("aggregates of a small batch")
def _():
    vals = [12, 7, 3.5, 20, 7]
    r = post(s, [{"name": "latency", "value": v, "ts": 1000 + i} for i, v in enumerate(vals)])
    a = agg(s, "latency")
    return r.json() == {"accepted": 5, "dropped": 0} and a == full("latency", 5, 49.5, 3.5, 20, 7, 20), (r, a)


@c.test("nearest-rank percentiles of 1..100")
def _():
    vals = list(range(1, 101))
    random.Random(1).shuffle(vals)
    post(s, pts("hundred", vals, 2000))
    a = agg(s, "hundred")
    return a == full("hundred", 100, 5050, 1, 100, 50, 99), a


@c.test("nearest-rank percentiles of small sets")
def _():
    post(s, pts("ten", [10, 9, 8, 7, 6, 5, 4, 3, 2, 1], 2000))
    post(s, pts("two", [2, 1], 2000))
    post(s, {"name": "one", "value": 42.25, "ts": 2000})
    got = [agg(s, n) for n in ("ten", "two", "one")]
    want = [full("ten", 10, 55, 1, 10, 5, 10), full("two", 2, 3, 1, 2, 1, 2), full("one", 1, 42.25, 42.25, 42.25, 42.25, 42.25)]
    return got == want, got


@c.test("negative and fractional values")
def _():
    post(s, pts("temp", [0.25, -2.5, 0], 2001))
    a = agg(s, "temp")
    return a == full("temp", 3, -2.25, -2.5, 0.25, 0, 0.25), a


@c.test("unknown name has empty aggregates")
def _():
    a = agg(s, "nothing-here")
    return a == {"name": "nothing-here", "count": 0, "sum": 0, "min": None, "max": None, "p50": None, "p99": None}, a


@c.test("unicode names, compared exactly")
def _():
    n1, n2 = 'cpu.Ü "core" 🌡; DROP TABLE m', 'CPU.ü "core" 🌡; DROP TABLE m'
    post(s, [{"name": n1, "value": 1, "ts": 2002}, {"name": n1, "value": 3, "ts": 2002}, {"name": n2, "value": 100, "ts": 2002}])
    a, b = agg(s, n1), agg(s, n2)
    return a == full(n1, 2, 4, 1, 3, 1, 3) and b == full(n2, 1, 100, 100, 100, 100, 100), (a, b)


@c.test("invalid samples reject the whole request")
def _():
    before = s.get("/stats").json()
    bodies = ['not json', '"x"', '[1]', '{"value": 1, "ts": 1}', '{"name": "", "value": 1, "ts": 1}',
              '{"name": 5, "value": 1, "ts": 1}', '{"name": "v", "value": "5", "ts": 1}',
              '{"name": "v", "value": true, "ts": 1}', '{"name": "v", "value": null, "ts": 1}',
              '{"name": "v", "ts": 1}', '{"name": "v", "value": 1}', '{"name": "v", "value": 1, "ts": 1.5}',
              '{"name": "v", "value": 1, "ts": "10"}', '{"name": "v", "value": 1, "ts": -1}',
              '[{"name": "v", "value": 1, "ts": 3000}, {"name": "v", "value": 1}]']
    rs = [s.post("/metrics", b, headers={"Content-Type": "application/json"}) for b in bodies]
    after = s.get("/stats").json()
    ok = all(r.status == 400 and "error" in (r.json() or {}) for r in rs) and before == after
    return ok and agg(s, "v")["count"] == 0, ([r.status for r in rs], before, after)


@c.test("bad paths and methods")
def _():
    rs = [s.get("/aggregate"), s.get("/nope"), s.get("/metrics"), s.post("/stats", {})]
    return [r.status for r in rs] == [400, 404, 405, 405], rs


@c.test("parallel ingestion keeps exact counts")
def _():
    singles = [{"name": f"par{i % 4}", "value": i % 7, "ts": 3000 + i % 500} for i in range(3000)]
    batches = [pts(f"par{b % 4}", list(range(50)), 3000 + b) for b in range(80)]
    with ThreadPoolExecutor(64) as ex:
        rs = list(ex.map(lambda body: post(s, body).json(), singles + batches))
    acc = sum(r["accepted"] for r in rs)
    want = {}
    for p in singles + [p for b in batches for p in b]:
        w = want.setdefault(p["name"], [0, 0])
        w[0] += 1
        w[1] += p["value"]
    got = {n: agg(s, n) for n in want}
    ok = acc == 7000 and all(got[n]["count"] == w[0] and got[n]["sum"] == w[1] for n, w in want.items())
    ok = ok and all(got[n]["min"] == 0 and got[n]["max"] == 49 for n in want)
    return ok, (acc, {n: (g["count"], g["sum"]) for n, g in got.items()}, want)


@c.test("stats totals")
def _():
    r = s.get("/stats")
    return r.status == 200 and r.json() == {"accepted": 5 + 100 + 13 + 3 + 3 + 7000, "dropped": 0, "now": 3499}, r


@c.test("large batch")
def _():
    vals = list(range(1, 50001))
    random.Random(2).shuffle(vals)
    r = post(s, pts("big", vals, 3499), timeout=30)
    a = agg(s, "big")
    return r.json() == {"accepted": 50000, "dropped": 0} and a == full("big", 50000, 1250025000, 1, 50000, 25000, 49500), a


# ---- a short window: sliding, late samples, the shared clock ----
w = c.server(env={"WINDOW_MS": "1000"})


@c.test("samples leave the window as time advances")
def _():
    post(w, [{"name": "w", "value": 1, "ts": 1000}, {"name": "w", "value": 2, "ts": 1500}, {"name": "w", "value": 3, "ts": 1999}])
    a = agg(w, "w")
    post(w, {"name": "w", "value": 4, "ts": 2500})
    b = agg(w, "w")
    return a == full("w", 3, 6, 1, 3, 2, 3) and b == full("w", 2, 7, 3, 4, 3, 4), (a, b)


@c.test("late samples are dropped at the boundary")
def _():
    r1 = post(w, {"name": "w", "value": 100, "ts": 1500})
    r2 = post(w, {"name": "w", "value": 5, "ts": 1501})
    a = agg(w, "w")
    ok = r1.json() == {"accepted": 0, "dropped": 1} and r2.json() == {"accepted": 1, "dropped": 0}
    return ok and a == full("w", 3, 12, 3, 5, 4, 5), (r1, r2, a)


@c.test("one clock for all names")
def _():
    post(w, {"name": "other", "value": 1, "ts": 4000})
    a = agg(w, "w")
    return a == {"name": "w", "count": 0, "sum": 0, "min": None, "max": None, "p50": None, "p99": None}, a


@c.test("a batch is processed in order")
def _():
    r = post(w, [{"name": "o", "value": 1, "ts": 5000}, {"name": "o", "value": 2, "ts": 4000},
                 {"name": "o", "value": 3, "ts": 4001}, {"name": "o", "value": 4, "ts": 3000}])
    a = agg(w, "o")
    return r.json() == {"accepted": 2, "dropped": 2} and a == full("o", 2, 4, 1, 3, 1, 3), (r, a)


@c.test("stats count dropped samples")
def _():
    r = w.get("/stats")
    return r.json() == {"accepted": 3 + 1 + 1 + 1 + 2, "dropped": 1 + 2, "now": 5000}, r


c.finish()
