#!/usr/bin/env python3
import sys, unicodedata
from collections import Counter

def usage():
    sys.stderr.write("usage: app [--top N] [--min-length L] [FILE...]\n"); sys.exit(64)

args = sys.argv[1:]; top, minl, files = 10, 1, []
i = 0
while i < len(args):
    a = args[i]
    if a in ("--top", "--min-length"):
        if i + 1 >= len(args) or not args[i+1].isdigit(): usage()
        v = int(args[i+1])
        if a == "--top": top = v
        else:
            if v < 1: usage()
            minl = v
        i += 2
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
words, cur = [], []
for ch in text + " ":
    if ch == "'" or unicodedata.category(ch).startswith("L"): cur.append(ch)
    else:
        w = "".join(cur).strip("'").lower(); cur = []
        if w and len(w) >= minl: words.append(w)
cnt = Counter(words)
for w, n in sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0]))[:top]:
    print(w, n)
print(f"total {len(words)} unique {len(cnt)}")
