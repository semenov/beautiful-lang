#!/usr/bin/env python3
import sys, os, stat, hashlib

def usage():
    sys.stderr.write("usage: app [--min-size BYTES] DIR\n"); sys.exit(64)

args = sys.argv[1:]; min_size = 1; dirs = []
i = 0
while i < len(args):
    a = args[i]
    if a == "--min-size":
        if i + 1 >= len(args) or not args[i + 1].isdigit(): usage()
        min_size = int(args[i + 1]); i += 2
    elif a.startswith("--"): usage()
    else: dirs.append(a); i += 1
if len(dirs) != 1: usage()
root = dirs[0]
if not os.path.isdir(root):
    sys.stderr.write(f"error: not a directory: {root}\n"); sys.exit(2)

failed = False
def fail(rel):
    global failed
    failed = True
    sys.stderr.write(f"error: cannot read {rel}\n")

groups = {}
def walk(path, rel):
    try:
        entries = os.listdir(path)
    except OSError:
        fail(rel); return
    for name in entries:
        p = os.path.join(path, name)
        r = name if not rel else rel + "/" + name
        try:
            st = os.lstat(p)
        except OSError:
            fail(r); continue
        if stat.S_ISDIR(st.st_mode):
            walk(p, r)
        elif stat.S_ISREG(st.st_mode):
            if st.st_size < min_size: continue
            hs = hashlib.sha256(); size = 0
            try:
                with open(p, "rb") as f:
                    while True:
                        b = f.read(1 << 16)
                        if not b: break
                        hs.update(b); size += len(b)
            except OSError:
                fail(r); continue
            groups.setdefault((size, hs.hexdigest()), []).append(r)
walk(root, "")

out = []; G = F = W = 0
for (size, hx), paths in sorted(groups.items(), key=lambda kv: (-kv[0][0], kv[0][1])):
    if len(paths) < 2: continue
    G += 1; F += len(paths); W += size * (len(paths) - 1)
    out.append(f"sha256:{hx} size {size}\n")
    for p in sorted(paths, key=lambda s: s.encode()): out.append(f"  {p}\n")
    out.append("\n")
out.append(f"groups {G} files {F} wasted {W}\n")
sys.stdout.write("".join(out))
sys.exit(1 if failed else 0)
