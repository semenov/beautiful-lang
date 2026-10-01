import sys, os, time, signal, subprocess
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


def submit(s, ms, inp="x"):
    r = s.post("/jobs", {"ms": ms, "input": inp})
    assert r.status == 202, r
    return r.json()["id"]


def job(s, i):
    return s.get(f"/jobs/{i}").json()


def wait_until(fn, timeout=5.0, step=0.01):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


s = c.server(env={"WORKERS": "3"})


@c.test("job runs and returns reversed input")
def _():
    i = submit(s, 50, "héllo 👋")
    first = job(s, i)
    done = wait_until(lambda: job(s, i)["status"] == "done", 3)
    j = job(s, i)
    ok = first["status"] in ("queued", "running") and first["result"] is None
    return ok and done and j == {"id": i, "status": "done", "result": "👋 olléh"}, (first, j)


@c.test("ids are 1, 2, 3 in order")
def _():
    a, b = submit(s, 0, ""), submit(s, 0, "ab")
    wait_until(lambda: job(s, b)["status"] == "done", 3)
    return (a, b) == (2, 3) and job(s, a)["result"] == "" and job(s, b)["result"] == "ba", (a, b)


@c.test("at most WORKERS run at once, in submission order")
def _():
    t0 = time.time()
    ids = [submit(s, 300, str(n)) for n in range(10)]
    time.sleep(0.1)
    early = [job(s, i)["status"] for i in ids[:4]]
    peak = 0
    while True:
        st = s.get("/stats").json()
        peak = max(peak, st["running"])
        if st["done"] >= 13:
            break
        if time.time() - t0 > 6:
            break
        time.sleep(0.02)
    took = time.time() - t0
    ok = early == ["running"] * 3 + ["queued"] and peak == 3 and 1.15 <= took <= 3.0
    ok = ok and all(job(s, i)["result"] == str(n)[::-1] for n, i in enumerate(ids))
    return ok, (early, peak, took)


@c.test("invalid submissions are 400")
def _():
    bodies = ['{"ms": -1, "input": "a"}', '{"ms": 60001, "input": "a"}', '{"ms": "100", "input": "a"}',
              '{"ms": 1.5, "input": "a"}', '{"input": "a"}', '{"ms": 1}', '{"ms": 1, "input": 5}',
              'not json', '[1]']
    rs = [s.post("/jobs", b, headers={"Content-Type": "application/json"}) for b in bodies]
    st = s.get("/stats").json()
    ok = all(r.status == 400 and "error" in (r.json() or {}) for r in rs)
    return ok and sum(st.values()) == 13, (rs, st)


@c.test("unknown jobs and paths are 404")
def _():
    rs = [s.get("/jobs/999"), s.get("/jobs/abc"), s.get("/jobs/0"), s.post("/jobs/999/cancel"), s.get("/nope")]
    return all(r.status == 404 for r in rs), rs


@c.test("parallel submissions get distinct consecutive ids")
def _():
    with ThreadPoolExecutor(64) as ex:
        ids = list(ex.map(lambda n: (n, submit(s, 0, f"in-{n}")), range(200)))
    nums = sorted(i for _, i in ids)
    wait_until(lambda: s.get("/stats").json()["done"] == 213, 10, 0.05)
    st = s.get("/stats").json()
    ok = nums == list(range(14, 214)) and st == {"queued": 0, "running": 0, "done": 213, "cancelled": 0}
    ok = ok and all(job(s, i)["result"] == f"in-{n}"[::-1] for n, i in ids[::17])
    return ok, (nums[:5], nums[-5:], st)


w = c.server(env={"WORKERS": "1"})


@c.test("cancel a queued job")
def _():
    a = submit(w, 400, "a")
    b = submit(w, 50, "b")
    r = w.post(f"/jobs/{b}/cancel")
    wait_until(lambda: job(w, a)["status"] == "done", 3)
    time.sleep(0.2)
    ok = r.status == 200 and r.json() == {"id": b, "status": "cancelled", "result": None}
    ok = ok and job(w, b) == {"id": b, "status": "cancelled", "result": None}
    st = w.get("/stats").json()
    return ok and st == {"queued": 0, "running": 0, "done": 1, "cancelled": 1}, (r, job(w, b), st)


@c.test("cancel a running job frees the worker")
def _():
    a = submit(w, 10000, "slow")
    b = submit(w, 50, "next")
    wait_until(lambda: job(w, a)["status"] == "running", 2)
    t0 = time.time()
    r = w.post(f"/jobs/{a}/cancel")
    done = wait_until(lambda: job(w, b)["status"] == "done", 3, 0.005)
    took = time.time() - t0
    ok = r.status == 200 and (r.json() or {}).get("status") == "cancelled" and done and took < 0.45
    ok = ok and job(w, a) == {"id": a, "status": "cancelled", "result": None}
    return ok, (r, took, job(w, a), job(w, b))


@c.test("cancel done or cancelled job is 409")
def _():
    r1, r2 = w.post("/jobs/1/cancel"), w.post("/jobs/2/cancel")
    ok = r1.status == 409 and r2.status == 409 and "error" in (r1.json() or {})
    return ok and job(w, 1)["status"] == "done", (r1, r2)


@c.test("stats count every status")
def _():
    ids = [submit(w, 1500, "z") for _ in range(3)]
    wait_until(lambda: job(w, ids[0])["status"] == "running", 2)
    w.post(f"/jobs/{ids[2]}/cancel")
    st = w.get("/stats").json()
    w.post(f"/jobs/{ids[0]}/cancel")
    w.post(f"/jobs/{ids[1]}/cancel")
    return st == {"queued": 1, "running": 1, "done": 2, "cancelled": 3}, st


def server_output(srv):
    srv.log.flush()
    return (c.work / f"server-{srv.port}.log").read_text()


@c.test("SIGTERM lets running jobs finish")
def _():
    g = c.server(env={"WORKERS": "2", "SHUTDOWN_GRACE_MS": "3000"})
    ids = [submit(g, 800, "r") for _ in range(2)] + [submit(g, 100, "q") for _ in range(3)]
    wait_until(lambda: g.get("/stats").json()["running"] == 2, 2)
    time.sleep(0.2)
    t0 = time.time()
    g.proc.send_signal(signal.SIGTERM)
    try:
        code = g.proc.wait(5)
    except subprocess.TimeoutExpired:
        return False, "still running 5 s after SIGTERM"
    took = time.time() - t0
    out = server_output(g)
    ok = code == 0 and 0.4 <= took <= 1.6 and "shutdown: completed 2, abandoned 3" in out
    return ok, (code, took, out[-300:])


@c.test("SIGTERM gives up after the grace period")
def _():
    g = c.server(env={"WORKERS": "1", "SHUTDOWN_GRACE_MS": "300"})
    submit(g, 5000, "long")
    submit(g, 100, "q")
    wait_until(lambda: g.get("/stats").json()["running"] == 1, 2)
    t0 = time.time()
    g.proc.send_signal(signal.SIGTERM)
    try:
        code = g.proc.wait(5)
    except subprocess.TimeoutExpired:
        return False, "still running 5 s after SIGTERM"
    took = time.time() - t0
    out = server_output(g)
    ok = code == 1 and 0.25 <= took <= 1.3 and "shutdown: completed 0, abandoned 2" in out
    return ok, (code, took, out[-300:])


c.finish()
