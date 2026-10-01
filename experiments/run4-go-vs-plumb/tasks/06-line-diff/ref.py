#!/usr/bin/env python3
import sys
from array import array

def usage():
    sys.stderr.write("usage: app [-U N] OLD NEW\n"); sys.exit(2)

args = sys.argv[1:]; ctx = 3
if args and args[0] == "-U":
    if len(args) < 2 or not args[1].isascii() or not args[1].isdigit(): usage()
    ctx = int(args[1]); args = args[2:]
if len(args) != 2 or any(a.startswith("-") and a != "-" for a in args): usage()

def lines(path):
    try: data = open(path, "rb").read()
    except OSError:
        sys.stderr.write(f"error: cannot read {path}\n"); sys.exit(2)
    return _split(data)

def _split(data):
    out, s = [], 0
    while s < len(data):
        e = data.find(b"\n", s)
        if e < 0: out.append(data[s:]); break
        out.append(data[s:e + 1]); s = e + 1
    return out

A = lines(args[0]); B = lines(args[1])
n, m = len(A), len(B)

p = 0
while p < n and p < m and A[p] == B[p]: p += 1
# L over suffixes starting at p
rows, cols = n - p, m - p
L = [array("i", [0]) * (cols + 1) for _ in range(rows + 1)]
for i in range(rows - 1, -1, -1):
    Li, Ln, a = L[i], L[i + 1], A[p + i]
    for j in range(cols - 1, -1, -1):
        if a == B[p + j]: Li[j] = Ln[j + 1] + 1
        else:
            x, y = Ln[j], Li[j + 1]
            Li[j] = x if x >= y else y

ops = []  # (tag, line, old_index, new_index)
for k in range(p): ops.append((" ", A[k], k, k))
i = j = 0
while i < rows or j < cols:
    if i < rows and j < cols and A[p + i] == B[p + j]:
        ops.append((" ", A[p + i], p + i, p + j)); i += 1; j += 1
    elif j == cols or (i < rows and L[i + 1][j] >= L[i][j + 1]):
        ops.append(("-", A[p + i], p + i, p + j)); i += 1
    else:
        ops.append(("+", B[p + j], p + i, p + j)); j += 1

changes = [k for k, o in enumerate(ops) if o[0] != " "]
if not changes: sys.exit(0)

groups = []
start = changes[0]; last = changes[0]
for k in changes[1:]:
    if k - last - 1 > 2 * ctx:
        groups.append((start, last)); start = k
    last = k
groups.append((start, last))

def rng(s, c):
    if c == 1: return f"{s + 1}"
    if c == 0: return f"{s},0"
    return f"{s + 1},{c}"

out = [f"--- {args[0]}\n".encode(), f"+++ {args[1]}\n".encode()]
for s, e in groups:
    s = max(0, s - ctx); e = min(len(ops) - 1, e + ctx)
    h = ops[s:e + 1]
    c1 = sum(1 for o in h if o[0] != "+"); c2 = sum(1 for o in h if o[0] != "-")
    out.append(f"@@ -{rng(h[0][2], c1)} +{rng(h[0][3], c2)} @@\n".encode())
    for tag, line, _, _ in h:
        out.append(tag.encode() + line)
        if not line.endswith(b"\n"): out.append(b"\n\\ No newline at end of file\n")
sys.stdout.buffer.write(b"".join(out))
sys.exit(1)
