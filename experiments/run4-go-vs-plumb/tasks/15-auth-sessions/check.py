import sys, os, re, time
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
for p in list(c.work.glob("auth*.db*")):
    p.unlink()
db = c.work / "auth.db"
env = {"DB_PATH": str(db), "SESSION_TTL_SECONDS": "3600"}
s = c.server(env)

PW = "s3cret-pass"
UPW = "pässwörd🔑 ünïcode"
passwords_used = [PW, UPW]


def reg(u, p):
    return s.post("/register", {"username": u, "password": p})


def login(u, p):
    return s.post("/login", {"username": u, "password": p})


def me(tok):
    return s.get("/me", headers={"Authorization": f"Bearer {tok}"})


def token(r):
    j = r.json()
    return j.get("token") if isinstance(j, dict) else None


@c.test("register returns 201 with the username")
def _():
    r = reg("Alice", PW)
    return r.status == 201 and r.json() == {"username": "Alice"}, r


@c.test("duplicate username in another case is 409")
def _():
    rs = [reg("alice", "another-pass"), reg("ALICE", PW)]
    return all(r.status == 409 and r.json() == {"error": "username taken"} for r in rs), rs


@c.test("invalid usernames are 400")
def _():
    bad = [{"password": PW}, {"username": 5, "password": PW}, {"username": "ab", "password": PW},
           {"username": "a" * 33, "password": PW}, {"username": "bad-name", "password": PW},
           {"username": "ünïcode", "password": PW}, {"username": "with space", "password": PW},
           {"username": "x'; DROP", "password": "short"}]
    rs = [s.post("/register", b) for b in bad]
    return all(r.status == 400 and r.json() == {"error": "invalid username"} for r in rs), rs


@c.test("invalid passwords are 400; length counts code points")
def _():
    bad = [{"username": "pwuser1"}, {"username": "pwuser1", "password": 12345678},
           {"username": "pwuser1", "password": "1234567"}, {"username": "pwuser1", "password": "x" * 129},
           {"username": "pwuser1", "password": "ééééééé"}]
    rs = [s.post("/register", b) for b in bad]
    ok1 = all(r.status == 400 and r.json() == {"error": "invalid password"} for r in rs)
    r8 = reg("pw_user8", "éééééééé")
    r128 = reg("a" * 32, "y" * 128)
    passwords_used.extend(["éééééééé", "y" * 128])
    return ok1 and r8.status == 201 and r128.status == 201, (rs, r8, r128)


@c.test("invalid JSON is 400")
def _():
    rs = [s.post(p, b, headers={"Content-Type": "application/json"})
          for p in ("/register", "/login") for b in ("{oops", "[]", "\"str\"")]
    return all(r.status == 400 and r.json() == {"error": "invalid JSON"} for r in rs), rs


@c.test("login is case-insensitive and returns a strong token")
def _():
    r = login("aLiCe", PW)
    t = token(r)
    ok = r.status == 200 and isinstance(t, str) and re.fullmatch(r"[A-Za-z0-9]{22,}", t) is not None
    m = me(t)
    return ok and m.status == 200 and m.json() == {"username": "Alice"}, (r, m)


@c.test("each login is a separate session with a new token")
def _():
    t1, t2 = token(login("alice", PW)), token(login("alice", PW))
    return t1 != t2 and me(t1).status == 200 and me(t2).status == 200, (t1, t2)


@c.test("wrong password and unknown user give the same 401")
def _():
    rs = [login("alice", "wrong-password"), login("nobody_here", PW), login("alice", "' OR '1'='1"),
          login("alice", PW.upper())]
    return all(r.status == 401 and r.json() == {"error": "invalid credentials"} for r in rs) \
        and len({r.body for r in rs}) == 1, rs


@c.test("unicode password works exactly")
def _():
    r = reg("Unicode_User", UPW)
    ok = r.status == 201 and login("unicode_user", UPW).status == 200
    return ok and login("unicode_user", "passwörd🔑 ünïcode").status == 401, r


@c.test("missing or bad token is 401")
def _():
    hs = [{}, {"Authorization": "Bearer "}, {"Authorization": "Bearer nonsense123"},
          {"Authorization": "Basic YWxpY2U6eA=="}, {"Authorization": "nonsense"}]
    rs = [s.get("/me", headers=h) for h in hs] + [s.post("/logout", headers=h) for h in hs]
    return all(r.status == 401 and r.json() == {"error": "unauthorized"} for r in rs), rs


@c.test("logout ends only that session")
def _():
    t1, t2 = token(login("Alice", PW)), token(login("Alice", PW))
    r = s.post("/logout", headers={"Authorization": f"Bearer {t1}"})
    m1, m2 = me(t1), me(t2)
    r2 = s.post("/logout", headers={"Authorization": f"Bearer {t1}"})
    return (r.status == 204 and r.body == "" and m1.status == 401 and m1.json() == {"error": "unauthorized"}
            and m2.status == 200 and r2.status == 401), (r, m1, m2, r2)


@c.test("404 for unknown paths, 405 for wrong methods")
def _():
    r404 = [s.get("/"), s.get("/users"), s.post("/me/x")]
    r405 = [s.get("/login"), s.get("/register"), s.post("/me"), s.get("/logout")]
    return (all(r.status == 404 and r.json() == {"error": "not found"} for r in r404)
            and all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in r405)), (r404, r405)


@c.test("racing registrations of one name: exactly one succeeds")
def _():
    names = ["Racer", "racer", "RACER", "rAcEr"] * 5
    with ThreadPoolExecutor(20) as ex:
        rs = list(ex.map(lambda u: reg(u, "race-pass-1"), names))
    ok = sum(r.status == 201 for r in rs) == 1 and sum(r.status == 409 for r in rs) == 19
    return ok and login("racer", "race-pass-1").status == 200, [r.status for r in rs]


@c.test("parallel registrations and logins of different users")
def _():
    def one(i):
        a = reg(f"user_{i}", f"password-{i}")
        b = login(f"USER_{i}", f"password-{i}")
        m = me(token(b))
        return a.status == 201 and b.status == 200 and m.json() == {"username": f"user_{i}"}
    with ThreadPoolExecutor(10) as ex:
        res = list(ex.map(one, range(20)))
    passwords_used.extend(f"password-{i}" for i in range(20))
    return all(res), res


@c.test("password text does not appear in the database")
def _():
    blob = b""
    for p in c.work.glob("auth.db*"):
        blob += p.read_bytes()
    found = [p for p in passwords_used if p.encode("utf-8") in blob]
    return len(blob) > 0 and not found, found


@c.test("users and sessions survive a restart")
def _():
    global s
    t = token(login("alice", PW))
    s.stop()
    s = c.server(env)
    return me(t).status == 200 and login("ALICE", PW).status == 200, t


@c.test("sessions expire after SESSION_TTL_SECONDS")
def _():
    s2 = c.server({"DB_PATH": str(c.work / "auth-ttl.db"), "SESSION_TTL_SECONDS": "2"})
    s2.post("/register", {"username": "shortlived", "password": PW})
    t = token(s2.post("/login", {"username": "shortlived", "password": PW}))
    m1 = s2.get("/me", headers={"Authorization": f"Bearer {t}"})
    time.sleep(3.2)
    m2 = s2.get("/me", headers={"Authorization": f"Bearer {t}"})
    t2 = token(s2.post("/login", {"username": "shortlived", "password": PW}))
    m3 = s2.get("/me", headers={"Authorization": f"Bearer {t2}"})
    return (m1.status == 200 and m2.status == 401 and m2.json() == {"error": "unauthorized"}
            and m3.status == 200), (m1, m2, m3)


c.finish()
