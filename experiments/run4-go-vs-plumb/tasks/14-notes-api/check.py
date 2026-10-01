import sys, os, json
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import quote
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
db = c.work / "notes.db"
for p in c.work.glob("notes.db*"):
    p.unlink()
env = {"DB_PATH": str(db)}
s = c.server(env)


def note(r):
    return r.json() if isinstance(r.json(), dict) else {}


@c.test("create returns 201 and the note")
def _():
    r = s.post("/notes", {"title": "Shopping", "body": "milk"})
    n = note(r)
    ok = (r.status == 201 and isinstance(n.get("id"), int) and n["id"] > 0
          and n == {"id": n["id"], "title": "Shopping", "body": "milk", "tag": None}
          and r.headers.get("content-type", "").startswith("application/json"))
    return ok, r


@c.test("get returns the stored note; defaults applied")
def _():
    n = note(s.post("/notes", {"title": "Only title", "extra": 5}))
    r = s.get(f"/notes/{n.get('id')}")
    return r.status == 200 and r.json() == {"id": n["id"], "title": "Only title", "body": "", "tag": None}, r


@c.test("unknown and malformed ids are 404")
def _():
    rs = [s.get("/notes/999999"), s.get("/notes/abc"), s.get("/notes/0"), s.get("/notes/-1"),
          s.req("PATCH", "/notes/999999", {"body": "x"}), s.req("DELETE", "/notes/999999")]
    return all(r.status == 404 and r.json() == {"error": "not found"} for r in rs), rs


@c.test("validation errors on create")
def _():
    cases = [({"body": "x"}, "title"), ({"title": ""}, "title"), ({"title": 5}, "title"),
             ({"title": None}, "title"), ({"title": "t", "body": 3}, "body"), ({"title": "t", "tag": 7}, "tag")]
    before = s.get("/notes").json()
    out = []
    for body, field in cases:
        r = s.post("/notes", body)
        out.append(r)
        if not (r.status == 400 and r.json() == {"error": f"invalid field: {field}"}):
            return False, (body, r)
    return s.get("/notes").json() == before, "notes changed after rejected creates"


@c.test("invalid JSON is 400")
def _():
    rs = [s.post("/notes", "{not json", headers={"Content-Type": "application/json"}),
          s.post("/notes", "[1,2]", headers={"Content-Type": "application/json"}),
          s.post("/notes", "", headers={"Content-Type": "application/json"})]
    return all(r.status == 400 and r.json() == {"error": "invalid JSON"} for r in rs), rs


@c.test("title length counts code points (200 ok, 201 rejected)")
def _():
    r1 = s.post("/notes", {"title": "é" * 200})
    r2 = s.post("/notes", {"title": "é" * 201})
    return r1.status == 201 and note(r1).get("title") == "é" * 200 and r2.status == 400 \
        and r2.json() == {"error": "invalid field: title"}, (r1, r2)


@c.test("unicode, quotes and SQL-like text stored verbatim")
def _():
    t = "Robert'); DROP TABLE notes;-- \"x\""
    b = "日本語 🎉 50% off_now \\ back"
    n = note(s.post("/notes", {"title": t, "body": b, "tag": "it's"}))
    r = s.get(f"/notes/{n.get('id')}")
    return r.status == 200 and r.json() == {"id": n["id"], "title": t, "body": b, "tag": "it's"}, r


@c.test("patch is partial")
def _():
    n = note(s.post("/notes", {"title": "A", "body": "B", "tag": "T"}))
    r = s.req("PATCH", f"/notes/{n['id']}", {"body": "B2"})
    g = s.get(f"/notes/{n['id']}")
    want = {"id": n["id"], "title": "A", "body": "B2", "tag": "T"}
    return r.status == 200 and r.json() == want and g.json() == want, (r, g)


@c.test("patch tag null clears it; title/body null rejected")
def _():
    n = note(s.post("/notes", {"title": "A", "body": "B", "tag": "T"}))
    r = s.req("PATCH", f"/notes/{n['id']}", {"tag": None})
    r2 = s.req("PATCH", f"/notes/{n['id']}", {"title": None})
    r3 = s.req("PATCH", f"/notes/{n['id']}", {"body": None, "tag": "Z"})
    r4 = s.req("PATCH", f"/notes/{n['id']}", "nope", headers={"Content-Type": "application/json"})
    g = s.get(f"/notes/{n['id']}")
    return (r.status == 200 and r.json()["tag"] is None
            and r2.status == 400 and r2.json() == {"error": "invalid field: title"}
            and r3.status == 400 and r3.json() == {"error": "invalid field: body"}
            and r4.status == 400 and r4.json() == {"error": "invalid JSON"}
            and g.json() == {"id": n["id"], "title": "A", "body": "B", "tag": None}), (r, r2, r3, r4, g)


@c.test("delete then get is 404, second delete is 404")
def _():
    n = note(s.post("/notes", {"title": "gone"}))
    r1 = s.req("DELETE", f"/notes/{n['id']}")
    r2 = s.get(f"/notes/{n['id']}")
    r3 = s.req("DELETE", f"/notes/{n['id']}")
    return r1.status == 204 and r1.body == "" and r2.status == 404 and r3.status == 404, (r1, r2, r3)


@c.test("ids are never reused after delete")
def _():
    a = note(s.post("/notes", {"title": "x"}))
    s.req("DELETE", f"/notes/{a['id']}")
    b = note(s.post("/notes", {"title": "y"}))
    s.req("DELETE", f"/notes/{b['id']}")
    return b.get("id", 0) > a["id"], (a, b)


@c.test("list returns all notes ordered by id")
def _():
    r = s.get("/notes")
    ns = r.json()["notes"]
    ids = [n["id"] for n in ns]
    return r.status == 200 and ids == sorted(ids) and len(ids) == len(set(ids)) and len(ids) >= 6, r


@c.test("search is a literal, case-sensitive substring of title or body")
def _():
    mk = lambda t, b: note(s.post("/notes", {"title": t, "body": b}))["id"]
    a = mk("sale 50% today", "")
    b = mk("sale 50 today", "x")
    c1 = mk("plain", "has under_score")
    d = mk("plain", "hasXunderXscore")
    e = mk("Quote", "she said \"hi\" and it's ok")
    f = mk("CaseTest", "lower")
    q = lambda t: [n["id"] for n in s.get("/notes?q=" + quote(t, safe="")).json()["notes"]]
    res = {"%": q("e 50%"), "_": q("r_s"), "quote": q("it's"), "dq": q("\"hi\""),
           "case": q("casetest"), "case2": q("CaseTest"), "body": q("lowe")}
    ok = (res["%"] == [a] and res["_"] == [c1] and res["quote"] == [e] and res["dq"] == [e]
          and res["case"] == [] and res["case2"] == [f] and f in res["body"])
    return ok, res


@c.test("search: plus is a space, empty q lists all")
def _():
    a = note(s.post("/notes", {"title": "two words here"}))["id"]
    r = s.get("/notes?q=two+words")
    allr = s.get("/notes?q=")
    return [n["id"] for n in r.json()["notes"]] == [a] and allr.json() == s.get("/notes").json(), (r, allr)


@c.test("405 for wrong method, 404 for unknown path")
def _():
    rs405 = [s.req("PUT", "/notes/1", {"title": "x"}), s.req("DELETE", "/notes"), s.req("PATCH", "/notes", {})]
    rs404 = [s.get("/"), s.get("/note"), s.get("/notes/1/extra")]
    return (all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in rs405)
            and all(r.status == 404 and r.json() == {"error": "not found"} for r in rs404)), (rs405, rs404)


@c.test("parallel creates all get distinct ids")
def _():
    before = len(s.get("/notes").json()["notes"])
    with ThreadPoolExecutor(25) as ex:
        rs = list(ex.map(lambda i: s.post("/notes", {"title": f"par {i}"}), range(60)))
    ids = [note(r).get("id") for r in rs]
    after = s.get("/notes").json()["notes"]
    titles = {n["title"] for n in after}
    return (all(r.status == 201 for r in rs) and len(set(ids)) == 60 and len(after) == before + 60
            and all(f"par {i}" in titles for i in range(60))), [r for r in rs if r.status != 201][:3]


@c.test("data survives a restart")
def _():
    global s
    want = s.get("/notes").json()
    s.stop()
    s = c.server(env)
    got = s.get("/notes").json()
    n = note(s.post("/notes", {"title": "after restart"}))
    return got == want and n.get("id", 0) > max(x["id"] for x in want["notes"]), (want == got, n)


c.finish()
