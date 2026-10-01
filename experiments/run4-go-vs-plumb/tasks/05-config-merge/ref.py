#!/usr/bin/env python3
import json, os, re, sys

def usage():
    sys.stderr.write("usage: app FILE...\n"); sys.exit(64)

files = sys.argv[1:]
if not files or any(f.startswith("-") for f in files): usage()

class Num(str): pass

def bad_const(s): raise ValueError(s)

def parse(text):
    return json.loads(text, parse_float=Num, parse_int=Num, parse_constant=bad_const)

def fail(msg):
    sys.stderr.write(msg + "\n"); sys.exit(2)

def merge(a, b):
    for k, v in b.items():
        if isinstance(a.get(k), dict) and isinstance(v, dict): merge(a[k], v)
        else: a[k] = v

result = None
for f in files:
    try: raw = open(f, "rb").read()
    except OSError: fail(f"error: cannot read {f}")
    try: v = parse(raw.decode("utf-8"))
    except ValueError: fail(f"error: invalid JSON in {f}")
    if not isinstance(v, dict): fail(f"error: {f} is not an object")
    if result is None: result = v
    else: merge(result, v)

env = os.environb if hasattr(os, "environb") else {k.encode(): v.encode() for k, v in os.environ.items()}
for name in sorted(k for k in env if k.startswith(b"APP_")):
    keys = [k.decode().translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))
            for k in name[4:].split(b"__")]
    if any(k == "" for k in keys): continue
    sval = env[name].decode("utf-8", "replace")
    try: val = parse(sval)
    except ValueError: val = sval
    cur = result
    for k in keys[:-1]:
        if not isinstance(cur.get(k), dict): cur[k] = {}
        cur = cur[k]
    cur[keys[-1]] = val

def get(path):
    cur = result
    for k in path.split("."):
        if not isinstance(cur, dict) or k not in cur: return None, False
        cur = cur[k]
    return cur, True

def nonempty_str(v): return type(v) is str and v != ""
def port(v): return isinstance(v, Num) and re.fullmatch(r"[0-9]+", v) is not None and 1 <= int(v) <= 65535
errors = []
for key, ok in (("service.name", nonempty_str), ("db.host", nonempty_str), ("db.port", port)):
    v, found = get(key)
    if not found or not ok(v): errors.append(f"error: invalid {key}")
if errors:
    sys.stderr.write("\n".join(errors) + "\n"); sys.exit(1)

ESC = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r"}
def s(x):
    return '"' + "".join(ESC.get(ch) or (f"\\u{ord(ch):04x}" if ord(ch) < 0x20 else ch) for ch in x) + '"'

def dump(v, ind):
    if isinstance(v, Num): return str(v)
    if isinstance(v, str): return s(v)
    if v is True: return "true"
    if v is False: return "false"
    if v is None: return "null"
    pad = "  " * (ind + 1); end = "  " * ind
    if isinstance(v, dict):
        if not v: return "{}"
        return "{\n" + ",\n".join(pad + s(k) + ": " + dump(v[k], ind + 1) for k in sorted(v)) + "\n" + end + "}"
    if not v: return "[]"
    return "[\n" + ",\n".join(pad + dump(x, ind + 1) for x in v) + "\n" + end + "]"

sys.stdout.write(dump(result, 0) + "\n")
