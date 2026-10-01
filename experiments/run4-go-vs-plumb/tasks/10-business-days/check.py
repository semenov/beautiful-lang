import sys, os, datetime
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def expect_all(name, pairs):
    """pairs: [(args, expected_stdout)]; all must exit 0 with that output."""
    bad = []
    for args, out in pairs:
        r = c.run(args)
        if r.code != 0 or r.out != out:
            bad.append((args, out, r))
    c.case(name, not bad, bad)

(c.work / "hol.txt").write_text("# public holidays\n2025-12-25   # Christmas\n2025-12-26\n")
expect_all("example", [
    (["between", "2025-12-22", "2025-12-29", "--holidays", "hol.txt"], "3\n"),
    (["add", "2025-12-24", "1", "--holidays", "hol.txt"], "2025-12-29\n"),
    (["add", "2025-12-21", "0"], "2025-12-22\n"),
])

expect_all("between, default weekend", [
    (["between", "2025-01-06", "2025-01-13"], "5\n"),     # Mon..Mon
    (["between", "2025-01-04", "2025-01-06"], "0\n"),     # Sat..Mon
    (["between", "2025-01-03", "2025-01-06"], "1\n"),     # Fri..Mon: Fri counted
    (["between", "2025-01-06", "2025-01-07"], "1\n"),
    (["between", "2025-01-06", "2025-01-06"], "0\n"),
])
expect_all("between, END before START is negative", [
    (["between", "2025-01-13", "2025-01-06"], "-5\n"),
    (["between", "2025-12-29", "2025-12-22", "--holidays", "hol.txt"], "-3\n"),
    (["between", "2025-01-06", "2025-01-03"], "-1\n"),
])
expect_all("between across leap day and year end", [
    (["between", "2024-02-28", "2024-03-01"], "2\n"),
    (["between", "2024-12-30", "2025-01-02"], "3\n"),
])

def count(a, b, weekend=(5, 6)):
    d, n = a, 0
    while d < b:
        if d.weekday() not in weekend:
            n += 1
        d += datetime.timedelta(1)
    return n

n = count(datetime.date(1999, 3, 15), datetime.date(2101, 7, 1))
expect("long range", c.run(["between", "1999-03-15", "2101-07-01"]), f"{n}\n")

expect_all("add forwards and backwards", [
    (["add", "2025-01-03", "1"], "2025-01-06\n"),      # Fri +1 -> Mon
    (["add", "2025-01-03", "5"], "2025-01-10\n"),
    (["add", "2025-01-04", "1"], "2025-01-06\n"),      # Sat +1 -> Mon
    (["add", "2025-01-06", "-1"], "2025-01-03\n"),     # Mon -1 -> Fri
    (["add", "2025-01-05", "-1"], "2025-01-03\n"),     # Sun -1 -> Fri
    (["add", "2025-12-29", "-1", "--holidays", "hol.txt"], "2025-12-24\n"),
    (["add", "2025-01-01", "-3"], "2024-12-27\n"),
])
expect_all("add 0", [
    (["add", "2025-01-08", "0"], "2025-01-08\n"),
    (["add", "2025-12-25", "0", "--holidays", "hol.txt"], "2025-12-29\n"),
])
expect_all("leap days", [
    (["add", "2000-02-28", "1"], "2000-02-29\n"),
    (["add", "2024-02-28", "1"], "2024-02-29\n"),
    (["add", "2023-02-28", "1"], "2023-03-01\n"),
])

d, left = datetime.date(2025, 1, 1), 100000
while left:
    d += datetime.timedelta(1)
    if d.weekday() < 5:
        left -= 1
expect("add a large N", c.run(["add", "2025-01-01", "100000"]), d.isoformat() + "\n")

expect_all("custom weekend fri,sat", [
    (["between", "2025-01-05", "2025-01-12", "--weekend", "fri,sat"], "5\n"),
    (["add", "2025-01-09", "1", "--weekend", "fri,sat"], "2025-01-12\n"),   # Thu +1 -> Sun
    (["between", "--weekend", "sun,sun,sat", "2025-01-06", "2025-01-13"], "5\n"),
])
expect_all("no weekend", [
    (["between", "2025-01-06", "2025-01-13", "--weekend", ""], "7\n"),
    (["add", "2025-01-03", "1", "--weekend", ""], "2025-01-04\n"),
    (["between", "2025-12-22", "2025-12-29", "--weekend", "", "--holidays", "hol.txt"], "5\n"),
])

(c.work / "messy.txt").write_text(
    "\n   \n# only a comment\n\t2025-07-04\t\r\n2025-07-05 # a Saturday\n#2025-07-07\n  2025-07-08  \n")
expect_all("holidays file: comments, blanks, whitespace, weekend holiday", [
    (["between", "2025-07-01", "2025-07-11", "--holidays", "messy.txt"], "6\n"),
    (["add", "2025-07-03", "1", "--holidays", "messy.txt"], "2025-07-07\n"),
    (["add", "2025-07-07", "1", "--holidays", "messy.txt"], "2025-07-09\n"),
])

(c.work / "august.txt").write_text("".join(f"2025-08-{d:02d}\n" for d in range(1, 32)))
expect("a month of holidays", c.run(["add", "2025-07-31", "1", "--holidays", "august.txt"]), "2025-09-01\n")

bad = []
for date in ("2023-02-29", "1900-02-29", "2025-04-31", "2025-4-01", "2025-13-01", "0000-01-01", "2025-01-01x", "yesterday"):
    r = c.run(["add", date, "1"])
    if not (r.code == 2 and r.out == "" and f"error: invalid date: {date}" in r.err):
        bad.append((date, r))
r = c.run(["between", "2025-01-01", "2025-02-30"])
if not (r.code == 2 and "error: invalid date: 2025-02-30" in r.err):
    bad.append(("END", r))
c.case("invalid dates rejected", not bad, bad)

(c.work / "badhol.txt").write_text("2025-01-01\n# fine\n2025-02-30\n")
r = c.run(["between", "2025-01-01", "2025-03-01", "--holidays", "badhol.txt"])
c.case("bad holidays line", r.code == 2 and r.out == "" and "error: badhol.txt:3: invalid date" in r.err, r)
r = c.run(["between", "2025-01-01", "2025-03-01", "--holidays", "nope.txt"])
c.case("missing holidays file", r.code == 2 and r.out == "" and "error: cannot read nope.txt" in r.err, r)

bad = []
for args in (["between"], ["between", "2025-01-01"], ["add", "2025-01-01"], ["add", "2025-01-01", "x"],
             ["add", "2025-01-01", "1.5"], ["subtract", "2025-01-01", "1"], [],
             ["add", "2025-01-01", "1", "--weekend", "fun"],
             ["add", "2025-01-01", "1", "--weekend", "mon,tue,wed,thu,fri,sat,sun"],
             ["add", "2025-01-01", "1", "--holidays"], ["add", "2025-01-01", "1", "--bogus", "x"],
             ["between", "2025-01-01", "2025-01-02", "2025-01-03"]):
    r = c.run(args)
    if not (r.code == 64 and r.err.startswith("usage:") and r.out == ""):
        bad.append((args, r))
c.case("usage errors", not bad, bad)
c.finish()
