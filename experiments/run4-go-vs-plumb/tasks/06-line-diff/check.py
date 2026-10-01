import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=1):
    c.case(name, r.code == code and r.out == out, r)

def diff(old, new, args=(), names=("old.txt", "new.txt")):
    for n, t in zip(names, (old, new)):
        (c.work / n).write_bytes(t.encode() if isinstance(t, str) else t)
    return c.run([*args, *names])

def lines(*xs):
    return "".join(x + "\n" for x in xs)

H = "--- old.txt\n+++ new.txt\n"
expect("example", diff(lines(*"abcdef"), lines(*"acdefg"), ["-U", "1"]),
       H + "@@ -1,3 +1,2 @@\n a\n-b\n c\n@@ -6 +5,2 @@\n f\n+g\n")
r = diff(lines("same", "text"), lines("same", "text"))
c.case("equal files: exit 0, no output", r.code == 0 and r.out == "", r)
r = diff("", "")
c.case("two empty files are equal", r.code == 0 and r.out == "", r)
expect("empty old file", diff("", lines("x", "y", "z")), H + "@@ -0,0 +1,3 @@\n+x\n+y\n+z\n")
expect("empty new file", diff(lines("x", "y"), ""), H + "@@ -1,2 +0,0 @@\n-x\n-y\n")
expect("tie-break: earliest lines kept, removals first",
       diff(lines("A", "B", "A"), lines("A")), H + "@@ -1,3 +1 @@\n A\n-B\n-A\n")
expect("tie-break: swapped lines", diff(lines("x", "y"), lines("y", "x")), H + "@@ -1,2 +1,2 @@\n-x\n y\n+x\n")
expect("replaced block: removals before additions",
       diff(lines("a", "b", "c"), lines("d", "e")), H + "@@ -1,3 +1,2 @@\n-a\n-b\n-c\n+d\n+e\n")
expect("moved block, minimal diff",
       diff(lines(*"123456789"), lines(*"6789123")),
       H + "@@ -1,9 +1,7 @@\n-1\n-2\n-3\n-4\n-5\n 6\n 7\n 8\n 9\n+1\n+2\n+3\n")
expect("-U 0: zero counts use the line before",
       diff(lines(*"abcdef"), lines("a", "b", "c", "N", "d", "e"), ["-U", "0"]),
       H + "@@ -3,0 +4 @@\n+N\n@@ -6 +6,0 @@\n-f\n")
base = [f"l{i}" for i in range(1, 21)]
def changed(*idx):
    return lines(*[("X" + x if i in idx else x) for i, x in enumerate(base, 1)])
expect("gap of exactly 2N kept lines joins hunks",
       diff(lines(*base), changed(5, 12)),
       H + "@@ -2,14 +2,14 @@\n l2\n l3\n l4\n-l5\n+Xl5\n l6\n l7\n l8\n l9\n l10\n l11\n-l12\n+Xl12\n l13\n l14\n l15\n")
expect("gap of 2N+1 kept lines splits hunks",
       diff(lines(*base), changed(5, 13)),
       H + "@@ -2,7 +2,7 @@\n l2\n l3\n l4\n-l5\n+Xl5\n l6\n l7\n l8\n"
           "@@ -10,7 +10,7 @@\n l10\n l11\n l12\n-l13\n+Xl13\n l14\n l15\n l16\n")
expect("context clipped at both ends, -U 2",
       diff(lines(*base), changed(1, 20), ["-U", "2"]),
       H + "@@ -1,3 +1,3 @@\n-l1\n+Xl1\n l2\n l3\n@@ -18,3 +18,3 @@\n l18\n l19\n-l20\n+Xl20\n")
expect("missing final newline differs",
       diff("a\nb", "a\nb\n"), H + "@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end of file\n+b\n")
expect("missing final newline on a kept line",
       diff("x\nb", "y\nb"), H + "@@ -1,2 +1,2 @@\n-x\n+y\n b\n\\ No newline at end of file\n")
expect("CR is part of the line", diff("a\r\nb\n", "a\nb\n"), H + "@@ -1,2 +1,2 @@\n-a\r\n+a\n b\n")
os.makedirs(c.work / "sub", exist_ok=True)
expect("unicode lines, names as given",
       diff(lines("héllo", "日本", "ok"), lines("hello", "日本", "ok"), names=("sub/ä.txt", "./b.txt")),
       "--- sub/ä.txt\n+++ ./b.txt\n@@ -1,3 +1,3 @@\n-héllo\n+hello\n 日本\n ok\n")

r = c.run(["old.txt", "nope.txt"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read nope.txt" in r.err, r)
for args in (["old.txt"], ["old.txt", "new.txt", "x.txt"], ["-U", "x", "old.txt", "new.txt"],
             ["-U", "-1", "old.txt", "new.txt"], ["-q", "old.txt", "new.txt"]):
    r = c.run(args)
    c.case("usage error " + " ".join(args), r.code == 2 and r.err.startswith("usage:") and r.out == "", r)

old = [f"line {i}" for i in range(2000)]
new = [("X" + x if i % 100 == 50 else x) for i, x in enumerate(old)]
want = H + "".join(f"@@ -{k - 2},7 +{k - 2},7 @@\n" + "".join(f" {x}\n" for x in old[k - 3:k])
                   + f"-{old[k]}\n+{new[k]}\n" + "".join(f" {x}\n" for x in old[k + 1:k + 4])
                   for k in range(50, 2000, 100))
expect("large files", diff(lines(*old), lines(*new)), want)
c.finish()
