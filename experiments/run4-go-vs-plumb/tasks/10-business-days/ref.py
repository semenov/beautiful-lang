#!/usr/bin/env python3
import sys, re, datetime

DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]

def usage(msg=""):
    sys.stderr.write("usage: app between START END | add DATE N [--holidays FILE] [--weekend DAYS] " + msg + "\n")
    sys.exit(64)

def err(msg):
    sys.stderr.write(f"error: {msg}\n"); sys.exit(2)

def date_of(s):
    if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", s): return None
    try: return datetime.date(int(s[:4]), int(s[5:7]), int(s[8:]))
    except ValueError: return None

args = sys.argv[1:]
if not args or args[0] not in ("between", "add"): usage()
cmd = args[0]; pos = []; hol_file = None; weekend_s = "sat,sun"
i = 1
while i < len(args):
    a = args[i]
    if a in ("--holidays", "--weekend"):
        if i + 1 >= len(args): usage()
        if a == "--holidays": hol_file = args[i + 1]
        else: weekend_s = args[i + 1]
        i += 2
    elif a.startswith("--"): usage()
    else: pos.append(a); i += 1
if len(pos) != 2: usage()
weekend = set()
if weekend_s != "":
    for name in weekend_s.split(","):
        if name not in DAYS: usage("unknown day " + name)
        weekend.add(DAYS.index(name))
if len(weekend) == 7: usage("no business days")
if cmd == "add" and not re.fullmatch(r"-?[0-9]+", pos[1]): usage("N must be a whole number")

dates = []
for s in (pos if cmd == "between" else pos[:1]):
    d = date_of(s)
    if d is None: err(f"invalid date: {s}")
    dates.append(d)

holidays = set()
if hol_file is not None:
    try: lines = open(hol_file, "rb").read().decode("utf-8", "replace").split("\n")
    except OSError: err(f"cannot read {hol_file}")
    for no, line in enumerate(lines, 1):
        s = line.split("#", 1)[0].strip()
        if not s: continue
        d = date_of(s)
        if d is None: err(f"{hol_file}:{no}: invalid date")
        holidays.add(d)

def business(d): return d.weekday() not in weekend and d not in holidays
ONE = datetime.timedelta(1)

if cmd == "between":
    a, b = dates; sign = 1
    if b < a: a, b, sign = b, a, -1
    n = 0
    while a < b:
        if business(a): n += 1
        a += ONE
    print(sign * n)
else:
    d, n = dates[0], int(pos[1])
    if n == 0:
        while not business(d): d += ONE
    else:
        step = ONE if n > 0 else -ONE; left = abs(n)
        while left:
            d += step
            if business(d): left -= 1
    print(d.isoformat())
