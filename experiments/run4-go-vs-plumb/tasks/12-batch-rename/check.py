import sys, os, shutil
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
n = [0]

def tree(names, dirs=(), links=()):
    """Creates a fresh directory with files (content = own name). Returns its name relative to the workdir."""
    n[0] += 1
    name = f"d{n[0]}"
    root = c.work / name
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    for f in names:
        (root / f).write_text("content of " + f)
    for d in dirs:
        (root / d).mkdir()
        (root / d / "inner.txt").write_text("inner")
    for l, target in links:
        os.symlink(target, root / l)
    return name

def listing(d):
    """{name: content-or-kind} of everything in d (one level, plus subdir contents)."""
    out = {}
    root = c.work / d
    for e in os.listdir(root):
        p = root / e
        if p.is_symlink():
            out[e] = "link"
        elif p.is_dir():
            out[e] = "dir:" + ",".join(sorted(os.listdir(p)))
        else:
            out[e] = p.read_text()
    return out

def files(mapping):
    """Expected listing: {new_name: original_name}."""
    return {new: "content of " + old for new, old in mapping.items()}

def expect(name, r, out, d, after, code=0, err=None):
    got = listing(d)
    ok = r.code == code and r.out == out and got == after and (err is None or err in r.err)
    c.case(name, ok, (r, got))

d = tree(["IMG_0003.JPG", "IMG_0012.JPG", "notes.txt"])
r1 = c.run([d, "--regex", r"IMG_(\d+)\.JPG", "--to", "photo-{1}.jpg"])
r2 = c.run([d, "--counter", "pic-###.jpg", "--match", r".*\.jpg", "--start", "9"])
got = listing(d)
c.case("example", r1.code == 0 and r1.out == "IMG_0003.JPG -> photo-0003.jpg\nIMG_0012.JPG -> photo-0012.jpg\nrenamed 2\n"
       and r2.code == 0 and r2.out == "photo-0003.jpg -> pic-009.jpg\nphoto-0012.jpg -> pic-010.jpg\nrenamed 2\n"
       and got == files({"pic-009.jpg": "IMG_0003.JPG", "pic-010.jpg": "IMG_0012.JPG", "notes.txt": "notes.txt"}),
       (r1, r2, got))

d = tree(["a1.txt", "a2.txt"])
expect("dry run changes nothing", c.run(["--dry-run", "--regex", r"a(\d)\.txt", "--to", "b{1}.txt", d]),
       "a1.txt -> b1.txt\na2.txt -> b2.txt\nwould rename 2\n", d, files({"a1.txt": "a1.txt", "a2.txt": "a2.txt"}))

d = tree(["123", "a123", "123b", "ab", "a"])
expect("pattern must match the whole name", c.run([d, "--regex", r"\d+|a", "--to", "n-{0}"]),
       "123 -> n-123\na -> n-a\nrenamed 2\n", d,
       files({"n-123": "123", "a123": "a123", "123b": "123b", "ab": "ab", "n-a": "a"}))

d = tree(["x.txt"], dirs=["sub.txt"], links=[("link.txt", "x.txt")])
expect("directories, their contents and links untouched", c.run([d, "--regex", r"(.*)\.txt", "--to", "{1}.md"]),
       "x.txt -> x.md\nrenamed 1\n", d, {"x.md": "content of x.txt", "sub.txt": "dir:inner.txt", "link.txt": "link"})

d = tree(["doc-v2.txt", "readme.txt"])
expect("optional group and literal braces", c.run([d, "--regex", r"([a-z]+)(-v(\d+))?\.txt", "--to", "{1}{{v{3}}}.md"]),
       "doc-v2.txt -> doc{v2}.md\nreadme.txt -> readme{v}.md\nrenamed 2\n", d,
       files({"doc{v2}.md": "doc-v2.txt", "readme{v}.md": "readme.txt"}))

d = tree(["a.log", "b.log", "c.txt"])
expect("unchanged names left out", c.run([d, "--regex", r"(.*)\.(log|txt)", "--to", "{1}.log"]),
       "c.txt -> c.log\nrenamed 1\n", d, files({"a.log": "a.log", "b.log": "b.log", "c.log": "c.txt"}))
d = tree(["a.log", "b.log", "c.txt"])
expect("{0} prefix rename", c.run([d, "--regex", r".*\.log", "--to", "old-{0}"]),
       "a.log -> old-a.log\nb.log -> old-b.log\nrenamed 2\n", d,
       files({"old-a.log": "a.log", "old-b.log": "b.log", "c.txt": "c.txt"}))

d = tree(["apple.txt", "avocado.txt", "banana.txt"])
expect("conflict refuses the whole batch", c.run([d, "--regex", r"(\w)\w*\.txt", "--to", "{1}.txt"]),
       "", d, files({"apple.txt": "apple.txt", "avocado.txt": "avocado.txt", "banana.txt": "banana.txt"}),
       code=1, err="error: conflict: a.txt")

d = tree(["one.txt", "two.txt", "one.md"])
expect("existing target refuses the whole batch", c.run([d, "--regex", r"(.*)\.txt", "--to", "{1}.md"]),
       "", d, files({"one.txt": "one.txt", "two.txt": "two.txt", "one.md": "one.md"}), code=1, err="error: exists: one.md")

d = tree(["f1", "f2", "f3"])
expect("target renamed away in the same batch still counts as existing", c.run([d, "--counter", "f#", "--start", "2"]),
       "", d, files({"f1": "f1", "f2": "f2", "f3": "f3"}), code=1, err="error: exists: f")

d = tree(["data.csv"], dirs=["report.csv"])
expect("existing directory counts", c.run([d, "--regex", r"data\.csv", "--to", "report.csv"]),
       "", d, {"data.csv": "content of data.csv", "report.csv": "dir:inner.txt"}, code=1, err="error: exists: report.csv")

d = tree(["a.txt", ".tmp", "b.txt"])
bad = []
for args, frag in (([d, "--regex", r"(.*)\.txt", "--to", "../{1}.txt"], "error: invalid name: ../"),
                   ([d, "--regex", r"(.*)\.txt", "--to", "{1}/x"], "error: invalid name: "),
                   ([d, "--regex", r"(.*)\.tmp", "--to", "{1}"], "error: invalid name"),
                   ([d, "--regex", r"a\.txt", "--to", ".."], "error: invalid name: .."),
                   ([d, "--counter", "../#"], "error: invalid name: ../")):
    r = c.run(args)
    if not (r.code == 1 and r.out == "" and frag in r.err):
        bad.append((args, r))
outside = sorted(p.name for p in c.work.iterdir() if p.is_file())
got = listing(d)
c.case("names leaving DIR are refused", not bad and got == files({"a.txt": "a.txt", ".tmp": ".tmp", "b.txt": "b.txt"})
       and outside == [], (bad, got, outside))

d = tree(["b", "a", "Z", "c", "d", "e", "f", "g", "h", "i", "j", "k"])
order = sorted(["b", "a", "Z", "c", "d", "e", "f", "g", "h", "i", "j", "k"])
mapping = {f"n-{8 + i}.txt": old for i, old in enumerate(order)}
expect("counter: order, start, longer numbers", c.run([d, "--counter", "n-#.txt", "--start", "8"]),
       "".join(f"{old} -> n-{8 + i}.txt\n" for i, old in enumerate(order)) + "renamed 12\n", d, files(mapping))

d = tree(["x.jpg", "y.png", "z.jpg", "w.gif"])
expect("counter with --match and start 0", c.run([d, "--match", r".*\.(jpg|png)", "--counter", "img_###_x", "--start", "0"]),
       "x.jpg -> img_000_x\ny.png -> img_001_x\nz.jpg -> img_002_x\nrenamed 3\n", d,
       files({"img_000_x": "x.jpg", "img_001_x": "y.png", "img_002_x": "z.jpg", "w.gif": "w.gif"}))

d = tree(["été.txt", "zebra.txt", "日本.txt", "Ärger.txt"])
expect("unicode names", c.run([d, "--regex", r"(.*)\.txt", "--to", "{1}.md"]),
       "zebra.txt -> zebra.md\nÄrger.txt -> Ärger.md\nété.txt -> été.md\n日本.txt -> 日本.md\nrenamed 4\n", d,
       files({"zebra.md": "zebra.txt", "Ärger.md": "Ärger.txt", "été.md": "été.txt", "日本.md": "日本.txt"}))

names = [f"raw{i}.dat" for i in range(500)]
d = tree(names)
order = sorted(names)
expect("500 files", c.run([d, "--counter", "file-####.dat"]),
       "".join(f"{old} -> file-{i + 1:04d}.dat\n" for i, old in enumerate(order)) + "renamed 500\n", d,
       files({f"file-{i + 1:04d}.dat": old for i, old in enumerate(order)}))

r = c.run(["no-such-dir", "--counter", "#"])
c.case("missing DIR", r.code == 2 and r.out == "" and "error: not a directory: no-such-dir" in r.err, r)

d = tree(["a1"])
bad = []
for args in ([d, "--counter", "plain"], [d, "--counter", "#-#"], [d, "--regex", r"a(\d)", "--to", "{2}"],
             [d, "--regex", r"a(\d)", "--to", "x{"], [d, "--regex", r"a(\d)", "--to", "x}"], [d, "--regex", r"a(\d)", "--to", "{x}"],
             [d, "--regex", "(abc", "--to", "x"], [d, "--regex", "a1", "--to", "b", "--counter", "#"], [d],
             [d, "--counter", "#", "--to", "x"], [d, "--regex", "a1", "--to", "b", "--start", "2"],
             [d, "--regex", "a1"], [d, "--counter", "#", "--start", "-1"], [d, "--counter", "#", "--start", "x"],
             [d, "--counter", "#", "--bogus"], ["--counter", "#"]):
    r = c.run(args)
    if not (r.code == 64 and r.err.startswith("usage:") and r.out == ""):
        bad.append((args, r))
c.case("usage errors, nothing renamed", not bad and listing(d) == files({"a1": "a1"}), bad)
c.finish()
