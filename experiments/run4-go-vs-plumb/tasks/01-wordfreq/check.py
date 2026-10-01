import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

expect("example", c.run(["--top", "2"], stdin="The cat and the hat. THE END"),
       "the 3\nand 1\ntotal 7 unique 5\n")
expect("ties by word", c.run([], stdin="b a c b a c d"), "a 2\nb 2\nc 2\nd 1\ntotal 7 unique 4\n")
expect("default top 10", c.run([], stdin=" ".join(f"w{chr(97+i)}" for i in range(15)).replace("w", "x")),
       "".join(f"x{chr(97+i)} 1\n" for i in range(10)) + "total 15 unique 15\n")
expect("unicode lowercase", c.run([], stdin="Ärger ärger ÄRGER straße"), "ärger 3\nstraße 1\ntotal 4 unique 2\n")
expect("min-length counts code points", c.run(["--min-length", "4"], stdin="élan ok été naïve"),
       "naïve 1\nélan 1\ntotal 2 unique 2\n")
expect("apostrophes", c.run([], stdin="'tis don't ''' rock'n'roll"),
       "don't 1\nrock'n'roll 1\ntis 1\ntotal 3 unique 3\n")
expect("digits separate", c.run([], stdin="abc123def 42"), "abc 1\ndef 1\ntotal 2 unique 2\n")
expect("empty input", c.run([], stdin=""), "total 0 unique 0\n")
expect("top 0", c.run(["--top", "0"], stdin="a b"), "total 2 unique 2\n")
expect("invalid utf-8 separates", c.run([], stdin=b"ab\xffcd ab"), "ab 2\ncd 1\ntotal 3 unique 2\n")

(c.work / "f1.txt").write_text("one two\n")
(c.work / "f2.txt").write_text("two three\n")
expect("several files", c.run(["f1.txt", "f2.txt"]), "two 2\none 1\nthree 1\ntotal 4 unique 3\n")

r = c.run(["f1.txt", "missing.txt"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read missing.txt" in r.err, r)
for args in (["--top"], ["--top", "x"], ["--top", "-1"], ["--min-length", "0"], ["--bogus"]):
    r = c.run(args, stdin="a")
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

big = ("alpha beta gamma " * 200000)
expect("large input", c.run(["--top", "1"], stdin=big, timeout=20), "alpha 200000\ntotal 600000 unique 3\n")
c.finish()
