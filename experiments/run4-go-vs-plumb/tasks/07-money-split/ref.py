#!/usr/bin/env python3
import re, sys

def usage():
    sys.stderr.write("usage: app [FILE]\n"); sys.exit(64)

args = sys.argv[1:]
if len(args) > 1 or any(a.startswith("-") for a in args): usage()
if args:
    try: data = open(args[0], "rb").read()
    except OSError:
        sys.stderr.write(f"error: cannot read {args[0]}\n"); sys.exit(2)
else:
    data = sys.stdin.buffer.read()
text = data.decode("utf-8", "replace")

NAME = r"[^ \t,:#]+"
AMT = re.compile(r"([0-9]+)(?:\.([0-9]{1,2}))?")
PART = re.compile(rf"({NAME})(?::([0-9]+))?")

def bad(n, why):
    sys.stderr.write(f"error: line {n}: {why}\n"); sys.exit(1)

bal = {}
lines = text.split("\n")
if lines and lines[-1] == "": lines.pop()
for n, ln in enumerate(lines, 1):
    if ln.endswith("\r"): ln = ln[:-1]
    s = ln.strip(" \t")
    if s == "" or s.startswith("#"): continue
    f = re.split(r"[ \t]+", s)
    if len(f) != 3: bad(n, "expected PAYER AMOUNT SPLIT")
    payer, amount, split = f
    if not re.fullmatch(NAME, payer): bad(n, "bad payer")
    m = AMT.fullmatch(amount)
    if not m: bad(n, "bad amount")
    t = int(m.group(1)) * 100 + int((m.group(2) or "").ljust(2, "0"))
    if t <= 0 or t > 100000000000: bad(n, "amount out of range")
    parts = []
    for p in split.split(","):
        pm = PART.fullmatch(p)
        if not pm: bad(n, "bad participant")
        sh = int(pm.group(2)) if pm.group(2) is not None else 1
        if not 1 <= sh <= 1000: bad(n, "bad shares")
        if any(q == pm.group(1) for q, _ in parts): bad(n, "duplicate participant")
        parts.append((pm.group(1), sh))
    S = sum(sh for _, sh in parts)
    charges = [t * sh // S for _, sh in parts]
    r = t - sum(charges)
    for i in range(r): charges[i] += 1
    bal[payer] = bal.get(payer, 0) + t
    for (name, _), ch in zip(parts, charges):
        bal[name] = bal.get(name, 0) - ch

def money(c, sign):
    s = ("+" if c > 0 else "-" if c < 0 else "") if sign else ""
    c = abs(c); return f"{s}{c // 100}.{c % 100:02d}"

out = ["balances:"] + [f"  {k} {money(bal[k], True)}" for k in sorted(bal)] + ["transfers:"]
b = dict(bal)
while True:
    debt = [k for k in b if b[k] < 0]; cred = [k for k in b if b[k] > 0]
    if not debt: break
    d = min(debt, key=lambda k: (b[k], k)); c = min(cred, key=lambda k: (-b[k], k))
    x = min(-b[d], b[c]); b[d] += x; b[c] -= x
    out.append(f"  {d} -> {c} {money(x, False)}")
sys.stdout.write("\n".join(out) + "\n")
