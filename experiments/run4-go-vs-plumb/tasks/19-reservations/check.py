import sys, os, random
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
for p in list(c.work.glob("res.db*")):
    p.unlink()
env = {"DB_PATH": str(c.work / "res.db")}
s = c.server(env)


def event(name, cap):
    return s.post("/events", {"name": name, "capacity": cap})


def reserve(eid, seats, key=None):
    h = {"Idempotency-Key": key} if key is not None else {}
    return s.post(f"/events/{eid}/reservations", {"seats": seats}, headers=h)


def remaining(eid):
    return s.get(f"/events/{eid}").json()["remaining"]


def J(r):
    j = r.json()
    return j if isinstance(j, dict) else {}


@c.test("create event")
def _():
    r = event("Concert", 100)
    j = J(r)
    return r.status == 201 and isinstance(j.get("id"), int) and j["id"] > 0 and \
        j == {"id": j["id"], "name": "Concert", "capacity": 100, "remaining": 100}, r


@c.test("get event; unknown or malformed ids are 404")
def _():
    e = J(event("Gig 🎸 'quoted'; DROP TABLE events;--", 7))
    g = s.get(f"/events/{e.get('id')}")
    rs = [s.get("/events/999999"), s.get("/events/abc"), s.get("/events/0"), s.get("/events/-3"),
          s.get("/reservations/999999"), s.get("/reservations/x"), s.req("DELETE", "/reservations/999999")]
    return (g.status == 200 and g.json() == e and e["name"] == "Gig 🎸 'quoted'; DROP TABLE events;--"
            and all(r.status == 404 and r.json() == {"error": "not found"} for r in rs)), (g, rs)


@c.test("event validation")
def _():
    cases = [({"capacity": 5}, "invalid name"), ({"name": "", "capacity": 5}, "invalid name"),
             ({"name": 7, "capacity": 5}, "invalid name"), ({"name": "x"}, "invalid capacity"),
             ({"name": "x", "capacity": 0}, "invalid capacity"), ({"name": "x", "capacity": -1}, "invalid capacity"),
             ({"name": "x", "capacity": "10"}, "invalid capacity"), ({"name": "x", "capacity": 1.5}, "invalid capacity"),
             ({"name": "x", "capacity": 100001}, "invalid capacity"), ({"name": "x", "capacity": None}, "invalid capacity")]
    bad = [(b, r) for b, m in cases for r in [s.post("/events", b)] if not (r.status == 400 and r.json() == {"error": m})]
    rj = [s.post("/events", b, headers={"Content-Type": "application/json"}) for b in ("{", "[1]", "")]
    ok_max = event("big", 100000).status == 201
    return not bad and ok_max and all(r.status == 400 and r.json() == {"error": "invalid JSON"} for r in rj), (bad, rj)


@c.test("reserve: 201, remaining goes down, reservation readable")
def _():
    eid = J(event("R1", 10))["id"]
    r = reserve(eid, 3)
    j = J(r)
    g = s.get(f"/reservations/{j.get('id')}")
    return (r.status == 201 and j == {"id": j["id"], "event_id": eid, "seats": 3, "status": "active"}
            and remaining(eid) == 7 and g.status == 200 and g.json() == j), (r, g)


@c.test("all or nothing: exact fill, then 409")
def _():
    eid = J(event("R2", 5))["id"]
    r1, r2, r3 = reserve(eid, 6), reserve(eid, 5), reserve(eid, 1)
    return (r1.status == 409 and r1.json() == {"error": "not enough seats"} and r2.status == 201
            and r3.status == 409 and remaining(eid) == 0), (r1, r2, r3)


@c.test("invalid seats are 400, before the event lookup")
def _():
    eid = J(event("R3", 5))["id"]
    bodies = [{"seats": 0}, {"seats": -2}, {"seats": "2"}, {"seats": 1.5}, {}, {"seats": None}, {"seats": True}]
    rs = [s.post(f"/events/{eid}/reservations", b) for b in bodies] + [s.post("/events/999999/reservations", {"seats": 0})]
    u = reserve(999999, 1)
    rj = s.post(f"/events/{eid}/reservations", "nope", headers={"Content-Type": "application/json"})
    return (all(r.status == 400 and r.json() == {"error": "invalid seats"} for r in rs) and remaining(eid) == 5
            and u.status == 404 and u.json() == {"error": "not found"}
            and rj.status == 400 and rj.json() == {"error": "invalid JSON"}), (rs, u, rj)


@c.test("cancel returns seats; second cancel is 409")
def _():
    eid = J(event("R4", 4))["id"]
    rid = J(reserve(eid, 4))["id"]
    d = s.req("DELETE", f"/reservations/{rid}")
    d2 = s.req("DELETE", f"/reservations/{rid}")
    g = s.get(f"/reservations/{rid}")
    return (d.status == 200 and d.json() == {"id": rid, "event_id": eid, "seats": 4, "status": "cancelled"}
            and d2.status == 409 and d2.json() == {"error": "already cancelled"} and g.json()["status"] == "cancelled"
            and remaining(eid) == 4 and reserve(eid, 4).status == 201), (d, d2, g)


@c.test("idempotency key returns the same reservation once")
def _():
    eid = J(event("R5", 10))["id"]
    a, b = reserve(eid, 2, "key-A"), reserve(eid, 2, "key-A")
    other = reserve(eid, 2, "key-B")
    return (a.status == 201 and b.status == 201 and a.json() == b.json() and J(other).get("id") != J(a).get("id")
            and remaining(eid) == 6), (a, b, other)


@c.test("idempotency key reused with other seats or event is 422")
def _():
    e1, e2 = J(event("R6a", 10))["id"], J(event("R6b", 10))["id"]
    a = reserve(e1, 1, "key-reuse")
    r1, r2 = reserve(e1, 2, "key-reuse"), reserve(e2, 1, "key-reuse")
    return (a.status == 201 and all(r.status == 422 and r.json() == {"error": "idempotency key reused"} for r in (r1, r2))
            and remaining(e1) == 9 and remaining(e2) == 10), (a, r1, r2)


@c.test("failed request does not record its key; cancelled state is returned on replay")
def _():
    eid = J(event("R7", 3))["id"]
    f = reserve(eid, 5, "key-fail")
    ok = reserve(eid, 3, "key-fail")
    rid = J(ok).get("id")
    s.req("DELETE", f"/reservations/{rid}")
    replay = reserve(eid, 3, "key-fail")
    return (f.status == 409 and ok.status == 201 and replay.status == 201
            and replay.json() == {"id": rid, "event_id": eid, "seats": 3, "status": "cancelled"} and remaining(eid) == 3), (f, ok, replay)


@c.test("80 concurrent 1-seat requests on 50 seats: exactly 50 succeed")
def _():
    eid = J(event("Rush", 50))["id"]
    with ThreadPoolExecutor(40) as ex:
        rs = list(ex.map(lambda i: reserve(eid, 1), range(80)))
    st = [r.status for r in rs]
    ids = {J(r).get("id") for r in rs if r.status == 201}
    return st.count(201) == 50 and st.count(409) == 30 and len(ids) == 50 and remaining(eid) == 0, st


@c.test("concurrent mixed sizes: seats taken equal capacity minus remaining")
def _():
    eid = J(event("Mixed", 100))["id"]
    sizes = [random.Random(i).randint(1, 4) for i in range(70)]
    with ThreadPoolExecutor(35) as ex:
        rs = list(ex.map(lambda n: reserve(eid, n), sizes))
    taken = sum(n for n, r in zip(sizes, rs) if r.status == 201)
    rem = remaining(eid)
    return (all(r.status in (201, 409) for r in rs) and taken == 100 - rem and 0 <= rem < 4), (taken, rem)


@c.test("concurrent requests with one key create one reservation")
def _():
    eid = J(event("Same key", 30))["id"]
    with ThreadPoolExecutor(20) as ex:
        rs = list(ex.map(lambda i: reserve(eid, 2, "key-concurrent"), range(20)))
    ids = {J(r).get("id") for r in rs}
    return all(r.status == 201 for r in rs) and len(ids) == 1 and remaining(eid) == 28, ([r.status for r in rs], ids)


@c.test("concurrent cancels and reservations keep counts exact")
def _():
    eid = J(event("Churn", 20))["id"]
    rid = J(reserve(eid, 5))["id"]
    with ThreadPoolExecutor(20) as ex:
        cancels = list(ex.map(lambda i: s.req("DELETE", f"/reservations/{rid}"), range(10)))
        res = list(ex.map(lambda i: reserve(eid, 1), range(25)))
    st = [r.status for r in cancels]
    got = sum(r.status == 201 for r in res)
    return st.count(200) == 1 and st.count(409) == 9 and got == 20 and remaining(eid) == 0, (st, got)


@c.test("data and idempotency keys survive a restart")
def _():
    global s
    eid = J(event("Persist", 9))["id"]
    a = reserve(eid, 4, "key-persist")
    s.stop()
    s = c.server(env)
    b = reserve(eid, 4, "key-persist")
    g = s.get(f"/reservations/{J(a).get('id')}")
    return a.status == 201 and b.json() == a.json() and g.json() == a.json() and remaining(eid) == 5, (a, b, g)


@c.test("404 for unknown paths, 405 for other methods")
def _():
    r404 = [s.get("/"), s.get("/event"), s.get("/events/1/other"), s.post("/reservations", {})]
    r405 = [s.get("/events"), s.req("DELETE", "/events/1"), s.get("/events/1/reservations"), s.post("/reservations/1", {})]
    return (all(r.status == 404 and r.json() == {"error": "not found"} for r in r404)
            and all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in r405)), (r404, r405)


c.finish()
