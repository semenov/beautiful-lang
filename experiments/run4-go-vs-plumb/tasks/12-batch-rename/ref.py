#!/usr/bin/env python3
import sys, os, re, stat

def usage(msg=""):
    sys.stderr.write("usage: app DIR (--regex PATTERN --to TEMPLATE | --counter TEMPLATE [--start N] [--match PATTERN]) [--dry-run] " + msg + "\n")
    sys.exit(64)

def fail(msg):
    sys.stderr.write(f"error: {msg}\n"); sys.exit(1)

args = sys.argv[1:]; opts = {}; pos = []; dry = False
i = 0
while i < len(args):
    a = args[i]
    if a == "--dry-run": dry = True; i += 1
    elif a in ("--regex", "--to", "--counter", "--start", "--match"):
        if i + 1 >= len(args) or a in opts: usage()
        opts[a] = args[i + 1]; i += 2
    elif a.startswith("--"): usage("unknown option " + a)
    else: pos.append(a); i += 1
if len(pos) != 1: usage()
regex_mode = "--regex" in opts
if regex_mode == ("--counter" in opts): usage("give exactly one of --regex and --counter")
if regex_mode:
    if "--to" not in opts or "--start" in opts or "--match" in opts: usage()
else:
    if "--to" in opts: usage()

def compile_re(p):
    try: return re.compile(p)
    except re.error: usage("invalid pattern")

def parse_template(t, ngroups):
    parts = []; i = 0
    while i < len(t):
        ch = t[i]
        if t.startswith("{{", i): parts.append("{"); i += 2
        elif t.startswith("}}", i): parts.append("}"); i += 2
        elif ch == "{":
            m = re.match(r"\{([0-9]+)\}", t[i:])
            if not m or int(m.group(1)) > ngroups: usage("invalid template")
            parts.append(int(m.group(1))); i += len(m.group(0))
        elif ch == "}": usage("invalid template")
        else: parts.append(ch); i += 1
    return parts

if regex_mode:
    rx = compile_re(opts["--regex"])
    tmpl = parse_template(opts["--to"], rx.groups)
else:
    t = opts["--counter"]
    runs = re.findall(r"#+", t)
    if len(runs) != 1: usage("counter template needs exactly one run of #")
    start = opts.get("--start", "1")
    if not re.fullmatch(r"[0-9]+", start): usage("bad --start")
    start = int(start)
    rx = compile_re(opts["--match"]) if "--match" in opts else None

d = pos[0]
if not os.path.isdir(d):
    sys.stderr.write(f"error: not a directory: {d}\n"); sys.exit(2)
entries = os.listdir(d)
cands = sorted(e for e in entries if stat.S_ISREG(os.lstat(os.path.join(d, e)).st_mode))

plan = []
k = 0
for name in cands:
    if regex_mode:
        m = rx.fullmatch(name)
        if not m: continue
        new = "".join(p if isinstance(p, str) else (m.group(p) or "") for p in tmpl)
    else:
        if rx is not None and not rx.fullmatch(name): continue
        width = len(runs[0])
        new = t.replace(runs[0], str(start + k).zfill(width)); k += 1
    if new != name: plan.append((name, new))

existing = set(entries)
seen = set()
for old, new in plan:
    if new in ("", ".", "..") or "/" in new or "\0" in new: fail(f"invalid name: {new}")
for old, new in plan:
    if new in seen: fail(f"conflict: {new}")
    seen.add(new)
for old, new in plan:
    if new in existing: fail(f"exists: {new}")

out = []
for old, new in plan:
    if not dry: os.rename(os.path.join(d, old), os.path.join(d, new))
    out.append(f"{old} -> {new}\n")
out.append(f"{'would rename' if dry else 'renamed'} {len(plan)}\n")
sys.stdout.write("".join(out))
