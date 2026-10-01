import sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def bad_lines(err):
    return [int(m) for m in re.findall(r"(?m)^line (\d+):", err)]

example = "region,product,amount\nNorth,\"Widget, large\",10.50\nSouth,Gadget,3.25\nNorth,Gizmo,0.10\n"
(c.work / "sales.csv").write_text(example)
expect("example", c.run(["--by", "region", "sales.csv"]), "region,total\nNorth,10.60\nSouth,3.25\n")
expect("stdin, columns in any order", c.run(["--by", "product"], stdin="amount,product\n1,b\n2,a\n3,b\n"),
       "product,total\na,2.00\nb,4.00\n")
expect("exact cents, no float error",
       c.run(["--by", "k"], stdin="k,amount\n" + "x,0.10\n" * 10 + "y,90071992547409.93\ny,0.01\ny,0.01\nz,0.1\nz,0.2\n"),
       "k,total\nx,1.00\ny,90071992547409.95\nz,0.30\n")
expect("negatives and zero", c.run(["--by", "k"], stdin="k,amount\na,-0.5\nb,1.25\nb,-1.25\nc,-3\nc,  2.5 \n"),
       "k,total\na,-0.50\nb,0.00\nc,-0.50\n")
expect("quoted fields: commas, quotes, newlines; output quoted",
       c.run(["--by", "name"], stdin='name,amount\n"Smith, J",1\n"say ""hi""",2\n"two\nlines",3\n"Smith, J","4.00"\n'),
       'name,total\n"Smith, J",5.00\n"say ""hi""",2.00\n"two\nlines",3.00\n')
expect("CRLF and no final newline", c.run(["--by", "k"], stdin="k,amount\r\na,1\r\nb,2\r\na,3"),
       "k,total\na,4.00\nb,2.00\n")
expect("empty lines skipped, empty value is a group", c.run(["--by", "k"], stdin="k,amount\n\na,1\n\n,2\n,3\n\n"),
       "k,total\n,5.00\na,1.00\n")
expect("unicode values sorted by code point",
       c.run(["--by", "city"], stdin="city,amount\nZürich,1\nZagreb,2\nÅrhus,3\nzug,4\n東京,5\n"),
       "city,total\nZagreb,2.00\nZürich,1.00\nzug,4.00\nÅrhus,3.00\n東京,5.00\n")
expect("header only", c.run(["--by", "k"], stdin="k,amount\n"), "k,total\n")

r = c.run(["--by", "k"], stdin='k,amount\na,1\nb,"1,000.00"\nc,+1\nd,.5\ne,5.\nf,1.234\ng,1e3\nh,\ni,1,extra\nj\na,2\n')
c.case("bad records skipped with line numbers, exit 1",
       r.code == 1 and r.out == "k,total\na,3.00\n" and bad_lines(r.err) == [3, 4, 5, 6, 7, 8, 9, 10, 11], r)
r = c.run(["--by", "k"], stdin='k,note,amount\na,"multi\nline\nnote",1\nb,x,bad\n"c\nd",y,oops\ne,z,2\n')
c.case("line numbers count line breaks inside quotes",
       r.code == 1 and r.out == "k,total\na,1.00\ne,2.00\n" and bad_lines(r.err) == [5, 6], r)
r = c.run(["--by", "k"], stdin='k,amount\n"a"x,1\nb,2\n')
c.case("text after closing quote is a bad record",
       r.code == 1 and r.out == "k,total\nb,2.00\n" and bad_lines(r.err) == [2], r)
r = c.run(["--by", "k"], stdin='k,amount\na,1\nb,"2\nc,3\n')
c.case("unterminated quote ends input",
       r.code == 1 and r.out == "k,total\na,1.00\n" and bad_lines(r.err) == [3] and "unterminated quote" in r.err, r)
expect("quote inside unquoted field is ordinary", c.run(["--by", "k"], stdin='k,amount\n5" disk,1\n'),
       'k,total\n"5"" disk",1.00\n')

r = c.run(["--by", "region"], stdin="region,price\nN,1\n")
c.case("missing amount column", r.code == 2 and r.out == "" and "error: missing column amount" in r.err, r)
r = c.run(["--by", "Region"], stdin="region,amount\nN,1\n")
c.case("missing by column (case-sensitive)", r.code == 2 and r.out == "" and "error: missing column Region" in r.err, r)
r = c.run(["--by", "k"], stdin="")
c.case("empty input", r.code == 2 and r.out == "" and "error: empty input" in r.err, r)
r = c.run(["--by", "k", "nope.csv"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read nope.csv" in r.err, r)
for args in ([], ["--by"], ["--by", "k", "--bogus"], ["--by", "k", "a.csv", "b.csv"]):
    r = c.run(args, stdin="k,amount\n")
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

rows = ["id,shop,amount"] + [f"{i},s{i % 7},{i % 1000}.{i % 100:02d}" for i in range(200000)]
tot = {}
for i in range(200000):
    tot[f"s{i % 7}"] = tot.get(f"s{i % 7}", 0) + (i % 1000) * 100 + i % 100
want = "shop,total\n" + "".join(f"{k},{v // 100}.{v % 100:02d}\n" for k, v in sorted(tot.items()))
expect("large input", c.run(["--by", "shop"], stdin="\n".join(rows) + "\n", timeout=20), want)
c.finish()
