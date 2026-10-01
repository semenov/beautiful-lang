import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
n = [0]

def fresh():
    n[0] += 1
    p = c.work / f"db{n[0]}" / "todo.db"
    p.parent.mkdir(parents=True, exist_ok=True)
    if p.exists():
        p.unlink()
    return str(p)

def runner(db):
    return lambda *args: c.run(list(args), env={"TODO_DB": db})

def seq(name, steps):
    """steps: [(args, expected_stdout, expected_code)]; fails at the first mismatch."""
    db = fresh(); app = runner(db)
    for args, out, code in steps:
        r = app(*args)
        if r.code != code or r.out != out:
            c.case(name, False, f"step {args!r}: expected exit={code} stdout={out!r}; got {r!r}")
            return db
    c.case(name, True)
    return db

db = seq("example", [
    (["add", "Buy milk", "--tag", "home", "--due", "2026-03-01"], "added 1\n", 0),
    (["add", "50% off: ask Bob's \"deal\""], "added 2\n", 0),
    (["done", "2"], "done 2\n", 0),
    (["list"], "1 [ ] Buy milk (due 2026-03-01) #home\n2 [x] 50% off: ask Bob's \"deal\"\n", 0),
    (["list", "--overdue", "--today", "2026-03-02"], "1 [ ] Buy milk (due 2026-03-01) #home\n", 0),
])
c.case("database file created at TODO_DB", os.path.isfile(db), db)

seq("ids never reused", [
    (["add", "a"], "added 1\n", 0), (["add", "b"], "added 2\n", 0), (["add", "c"], "added 3\n", 0),
    (["rm", "3"], "removed 3\n", 0), (["rm", "1"], "removed 1\n", 0),
    (["add", "d"], "added 4\n", 0),
    (["list"], "2 [ ] b\n4 [ ] d\n", 0),
])

seq("tags sorted, deduplicated, filtered exactly", [
    (["add", "t1", "--tag", "work", "--tag", "Urgent", "--tag", "a-b_c", "--tag", "work"], "added 1\n", 0),
    (["add", "--tag", "urgent", "t2"], "added 2\n", 0),
    (["add", "t3"], "added 3\n", 0),
    (["list"], "1 [ ] t1 #Urgent #a-b_c #work\n2 [ ] t2 #urgent\n3 [ ] t3\n", 0),
    (["list", "--tag", "urgent"], "2 [ ] t2 #urgent\n", 0),
    (["list", "--tag", "wor"], "", 0),
])

seq("overdue: open, due strictly before today", [
    (["add", "past", "--due", "2026-01-31"], "added 1\n", 0),
    (["add", "today", "--due", "2026-02-01"], "added 2\n", 0),
    (["add", "future", "--due", "2027-01-01"], "added 3\n", 0),
    (["add", "nodue"], "added 4\n", 0),
    (["add", "past but done", "--due", "2025-12-01", "--tag", "x"], "added 5\n", 0),
    (["add", "long ago", "--due", "1999-12-31", "--tag", "x"], "added 6\n", 0),
    (["done", "5"], "done 5\n", 0),
    (["list", "--today", "2026-02-01", "--overdue"], "1 [ ] past (due 2026-01-31)\n6 [ ] long ago (due 1999-12-31) #x\n", 0),
    (["list", "--overdue", "--today", "2026-02-01", "--tag", "x"], "6 [ ] long ago (due 1999-12-31) #x\n", 0),
])

evil = "Robert'); DROP TABLE todos;--"
seq("SQL-like title stored verbatim", [
    (["add", "first"], "added 1\n", 0),
    (["add", evil], "added 2\n", 0),
    (["add", "x' OR '1'='1"], "added 3\n", 0),
    (["list"], f"1 [ ] first\n2 [ ] {evil}\n3 [ ] x' OR '1'='1\n", 0),
    (["search", "' OR '"], "3 [ ] x' OR '1'='1\n", 0),
])

seq("search: % and _ are literal", [
    (["add", "100% done"], "added 1\n", 0),
    (["add", "100 percent"], "added 2\n", 0),
    (["add", "file_name"], "added 3\n", 0),
    (["add", "filename"], "added 4\n", 0),
    (["search", "%"], "1 [ ] 100% done\n", 0),
    (["search", "_"], "3 [ ] file_name\n", 0),
    (["search", "e_n"], "3 [ ] file_name\n", 0),
    (["search", "100%"], "1 [ ] 100% done\n", 0),
])

seq("search is case-sensitive substring", [
    (["add", "Buy Milk"], "added 1\n", 0),
    (["add", "milk the cow"], "added 2\n", 0),
    (["add", "MILKSHAKE"], "added 3\n", 0),
    (["search", "Milk"], "1 [ ] Buy Milk\n", 0),
    (["search", "milk"], "2 [ ] milk the cow\n", 0),
    (["search", "ilk"], "1 [ ] Buy Milk\n2 [ ] milk the cow\n", 0),
    (["search", "nothing"], "", 0),
])

seq("quotes, backslashes and unicode", [
    (["add", 'say "hi" \\n it\'s'], "added 1\n", 0),
    (["add", "Größe 日本語 ✓"], "added 2\n", 0),
    (["add", "  padded  "], "added 3\n", 0),
    (["search", '"hi" \\'], '1 [ ] say "hi" \\n it\'s\n', 0),
    (["search", "日本"], "2 [ ] Größe 日本語 ✓\n", 0),
    (["list"], '1 [ ] say "hi" \\n it\'s\n2 [ ] Größe 日本語 ✓\n3 [ ]   padded  \n', 0),
])

seq("done is idempotent", [
    (["add", "a"], "added 1\n", 0), (["done", "1"], "done 1\n", 0), (["done", "1"], "done 1\n", 0),
    (["list"], "1 [x] a\n", 0),
])

db = fresh(); app = runner(db)
app("add", "a"); app("add", "b"); app("rm", "2")
r1, r2, r3 = app("done", "7"), app("rm", "2"), app("done", "2")
c.case("unknown ids exit 3",
       all(r.code == 3 and r.out == "" for r in (r1, r2, r3))
       and "error: no such todo: 7" in r1.err and "error: no such todo: 2" in r2.err, (r1, r2, r3))

seq("leap day accepted", [(["add", "leap", "--due", "2024-02-29"], "added 1\n", 0),
                          (["list"], "1 [ ] leap (due 2024-02-29)\n", 0)])

db = fresh(); app = runner(db)
bad = [app("add", "x", "--due", d) for d in ("2025-02-30", "2023-02-29", "2026-13-01", "2026-1-05", "2026-04-31", "tomorrow")]
bad.append(app("list", "--overdue", "--today", "2026-02-30"))
after = app("list")
c.case("invalid dates rejected, nothing stored",
       all(r.code == 64 and r.err.startswith("usage:") and r.out == "" for r in bad) and after.code == 0 and after.out == "",
       (bad, after))

r = c.run(["list"], env={"TODO_DB": ""})
r2 = c.run(["add", "x"], env={"TODO_DB": ""})
c.case("TODO_DB empty", r.code == 2 and "error: TODO_DB is not set" in r.err and r2.code == 2 and r2.out == "", (r, r2))

db = fresh(); app = runner(db)
app("add", "keep")
cases = {
    "unknown command": ["frobnicate"],
    "add without title": ["add"],
    "add empty title": ["add", ""],
    "title with newline": ["add", "two\nlines"],
    "invalid tag": ["add", "x", "--tag", "a b"],
    "id not a number": ["done", "abc"],
    "id zero": ["rm", "0"],
    "overdue without today": ["list", "--overdue"],
    "today without overdue": ["list", "--today", "2026-01-01"],
    "empty search": ["search", ""],
    "extra argument": ["done", "1", "2"],
    "unknown option": ["list", "--all"],
}
fails = []
for label, args in cases.items():
    r = app(*args)
    if not (r.code == 64 and r.err.startswith("usage:") and r.out == ""):
        fails.append((label, r))
after = app("list")
c.case("usage errors", not fails and after.out == "1 [ ] keep\n", (fails, after))

db = fresh(); app = runner(db)
outs = [app("add", f"task {i}", "--tag", f"g{i % 3}").out for i in range(1, 151)]
r = app("list", "--tag", "g0")
c.case("150 todos", outs == [f"added {i}\n" for i in range(1, 151)]
       and r.out == "".join(f"{i} [ ] task {i} #g0\n" for i in range(3, 151, 3)), r)
c.finish()
