import sys, os, shutil, hashlib
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
n = [0]

def tree(files, links=()):
    """files: {relpath: bytes}; links: [(relpath, target)]. Returns the dir name (relative to workdir)."""
    n[0] += 1
    name = f"t{n[0]}"
    root = c.work / name
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode())
    for rel, target in links:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(target, p)
    return name

def h(data):
    return hashlib.sha256(data if isinstance(data, bytes) else data.encode()).hexdigest()

def group(data, paths):
    size = len(data if isinstance(data, bytes) else data.encode())
    return f"sha256:{h(data)} size {size}\n" + "".join(f"  {p}\n" for p in paths) + "\n"

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

d = tree({"a.txt": "hello", "sub/b.txt": "hello", "c.txt": "other"})
expect("example", c.run([d]), group("hello", ["a.txt", "sub/b.txt"]) + "groups 1 files 2 wasted 5\n")
expect("trailing slash on DIR", c.run([d + "/"]), group("hello", ["a.txt", "sub/b.txt"]) + "groups 1 files 2 wasted 5\n")

d = tree({"a": "1", "b": "2", "c/d": "3"})
expect("no duplicates", c.run([d]), "groups 0 files 0 wasted 0\n")

# Two groups of size 3 (ordered by hash), one of size 10 first; paths sorted.
x, y, big = "xxx", "yyy", "0123456789"
d = tree({"z/1": x, "a/2": x, "m": y, "k": y, "q/big1": big, "big2": big, "single": "abc"})
gs = sorted([(x, ["a/2", "z/1"]), (y, ["k", "m"])], key=lambda g: h(g[0]))
expect("group order and path order", c.run([d]),
       group(big, ["big2", "q/big1"]) + "".join(group(*g) for g in gs) + "groups 3 files 6 wasted 16\n")

d = tree({"a": "same", "b/c": "same", "b/d/e": "same"})
expect("three files in a group", c.run([d]), group("same", ["a", "b/c", "b/d/e"]) + "groups 1 files 3 wasted 8\n")

d = tree({"e1": "", "e2": "", "f1": "data", "f2": "data"})
expect("empty files ignored by default", c.run([d]), group("data", ["f1", "f2"]) + "groups 1 files 2 wasted 4\n")
expect("min-size 0 includes empty files", c.run(["--min-size", "0", d]),
       group("data", ["f1", "f2"]) + group("", ["e1", "e2"]) + "groups 2 files 4 wasted 4\n")

d = tree({"s1": "short", "s2": "short", "l1": "longer!", "l2": "longer!"})
expect("min-size filters smaller files", c.run([d, "--min-size", "6"]), group("longer!", ["l1", "l2"]) + "groups 1 files 2 wasted 7\n")

d = tree({"real/a": "dup", "real/b": "dup", "only": "unique!"},
         links=[("link-to-file", "real/a"), ("link-to-dir", "real"), ("dangling", "nowhere")])
expect("symlinks skipped and not followed", c.run([d]), group("dup", ["real/a", "real/b"]) + "groups 1 files 2 wasted 3\n")

d = tree({".hidden": "secret", ".cfg/x": "secret", "visible": "secret"})
expect("hidden files included", c.run([d]), group("secret", [".cfg/x", ".hidden", "visible"]) + "groups 1 files 3 wasted 12\n")

d = tree({"Zebra": "u", "été.txt": "u", "a b.txt": "u", "Ärger/x": "u", "日本.txt": "u"})
expect("unicode names sorted by bytes", c.run([d]),
       group("u", ["Zebra", "a b.txt", "Ärger/x", "été.txt", "日本.txt"]) + "groups 1 files 5 wasted 4\n")

d = tree({"a": "A" * 1000 + "1", "b": "A" * 1000 + "2", "c": "B" + "A" * 1000})
expect("same size, different content", c.run([d]), "groups 0 files 0 wasted 0\n")

# Unreadable file and directory: reported, skipped, exit 1.
d = tree({"ok1": "fine", "ok2": "fine", "locked": "fine", "closed/inner": "fine"})
os.chmod(c.work / d / "locked", 0)
os.chmod(c.work / d / "closed", 0)
r = c.run([d])
c.case("unreadable file reported, others still grouped",
       r.code == 1 and r.out == group("fine", ["ok1", "ok2"]) + "groups 1 files 2 wasted 4\n"
       and "error: cannot read locked" in r.err, r)
c.case("unreadable directory reported", r.code == 1 and "error: cannot read closed" in r.err, r)
os.chmod(c.work / d / "closed", 0o755)

# Large files: identical except two differ only in the last byte.
d = tree({})
block = os.urandom(1 << 20)
for name, last in (("big/a.bin", b"\x00"), ("big/b.bin", b"\x00"), ("c.bin", b"\x01")):
    p = c.work / d / name
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, "wb") as f:
        for _ in range(24):
            f.write(block)
        f.write(last)
data = block * 24 + b"\x00"
expect("large files", c.run([d], timeout=30),
       f"sha256:{h(data)} size {len(data)}\n  big/a.bin\n  big/b.bin\n\ngroups 1 files 2 wasted {len(data)}\n")

# Many files: 600 pairs plus 300 unique, in nested dirs.
files = {}
for i in range(600):
    files[f"d{i % 7}/p{i:04d}a"] = f"content-{i}"
    files[f"e{i % 5}/sub/p{i:04d}b"] = f"content-{i}"
for i in range(300):
    files[f"u/{i}"] = f"unique-{i}"
d = tree(files)
groups = []
for i in range(600):
    data = f"content-{i}"
    groups.append((-len(data), h(data), group(data, sorted([f"d{i % 7}/p{i:04d}a", f"e{i % 5}/sub/p{i:04d}b"]))))
groups.sort()
expect("many files", c.run([d], timeout=30),
       "".join(g[2] for g in groups) + f"groups 600 files 1200 wasted {sum(-g[0] for g in groups)}\n")

r = c.run(["no-such-dir"])
c.case("missing DIR", r.code == 2 and r.out == "" and "error: not a directory: no-such-dir" in r.err, r)
(c.work / "plain-file").write_text("x")
r = c.run(["plain-file"])
c.case("DIR is a file", r.code == 2 and r.out == "" and "error: not a directory: plain-file" in r.err, r)

d = tree({"a": "1"})
for args in ([], [d, d], ["--min-size", "-1", d], ["--min-size", "x", d], ["--bogus", d], [d, "--min-size"]):
    r = c.run(args)
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)
c.finish()
