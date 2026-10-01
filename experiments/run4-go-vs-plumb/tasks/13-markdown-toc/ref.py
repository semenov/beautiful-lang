#!/usr/bin/env python3
import sys, re, unicodedata

def usage():
    sys.stderr.write("usage: app [--min-level N] [--max-level M] [FILE]\n"); sys.exit(64)

args = sys.argv[1:]; lo, hi, files = 1, 6, []
i = 0
while i < len(args):
    a = args[i]
    if a in ("--min-level", "--max-level"):
        if i + 1 >= len(args) or not re.fullmatch(r"[0-9]+", args[i + 1]): usage()
        if a == "--min-level": lo = int(args[i + 1])
        else: hi = int(args[i + 1])
        i += 2
    elif a.startswith("--"): usage()
    else: files.append(a); i += 1
if len(files) > 1 or not (1 <= lo <= hi <= 6): usage()
if files:
    try: data = open(files[0], "rb").read()
    except OSError:
        sys.stderr.write(f"error: cannot read {files[0]}\n"); sys.exit(2)
else:
    data = sys.stdin.buffer.read()
lines = data.decode("utf-8", "replace").split("\n")
lines = [l[:-1] if l.endswith("\r") else l for l in lines]

HEADING = re.compile(r" {0,3}(#{1,6})(?:[ \t](.*))?")
FENCE = re.compile(r" {0,3}(`{3,}|~{3,})")

def slug_base(text):
    out = []
    for ch in text.lower():
        cat = unicodedata.category(ch)
        if cat[0] in "LM" or cat == "Nd" or ch in "-_": out.append(ch)
        elif ch == " ": out.append("-")
    return "".join(out)

seen = {}
out = []
fence = None  # (char, length)
for line in lines:
    if fence:
        m = re.fullmatch(r" {0,3}(" + re.escape(fence[0]) + r"{%d,})[ \t]*" % fence[1], line)
        if m: fence = None
        continue
    m = FENCE.match(line)
    if m:
        fence = (m.group(1)[0], len(m.group(1))); continue
    m = HEADING.fullmatch(line)
    if not m: continue
    level = len(m.group(1))
    text = (m.group(2) or "").strip(" \t")
    cm = re.search(r"#+$", text)
    if cm and (cm.start() == 0 or text[cm.start() - 1] in " \t"):
        text = text[:cm.start()].rstrip(" \t")
    if not text: continue
    base = slug_base(text)
    k = seen.get(base, 0); seen[base] = k + 1
    slug = base if k == 0 else f"{base}-{k}"
    if lo <= level <= hi:
        out.append("  " * (level - lo) + f"- [{text}](#{slug})\n")
sys.stdout.write("".join(out))
