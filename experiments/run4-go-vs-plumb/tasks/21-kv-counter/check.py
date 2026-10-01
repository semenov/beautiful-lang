import sys, os, json
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
s = c.server()

MAX = 9223372036854775807


def put(k, v):
    return s.req("PUT", "/kv/" + quote(k, safe=""), {"value": v})


def get(k):
    return s.get("/kv/" + quote(k, safe=""))


def incr(k, by=None):
    return s.post("/incr/" + quote(k, safe="") + ("" if by is None else "?by=" + by))


def cas(k, body):
    return s.post("/cas/" + quote(k, safe=""), body)


def parallel(fn, items, workers=64):
    with ThreadPoolExecutor(workers) as ex:
        return list(ex.map(fn, items))


@c.test("put then get a string verbatim")
def _():
    v = "it's \"quoted\"; DROP TABLE kv; -- 100% \\ ok"
    a, b = put("note", v), get("note")
    return (a.status, a.json(), b.status, b.json()) == (200, {"key": "note", "value": v}, 200, {"key": "note", "value": v}), (a, b)


@c.test("unicode key and value")
def _():
    a = put("café x/ü", "Grüße 🌍")
    b = s.get("/kv/caf%C3%A9%20x%2F%C3%BC")
    return a.status == 200 and b.status == 200 and b.json() == {"key": "café x/ü", "value": "Grüße 🌍"}, (a, b)


@c.test("integers stay integers")
def _():
    a, b = put("n", MAX), get("n")
    raw = b.body.replace(" ", "")
    return a.status == 200 and b.json() == {"key": "n", "value": MAX} and '"value":9223372036854775807' in raw, (a, b)


@c.test("overwrite and delete")
def _():
    put("tmp", "one")
    a = put("tmp", 2)
    b = get("tmp").json()
    d1 = s.req("DELETE", "/kv/tmp")
    g = get("tmp")
    d2 = s.req("DELETE", "/kv/tmp")
    ok = a.json() == {"key": "tmp", "value": 2} and b == {"key": "tmp", "value": 2}
    ok = ok and d1.status == 204 and g.status == 404 and d2.status == 404
    return ok, (a, b, d1, g, d2)


@c.test("missing key is 404")
def _():
    r = get("nobody")
    return r.status == 404 and r.json() == {"error": "not found"}, r


@c.test("incr creates, adds and subtracts")
def _():
    got = [incr("hits").json(), incr("hits", "5").json(), incr("hits", "-10").json()]
    want = [{"key": "hits", "value": v} for v in (1, 6, -4)]
    return got == want, got


@c.test("incr on a non-integer is 409 and changes nothing")
def _():
    put("s", "abc")
    put("s5", "5")
    a, b = incr("s"), incr("s5", "2")
    ok = a.status == 409 and (a.json() or {}).get("value") == "abc"
    ok = ok and b.status == 409 and (b.json() or {}).get("value") == "5"
    ok = ok and get("s5").json()["value"] == "5"
    return ok, (a, b)


@c.test("bad by is 400")
def _():
    rs = [s.post("/incr/hits?by=" + b) for b in ("abc", "1.5", "", "9223372036854775808")]
    ok = all(r.status == 400 for r in rs) and get("hits").json()["value"] == -4
    return ok, rs


@c.test("incr overflow is 409 and changes nothing")
def _():
    put("big", MAX - 1)
    a = incr("big")
    b = incr("big")
    put("small", -MAX - 1)
    d = incr("small", "-1")
    ok = a.json() == {"key": "big", "value": MAX} and b.status == 409 and (b.json() or {}).get("value") == MAX
    ok = ok and get("big").json()["value"] == MAX and d.status == 409 and get("small").json()["value"] == -MAX - 1
    return ok, (a, b, d)


@c.test("invalid values are 400")
def _():
    bodies = ['{"value": 1.5}', '{"value": true}', '{"value": null}', '{"value": [1]}', '{}',
              'not json', '{"value": 9223372036854775808}', '"x"']
    rs = [s.req("PUT", "/kv/bad", b, headers={"Content-Type": "application/json"}) for b in bodies]
    return all(r.status == 400 for r in rs) and get("bad").status == 404, rs


@c.test("cas succeeds only on an equal value")
def _():
    put("c", 10)
    a = cas("c", {"expected": 10, "value": 11})
    b = cas("c", {"expected": 10, "value": 12})
    d = cas("c", {"expected": "11", "value": 12})
    ok = a.status == 200 and a.json() == {"key": "c", "value": 11}
    ok = ok and b.status == 409 and (b.json() or {}).get("value") == 11
    ok = ok and d.status == 409 and get("c").json()["value"] == 11
    return ok, (a, b, d)


@c.test("cas with expected null")
def _():
    a = cas("fresh", {"expected": None, "value": "first"})
    b = cas("fresh", {"expected": None, "value": "second"})
    d = cas("absent", {"expected": "x", "value": "y"})
    e = cas("absent", {"value": "y"})
    ok = a.status == 200 and a.json() == {"key": "fresh", "value": "first"}
    ok = ok and b.status == 409 and (b.json() or {}).get("value") == "first"
    ok = ok and d.status == 409 and "value" in (d.json() or {}) and d.json()["value"] is None
    ok = ok and e.status == 400 and get("absent").status == 404
    return ok, (a, b, d, e)


@c.test("large value round trip")
def _():
    v = "ж" * 100000 + "end"
    a, b = put("large", v), get("large")
    return a.status == 200 and b.json() == {"key": "large", "value": v}, (a.status, b.status, len(b.body))


@c.test("empty key, unknown path, wrong method")
def _():
    a = s.req("PUT", "/kv/", {"value": 1})
    b = s.get("/other")
    d = s.req("PUT", "/incr/x")
    e = s.post("/snapshot")
    return (a.status, b.status, d.status, e.status) == (400, 404, 405, 405), (a, b, d, e)


KEYS = ["p0", "p1", "p2", "p3"]
JOBS = [(KEYS[i % 4], 1 + i % 3) for i in range(4000)]
SUMS = {k: sum(b for kk, b in JOBS if kk == k) for k in KEYS}


@c.test("parallel increments are exact")
def _():
    keys, jobs, want = KEYS, JOBS, SUMS
    rs = parallel(lambda kb: incr(kb[0], str(kb[1])).status, jobs)
    got = {k: get(k).json()["value"] for k in keys}
    return all(st == 200 for st in rs) and got == want, (got, want, set(rs))


@c.test("parallel cas: exactly one winner")
def _():
    put("lock", 0)
    rs = parallel(lambda i: cas("lock", {"expected": 0, "value": i + 1}).status, range(200))
    v = get("lock").json()["value"]
    return rs.count(200) == 1 and rs.count(409) == 199 and 1 <= v <= 200, (rs.count(200), rs.count(409), v)


@c.test("parallel cas loops lose no update")
def _():
    put("ctr", 0)

    def bump(_):
        for _ in range(10):
            while True:
                cur = get("ctr").json()["value"]
                if cas("ctr", {"expected": cur, "value": cur + 1}).status == 200:
                    break
    parallel(bump, range(20), workers=20)
    return get("ctr").json()["value"] == 200, get("ctr")


@c.test("snapshot has every key")
def _():
    r = s.get("/snapshot")
    j = r.json() or {}
    items = j.get("items", {})
    want = {"note": "it's \"quoted\"; DROP TABLE kv; -- 100% \\ ok", "café x/ü": "Grüße 🌍", "n": MAX,
            "hits": -4, "s": "abc", "s5": "5", "big": MAX, "small": -MAX - 1, "c": 11, "fresh": "first",
            "lock": items.get("lock"), "ctr": 200, **SUMS}
    want["large"] = "ж" * 100000 + "end"
    ok = r.status == 200 and j.get("count") == len(want) and items == want
    return ok, (r.status, j.get("count"), sorted(items))


c.finish()
