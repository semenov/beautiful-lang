import sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def report(bal, tr):
    return ("balances:\n" + "".join(f"  {n} {a}\n" for n, a in bal) + "transfers:\n"
            + "".join(f"  {f} -> {t} {a}\n" for f, t, a in tr))

expect("example", c.run([], stdin="alice 90.00 alice,bob,carol\nbob 30 alice:2,bob\n"),
       report([("alice", "+40.00"), ("bob", "-10.00"), ("carol", "-30.00")],
              [("carol", "alice", "30.00"), ("bob", "alice", "10.00")]))
expect("leftover cents to first listed; ties by name",
       c.run([], stdin="p 100.00 c,a,b\n"),
       report([("a", "-33.33"), ("b", "-33.33"), ("c", "-33.34"), ("p", "+100.00")],
              [("c", "p", "33.34"), ("a", "p", "33.33"), ("b", "p", "33.33")]))
expect("uneven shares with leftover",
       c.run([], stdin="z 10.00 x:1,y:2\nz 0.01 q,r,s\n"),
       report([("q", "-0.01"), ("r", "0.00"), ("s", "0.00"), ("x", "-3.34"), ("y", "-6.66"), ("z", "+10.01")],
              [("y", "z", "6.66"), ("x", "z", "3.34"), ("q", "z", "0.01")]))
expect("largest amounts are exact",
       c.run([], stdin="w 1000000000.00 x:7,y:3,z:1\n"),
       report([("w", "+1000000000.00"), ("x", "-636363636.37"), ("y", "-272727272.73"), ("z", "-90909090.90")],
              [("x", "w", "636363636.37"), ("y", "w", "272727272.73"), ("z", "w", "90909090.90")]))
expect("greedy: largest debtor to largest creditor, partial transfers",
       c.run([], stdin="a 50 c:3,d:2\nb 50 c:3,d:2\n"),
       report([("a", "+50.00"), ("b", "+50.00"), ("c", "-60.00"), ("d", "-40.00")],
              [("c", "a", "50.00"), ("d", "b", "40.00"), ("c", "b", "10.00")]))
expect("everyone settled: zero balances, no transfers",
       c.run([], stdin="a 10 a,b\nb 10 a,b\nsolo 5.5 solo\n"),
       report([("a", "0.00"), ("b", "0.00"), ("solo", "0.00")], []))
expect("unicode and case-sensitive names, sorted by code point",
       c.run([], stdin="Zoë 4 zoe,Ann,ann,Ärni\n"),
       report([("Ann", "-1.00"), ("Zoë", "+4.00"), ("ann", "-1.00"), ("zoe", "-1.00"), ("Ärni", "-1.00")],
              [("Ann", "Zoë", "1.00"), ("ann", "Zoë", "1.00"), ("zoe", "Zoë", "1.00"), ("Ärni", "Zoë", "1.00")]))
expect("comments, blank lines, tabs, CRLF",
       c.run([], stdin="# trip\n\n   \n\t  # indented comment\r\n  a\t \t3.5   a,b:6  \r\nb 0.7 a\n"),
       report([("a", "+2.30"), ("b", "-2.30")], [("b", "a", "2.30")]))
expect("empty input", c.run([], stdin=""), report([], []))

def bad_line(text, n):
    r = c.run([], stdin=text)
    return r.code == 1 and r.out == "" and re.match(rf"error: line {n}:", r.err) is not None, r
fails = []
for amt in ("0", "0.00", "1.234", "-5", "abc", ".5", "5.", "1000000000.01", "1,5", "+3"):
    ok, r = bad_line(f"# ok\na 1 a,b\na {amt} a,b\n", 3)
    if not ok: fails.append((amt, r))
c.case("invalid amounts rejected with line number", not fails, fails)
fails = []
for line in ("a 1 a,,b", "a 1 a:0", "a 1 a:1001", "a 1 a:x", "a 1 a,b,a", "a 1 a:2,a", "a 1", "a 1 b c",
             "a:b 1 a", "a 1 a, b", "a 1 a,b:"):
    ok, r = bad_line(f"\nok 1 ok\n{line}\n", 3)
    if not ok: fails.append((line, r))
c.case("invalid splits and fields rejected with line number", not fails, fails)
r = c.run([], stdin="a 1 a\nb x b\nc y c\n")
c.case("only the first bad line is reported", r.code == 1 and r.out == "" and r.err.startswith("error: line 2:")
       and "line 3" not in r.err, r)

(c.work / "trip.txt").write_text("x 2 x,y\n")
expect("reads FILE", c.run(["trip.txt"]), report([("x", "+1.00"), ("y", "-1.00")], [("y", "x", "1.00")]))
r = c.run(["nope.txt"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read nope.txt" in r.err, r)
for args in (["a.txt", "b.txt"], ["--verbose"]):
    r = c.run(args)
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

big = "a 0.03 a,b\nc 10.01 a:1,b:1,c:1\n" * 30000
expect("large input", c.run([], stdin=big, timeout=20),
       report([("a", "-99900.00"), ("b", "-100500.00"), ("c", "+200400.00")],
              [("b", "c", "100500.00"), ("a", "c", "99900.00")]))
c.finish()
