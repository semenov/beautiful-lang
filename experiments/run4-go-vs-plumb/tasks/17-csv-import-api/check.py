import sys, os
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
for p in list(c.work.glob("users.db*")):
    p.unlink()
env = {"DB_PATH": str(c.work / "users.db")}
s = c.server(env)

HDR = {"Content-Type": "text/csv"}


def imp(text):
    return s.req("POST", "/import", text.encode("utf-8") if isinstance(text, str) else text, headers=HDR, timeout=30)


def users():
    return s.get("/users").json()["users"]


def err(row, field, message):
    return {"row": row, "field": field, "message": message}


@c.test("happy path: quoting, unicode, verbatim names, lowercased emails")
def _():
    csv = ('email,name,age\n'
           'Ann@Example.com,Ann,34\n'
           'bob@example.com,"Smith, Bob ""The Builder""",40\n'
           'zoe@example.org,"multi\nline",13\n'
           'chen@example.cn,陈 美丽,120\n'
           'sql@example.com,"Robert\'); DROP TABLE users;--",50\n')
    r = imp(csv)
    want = sorted([
        {"email": "ann@example.com", "name": "Ann", "age": 34},
        {"email": "bob@example.com", "name": 'Smith, Bob "The Builder"', "age": 40},
        {"email": "zoe@example.org", "name": "multi\nline", "age": 13},
        {"email": "chen@example.cn", "name": "陈 美丽", "age": 120},
        {"email": "sql@example.com", "name": "Robert'); DROP TABLE users;--", "age": 50}], key=lambda u: u["email"])
    u = users()
    return r.status == 201 and r.json() == {"imported": 5} and u == want, (r, u)


@c.test("CRLF, no final newline, columns in another order, spaces kept")
def _():
    r = imp('age,email,name\r\n21,crlf1@example.com, Padded \r\n22,crlf2@example.com,"x"')
    u = {x["email"]: x for x in users()}
    return (r.status == 201 and r.json() == {"imported": 2} and u.get("crlf1@example.com") == {"email": "crlf1@example.com", "name": " Padded ", "age": 21}
            and u.get("crlf2@example.com", {}).get("name") == "x"), (r, u.get("crlf1@example.com"))


@c.test("header only imports nothing")
def _():
    r1, r2 = imp("email,name,age\n"), imp("name,age,email")
    return r1.status == 201 and r1.json() == {"imported": 0} and r2.status == 201 and r2.json() == {"imported": 0}, (r1, r2)


@c.test("per-row errors in the stated shape and order; nothing stored")
def _():
    before = users()
    csv = ('email,name,age\n'
           'good1@example.com,Good,30\n'
           'bad-email,,12\n'
           'good2@example.com,Fine\n'
           'good3@example.com,Fine,30,extra\n'
           'GOOD1@example.com,Again,30\n'
           'ann@example.com,Ann again,abc\n'
           'ok@example.com,' + "é" * 101 + ',121\n')
    r = imp(csv)
    want = {"imported": 0, "errors": [
        err(2, "email", "invalid email"), err(2, "name", "invalid name"), err(2, "age", "invalid age"),
        err(3, "row", "wrong number of fields"), err(4, "row", "wrong number of fields"),
        err(5, "email", "duplicate email in file"),
        err(6, "email", "email already exists"), err(6, "age", "invalid age"),
        err(7, "name", "invalid name"), err(7, "age", "invalid age")]}
    return r.status == 422 and r.json() == want and users() == before, r


@c.test("stored email repeated in file: already exists on every row")
def _():
    r = imp("email,name,age\nANN@example.com,A,20\nann@EXAMPLE.com,B,20\nnew1@example.com,C,20\n")
    want = {"imported": 0, "errors": [err(1, "email", "email already exists"), err(2, "email", "email already exists")]}
    return r.status == 422 and r.json() == want, r


@c.test("email rule")
def _():
    good = ["a@b.co", "first.last+tag@sub.example.org", "x" * 242 + "@example.com", "ünï@例え.jp"]
    bad = ["a@@b.com", "@b.com", "a@b", "a@.com", "a@com.", "a b@c.com", "a\t@c.com", "a@b.com ", "ab.com",
           "a@b@c.com", "x" * 243 + "@example.com"]
    rows = "".join(f'"{e}",N,30\n' for e in bad)
    r = imp("email,name,age\n" + rows)
    want = {"imported": 0, "errors": [err(i + 1, "email", "invalid email") for i in range(len(bad))]}
    r2 = imp("email,name,age\n" + "".join(f'"{e}",N,30\n' for e in good))
    return r.status == 422 and r.json() == want and r2.status == 201 and r2.json() == {"imported": len(good)}, (r, r2)


@c.test("name and age bounds")
def _():
    bad_age = ["12", "121", "abc", "-5", "20.5", " 20", "", "+20"]
    rows = "".join(f'age{i}@example.com,N,"{a}"\n' for i, a in enumerate(bad_age)) + "nm@example.com,,30\n"
    r = imp("email,name,age\n" + rows)
    want = {"imported": 0, "errors": [err(i + 1, "age", "invalid age") for i in range(len(bad_age))]
            + [err(len(bad_age) + 1, "name", "invalid name")]}
    r2 = imp("email,name,age\nb1@example.com," + "é" * 100 + ",13\nb2@example.com,x,120\n")
    return r.status == 422 and r.json() == want and r2.status == 201, (r, r2)


@c.test("malformed CSV is 400")
def _():
    bodies = ["", "email,name,age\nx@y.com,\"unterminated,30\n", "email,name\nx@y.com,N\n",
              "email,name,age,extra\nx@y.com,N,30,1\n", "email,name,name\nx@y.com,N,M\n", "Email,Name,Age\n",
              "email,name,age\nx@y.com,a\"b,30\n", "email,name,age\nx@y.com,\"ab\"c,30\n",
              b"email,name,age\nx@y.com,\xff\xfe,30\n"]
    rs = [imp(b) for b in bodies]
    bad = [(i, r) for i, r in enumerate(rs) if not (r.status == 400 and r.json() == {"error": "invalid csv"})]
    return not bad, bad


@c.test("one bad row among many: nothing stored")
def _():
    before = users()
    rows = "".join(f"atom{i}@example.com,User {i},30\n" for i in range(999)) + "atom-bad,User,30\n"
    r = imp("email,name,age\n" + rows)
    j = r.json()
    return r.status == 422 and j["errors"] == [err(1000, "email", "invalid email")] and users() == before, r


@c.test("large import of 5000 rows")
def _():
    n0 = len(users())
    rows = "".join(f"big{i:05d}@example.com,\"Big, {i}\",{13 + i % 100}\n" for i in range(5000))
    r = imp("email,name,age\n" + rows)
    u = users()
    return r.status == 201 and r.json() == {"imported": 5000} and len(u) == n0 + 5000, r


@c.test("concurrent imports sharing an email: exactly one succeeds")
def _():
    def one(i):
        return imp(f"email,name,age\nshared@race.com,R{i},30\nown{i}@race.com,O{i},30\n")
    with ThreadPoolExecutor(12) as ex:
        rs = list(ex.map(one, range(12)))
    wins = [i for i, r in enumerate(rs) if r.status == 201]
    losers_ok = all(r.json() == {"imported": 0, "errors": [err(1, "email", "email already exists")]}
                    for r in rs if r.status != 201)
    race = [u["email"] for u in users() if u["email"].endswith("@race.com")]
    return (len(wins) == 1 and losers_ok and sorted(race) == sorted(["shared@race.com", f"own{wins[0]}@race.com"])), \
        ([r.status for r in rs], race)


@c.test("concurrent imports of different emails all succeed")
def _():
    n0 = len(users())
    def one(i):
        return imp("email,name,age\n" + "".join(f"par{i}-{j}@example.com,P,40\n" for j in range(50)))
    with ThreadPoolExecutor(10) as ex:
        rs = list(ex.map(one, range(10)))
    return all(r.status == 201 and r.json() == {"imported": 50} for r in rs) and len(users()) == n0 + 500, \
        [r for r in rs if r.status != 201][:2]


@c.test("users ordered by email and survive a restart")
def _():
    global s
    u = users()
    s.stop()
    s = c.server(env)
    u2 = users()
    emails = [x["email"] for x in u2]
    return u2 == u and emails == sorted(emails), len(u2)


@c.test("404 and 405")
def _():
    r404 = [s.get("/"), s.get("/user"), s.post("/import/x", "a")]
    r405 = [s.get("/import"), s.post("/users", "x"), s.req("DELETE", "/users")]
    return (all(r.status == 404 and r.json() == {"error": "not found"} for r in r404)
            and all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in r405)), (r404, r405)


c.finish()
