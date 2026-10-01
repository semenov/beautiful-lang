import sys, os, random
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def L(method, target, status, lat, bytes_="100", client="10.0.0.1"):
    return f'{client} - - [10/Oct/2024:13:55:36 +0000] "{method} {target} HTTP/1.1" {status} {bytes_} {lat}\n'

example = (L("GET", "/api/users?id=3", 200, 40, "512") + L("GET", "/api/users", 503, 120, "-")
           + L("POST", "/api/login", 200, 15, "64") + "garbage\n")
(c.work / "access.log").write_text(example)
expect("example", c.run(["access.log"]),
       "GET /api/users count=2 error_rate=50.00% p50=40 p95=120 max=120\n"
       "POST /api/login count=1 error_rate=0.00% p50=15 p95=15 max=15\n"
       "total=3 malformed=1\n")

rnd = random.Random(7)
v = list(range(1, 21)); rnd.shuffle(v)
w = list(range(1, 101)); rnd.shuffle(w)
expect("nearest-rank percentiles (n=20, n=100, n=1)",
       c.run([], stdin="".join(L("GET", "/a", 200, x) for x in v) + "".join(L("GET", "/b", 200, x * 3) for x in w)
             + L("GET", "/c", 200, 77)),
       "GET /b count=100 error_rate=0.00% p50=150 p95=285 max=300\n"
       "GET /a count=20 error_rate=0.00% p50=10 p95=19 max=20\n"
       "GET /c count=1 error_rate=0.00% p50=77 p95=77 max=77\n"
       "total=121 malformed=0\n")
expect("percentiles with repeated values (n=7)",
       c.run([], stdin="".join(L("GET", "/r", 200, x) for x in [5, 1, 5, 9, 1, 1, 30])),
       "GET /r count=7 error_rate=0.00% p50=5 p95=30 max=30\ntotal=7 malformed=0\n")

err = (L("GET", "/x", 500, 1) + L("GET", "/x", 200, 1) * 799
       + L("GET", "/y", 599, 1) + L("GET", "/y", 499, 1) * 2
       + L("GET", "/z", 502, 1) * 2 + L("GET", "/z", 404, 1)
       + L("GET", "/w", 500, 1) + L("GET", "/w", 302, 1) * 15)
expect("error rate exact, halves up, only 5xx",
       c.run([], stdin=err),
       "GET /x count=800 error_rate=0.13% p50=1 p95=1 max=1\n"
       "GET /w count=16 error_rate=6.25% p50=1 p95=1 max=1\n"
       "GET /y count=3 error_rate=33.33% p50=1 p95=1 max=1\n"
       "GET /z count=3 error_rate=66.67% p50=1 p95=1 max=1\n"
       "total=822 malformed=0\n")

expect("ties ordered by endpoint, query stripped, method distinct",
       c.run([], stdin=L("POST", "/b", 200, 1) + L("GET", "/b?x=1&y=2", 200, 2) + L("GET", "/a?", 200, 3)
             + L("DELETE", "/b", 200, 4)),
       "DELETE /b count=1 error_rate=0.00% p50=4 p95=4 max=4\n"
       "GET /a count=1 error_rate=0.00% p50=3 p95=3 max=3\n"
       "GET /b count=1 error_rate=0.00% p50=2 p95=2 max=2\n"
       "POST /b count=1 error_rate=0.00% p50=1 p95=1 max=1\n"
       "total=4 malformed=0\n")

good = L("GET", "/ok", 200, 5)
bad = [
    good.replace(" 5\n", "\n"),                     # missing latency
    good.replace(" 5\n", " 5ms\n"),                 # latency with unit
    good.replace(" 5\n", " 5 \n"),                  # trailing space
    " " + good,                                     # leading space
    good.replace('"GET', '"get'),                   # lowercase method
    good.replace(" 200 ", " 600 "),                 # status out of range
    good.replace(" 200 ", " 099 "),
    good.replace(" 200 ", " 20 "),
    good.replace(" 100 ", " 1k "),                  # bad bytes
    good.replace(" - - ", " -  - "),                # double space
    good.replace("[10/Oct", "10/Oct"),              # missing bracket
    good.replace(" HTTP/1.1", ""),                  # no protocol
    good.replace(" HTTP/1.1", " HTTP/x"),
    good.replace("/ok", "/o\"k"),
    "\t\n",
]
expect("malformed lines counted, empty lines and CRLF fine",
       c.run([], stdin="\n" + good + "".join(bad) + "\n" + good.replace("\n", "\r\n") + "\n"),
       "GET /ok count=2 error_rate=0.00% p50=5 p95=5 max=5\ntotal=2 malformed=15\n")
expect("latency leading zeros, bytes dash, odd time text",
       c.run([], stdin=L("GET", "/l", 200, "007", "-").replace("[10/Oct/2024:13:55:36 +0000]", "[any time \"here\"]")
             + L("GET", "/l", 200, "0120")),
       "GET /l count=2 error_rate=0.00% p50=7 p95=120 max=120\ntotal=2 malformed=0\n")
expect("min-count hides endpoints but totals count them",
       c.run(["--min-count", "2"], stdin=L("GET", "/a", 200, 1) * 2 + L("GET", "/b", 200, 1) + "junk\n"),
       "GET /a count=2 error_rate=0.00% p50=1 p95=1 max=1\ntotal=3 malformed=1\n")
expect("unicode paths sorted by code point",
       c.run([], stdin=L("GET", "/café", 200, 1) + L("GET", "/cafe", 200, 2) + L("GET", "/caff", 200, 3)
             + L("GET", "/日本", 200, 4)),
       "GET /cafe count=1 error_rate=0.00% p50=2 p95=2 max=2\n"
       "GET /caff count=1 error_rate=0.00% p50=3 p95=3 max=3\n"
       "GET /café count=1 error_rate=0.00% p50=1 p95=1 max=1\n"
       "GET /日本 count=1 error_rate=0.00% p50=4 p95=4 max=4\n"
       "total=4 malformed=0\n")
expect("empty input", c.run([], stdin=""), "total=0 malformed=0\n")
expect("no final newline", c.run([], stdin=good.rstrip("\n")),
       "GET /ok count=1 error_rate=0.00% p50=5 p95=5 max=5\ntotal=1 malformed=0\n")

(c.work / "a.log").write_text(L("GET", "/m", 200, 10))
(c.work / "b.log").write_text(L("GET", "/m", 500, 20) + "bad\n")
expect("several files", c.run(["a.log", "b.log"]),
       "GET /m count=2 error_rate=50.00% p50=10 p95=20 max=20\ntotal=2 malformed=1\n")
r = c.run(["a.log", "missing.log"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read missing.log" in r.err, r)
for args in (["--min-count"], ["--min-count", "0"], ["--min-count", "x"], ["--bogus"]):
    r = c.run(args, stdin=good)
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

rnd = random.Random(11)
parts, lats = [], {}
for i in range(200000):
    ep = f"/e{i % 5}"; lat = rnd.randrange(1, 5000); st = 500 if i % 97 == 0 else 200
    parts.append(L("GET", ep + f"?i={i}", st, lat)); lats.setdefault(ep, [[], 0]); lats[ep][0].append(lat)
    lats[ep][1] += st == 500
want = ""
for ep in sorted(lats):
    v, e = lats[ep]; v.sort(); n = len(v); bp = (e * 20000 + n) // (2 * n)
    want += (f"GET {ep} count={n} error_rate={bp // 100}.{bp % 100:02d}% p50={v[(50 * n + 99) // 100 - 1]} "
             f"p95={v[(95 * n + 99) // 100 - 1]} max={v[-1]}\n")
want += "total=200000 malformed=0\n"
expect("large input", c.run([], stdin="".join(parts), timeout=20), want)
c.finish()
