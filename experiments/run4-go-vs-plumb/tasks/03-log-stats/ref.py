#!/usr/bin/env python3
import re, sys

def usage():
    sys.stderr.write("usage: app [--min-count N] [FILE...]\n"); sys.exit(64)

args = sys.argv[1:]; minc, files = 1, []
i = 0
while i < len(args):
    a = args[i]
    if a == "--min-count":
        if i + 1 >= len(args) or not re.fullmatch(r"[0-9]+", args[i + 1]) or int(args[i + 1]) < 1: usage()
        minc = int(args[i + 1]); i += 2
    elif a.startswith("--"): usage()
    else: files.append(a); i += 1
data = b""
if files:
    for f in files:
        try: data += open(f, "rb").read()
        except OSError:
            sys.stderr.write(f"error: cannot read {f}\n"); sys.exit(2)
else:
    data = sys.stdin.buffer.read()
text = data.decode("utf-8", "replace")

LINE = re.compile(r'[^ ]+ [^ ]+ [^ ]+ \[[^\]]*\] "([A-Z]+) ([^ "]+) HTTP/[0-9.]+" ([1-5][0-9][0-9]) (?:[0-9]+|-) ([0-9]+)')
stats = {}; total = mal = 0
lines = text.split("\n")
if lines and lines[-1] == "": lines.pop()
for ln in lines:
    if ln.endswith("\r"): ln = ln[:-1]
    if ln == "": continue
    m = LINE.fullmatch(ln)
    if not m or not (100 <= int(m.group(3)) <= 599):
        mal += 1; continue
    total += 1
    ep = m.group(1) + " " + m.group(2).split("?", 1)[0]
    s = stats.setdefault(ep, [0, []])
    if int(m.group(3)) >= 500: s[0] += 1
    s[1].append(int(m.group(4)))

def pct(v, p):
    n = len(v); return v[(p * n + 99) // 100 - 1]

out = []
for ep, (err, lat) in sorted(stats.items(), key=lambda kv: (-len(kv[1][1]), kv[0])):
    n = len(lat)
    if n < minc: continue
    lat.sort()
    bp = (err * 10000 * 2 + n) // (2 * n)  # round half up of err*10000/n
    out.append(f"{ep} count={n} error_rate={bp // 100}.{bp % 100:02d}% p50={pct(lat, 50)} p95={pct(lat, 95)} max={lat[-1]}")
out.append(f"total={total} malformed={mal}")
sys.stdout.write("\n".join(out) + "\n")
