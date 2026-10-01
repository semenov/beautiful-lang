#!/usr/bin/env python3
import json, sys

def usage():
    sys.stderr.write("usage: app [--where PATH=VALUE]... [--select PATH,PATH...]\n"); sys.exit(64)

def path(p):
    keys = p.split(".")
    if any(k == "" for k in keys): usage()
    return keys

args = sys.argv[1:]; wheres, select = [], None
i = 0
while i < len(args):
    a = args[i]
    if a in ("--where", "--select") and i + 1 < len(args):
        v = args[i + 1]; i += 2
        if a == "--where":
            if "=" not in v: usage()
            p, val = v.split("=", 1); wheres.append((path(p), val))
        else:
            select = [(p, path(p)) for p in v.split(",")]
    else: usage()

class Num(str): pass
class Obj(list): pass

def bad_const(s): raise ValueError(s)

def find(obj, keys):
    cur = obj
    for k in keys:
        if not isinstance(cur, Obj): return None, False
        for kk, vv in cur:
            if kk == k: cur = vv; break
        else: return None, False
    return cur, True

def text_of(v):
    if isinstance(v, str) and not isinstance(v, Num): return v
    if isinstance(v, Num): return str(v)
    if v is True: return "true"
    if v is False: return "false"
    if v is None: return "null"
    return None  # object / array

ESC = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r"}
def s(x):
    return '"' + "".join(ESC.get(ch) or (f"\\u{ord(ch):04x}" if ord(ch) < 0x20 else ch) for ch in x) + '"'

def dump(v):
    if isinstance(v, Num): return str(v)
    if isinstance(v, str): return s(v)
    if v is True: return "true"
    if v is False: return "false"
    if v is None: return "null"
    if isinstance(v, Obj): return "{" + ",".join(s(k) + ":" + dump(x) for k, x in v) + "}"
    return "[" + ",".join(dump(x) for x in v) + "]"

failed = False; out = []
data = sys.stdin.buffer.read().decode("utf-8", "replace")
lines = data.split("\n")
for n, line in enumerate(lines, 1):
    if line.strip(" \t\r\n") == "": continue
    try:
        v = json.loads(line, parse_float=Num, parse_int=Num, parse_constant=bad_const, object_pairs_hook=Obj)
    except ValueError:
        sys.stderr.write(f"error: line {n}: invalid JSON\n"); failed = True; continue
    if not isinstance(v, Obj):
        sys.stderr.write(f"error: line {n}: not an object\n"); failed = True; continue
    ok = True
    for keys, val in wheres:
        x, found = find(v, keys)
        if not found or text_of(x) != val: ok = False; break
    if not ok: continue
    if select is None: out.append(dump(v))
    else:
        out.append("{" + ",".join(s(p) + ":" + dump(find(v, keys)[0]) for p, keys in select) + "}")
sys.stdout.write("".join(o + "\n" for o in out))
sys.exit(1 if failed else 0)
