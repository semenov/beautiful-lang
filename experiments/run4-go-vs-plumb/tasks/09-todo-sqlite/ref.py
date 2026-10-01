#!/usr/bin/env python3
import sys, os, re, sqlite3, datetime

def usage(msg="app add|list|done|rm|search ..."):
    sys.stderr.write(f"usage: {msg}\n"); sys.exit(64)

def parse_date(s):
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", s): usage("invalid date " + s)
    try: datetime.date(int(s[:4]), int(s[5:7]), int(s[8:]))
    except ValueError: usage("invalid date " + s)
    return s

def parse_id(s):
    if not re.fullmatch(r"[0-9]+", s) or int(s) < 1: usage("invalid id " + s)
    return int(s)

def parse(args, opts):
    """opts: {name: 'flag'|'value'|'multi'}. Returns (positionals, values)."""
    pos, vals = [], {}
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("--"):
            kind = opts.get(a)
            if kind is None: usage("unknown option " + a)
            if kind == "flag": vals[a] = True; i += 1; continue
            if i + 1 >= len(args): usage("missing value for " + a)
            if kind == "multi": vals.setdefault(a, []).append(args[i + 1])
            else: vals[a] = args[i + 1]
            i += 2
        else:
            pos.append(a); i += 1
    return pos, vals

args = sys.argv[1:]
if not args: usage()
cmd, rest = args[0], args[1:]
db_path = os.environ.get("TODO_DB", "")

def db():
    if not db_path:
        sys.stderr.write("error: TODO_DB is not set\n"); sys.exit(2)
    con = sqlite3.connect(db_path)
    con.executescript("""
        CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL,
            due TEXT, done INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS tags (todo_id INTEGER NOT NULL, tag TEXT NOT NULL, PRIMARY KEY (todo_id, tag));
    """)
    return con

def show(con, rows):
    out = []
    for tid, title, due, done in rows:
        tags = sorted(t for (t,) in con.execute("SELECT tag FROM tags WHERE todo_id = ?", (tid,)))
        line = f"{tid} [{'x' if done else ' '}] {title}"
        if due: line += f" (due {due})"
        line += "".join(f" #{t}" for t in tags)
        out.append(line + "\n")
    sys.stdout.write("".join(out))

def need_exists(con, tid):
    if con.execute("SELECT 1 FROM todos WHERE id = ?", (tid,)).fetchone() is None:
        sys.stderr.write(f"error: no such todo: {tid}\n"); sys.exit(3)

if cmd == "add":
    pos, v = parse(rest, {"--due": "value", "--tag": "multi"})
    if len(pos) != 1 or pos[0] == "" or "\n" in pos[0] or "\r" in pos[0]: usage("app add TITLE")
    due = parse_date(v["--due"]) if "--due" in v else None
    tags = v.get("--tag", [])
    for t in tags:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", t): usage("invalid tag " + t)
    con = db()
    with con:
        cur = con.execute("INSERT INTO todos (title, due) VALUES (?, ?)", (pos[0], due))
        for t in set(tags):
            con.execute("INSERT INTO tags VALUES (?, ?)", (cur.lastrowid, t))
    print(f"added {cur.lastrowid}")
elif cmd == "list":
    pos, v = parse(rest, {"--tag": "value", "--overdue": "flag", "--today": "value"})
    if pos or ("--overdue" in v) != ("--today" in v): usage("app list [--tag TAG] [--overdue --today DATE]")
    today = parse_date(v["--today"]) if "--today" in v else None
    con = db()
    q, p = "SELECT id, title, due, done FROM todos t WHERE 1", []
    if "--tag" in v:
        q += " AND EXISTS (SELECT 1 FROM tags WHERE todo_id = t.id AND tag = ?)"; p.append(v["--tag"])
    if today:
        q += " AND done = 0 AND due IS NOT NULL AND due < ?"; p.append(today)
    show(con, con.execute(q + " ORDER BY id", p).fetchall())
elif cmd in ("done", "rm"):
    pos, v = parse(rest, {})
    if len(pos) != 1: usage(f"app {cmd} ID")
    tid = parse_id(pos[0])
    con = db()
    need_exists(con, tid)
    with con:
        if cmd == "done":
            con.execute("UPDATE todos SET done = 1 WHERE id = ?", (tid,)); print(f"done {tid}")
        else:
            con.execute("DELETE FROM tags WHERE todo_id = ?", (tid,))
            con.execute("DELETE FROM todos WHERE id = ?", (tid,)); print(f"removed {tid}")
elif cmd == "search":
    pos, v = parse(rest, {})
    if len(pos) != 1 or pos[0] == "": usage("app search TEXT")
    con = db()
    show(con, con.execute("SELECT id, title, due, done FROM todos WHERE instr(title, ?) > 0 ORDER BY id",
                          (pos[0],)).fetchall())
else:
    usage()
