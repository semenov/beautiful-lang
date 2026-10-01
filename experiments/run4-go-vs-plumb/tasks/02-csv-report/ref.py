#!/usr/bin/env python3
import re, sys

def usage():
    sys.stderr.write("usage: app --by COLUMN [FILE]\n"); sys.exit(64)

args = sys.argv[1:]; by = None; files = []
i = 0
while i < len(args):
    a = args[i]
    if a == "--by":
        if i + 1 >= len(args): usage()
        by = args[i + 1]; i += 2
    elif a.startswith("--"): usage()
    else: files.append(a); i += 1
if by is None or len(files) > 1: usage()
if files:
    try: data = open(files[0], "rb").read()
    except OSError:
        sys.stderr.write(f"error: cannot read {files[0]}\n"); sys.exit(2)
else:
    data = sys.stdin.buffer.read()
text = data.decode("utf-8", "replace")

def records(t):
    """Yields (line, fields or None, reason)."""
    n = len(t); i = 0; line = 1
    while i < n:
        start = line
        # empty line
        if t[i] == "\n": i += 1; line += 1; continue
        if t.startswith("\r\n", i): i += 2; line += 1; continue
        fields = []; bad = None
        while True:
            if i < n and t[i] == '"':
                i += 1; buf = []
                while True:
                    if i >= n:
                        yield start, None, "unterminated quote"; return
                    ch = t[i]
                    if ch == '"':
                        if i + 1 < n and t[i + 1] == '"': buf.append('"'); i += 2; continue
                        i += 1; break
                    if ch == "\n": line += 1
                    buf.append(ch); i += 1
                fields.append("".join(buf))
                if i < n and t[i] == ",": i += 1; continue
                if i >= n: break
                if t[i] == "\n": i += 1; line += 1; break
                if t.startswith("\r\n", i): i += 2; line += 1; break
                bad = "bad quoting"
                while i < n and t[i] != "\n": i += 1
                if i < n: i += 1; line += 1
                break
            else:
                j = i
                while j < n and t[j] not in ",\n" and not t.startswith("\r\n", j): j += 1
                fields.append(t[i:j]); i = j
                if i < n and t[i] == ",": i += 1; continue
                if i >= n: break
                if t[i] == "\n": i += 1; line += 1; break
                i += 2; line += 1; break
        yield start, (None if bad else fields), bad

AMT = re.compile(r"(-?)([0-9]+)(?:\.([0-9]{1,2}))?")
def cents(s):
    m = AMT.fullmatch(s.strip(" "))
    if not m: return None
    v = int(m.group(2)) * 100 + int((m.group(3) or "").ljust(2, "0"))
    return -v if m.group(1) else v

def q(f):
    if any(c in f for c in ',"\r\n'): return '"' + f.replace('"', '""') + '"'
    return f

def money(c):
    s = "-" if c < 0 else ""; c = abs(c)
    return f"{s}{c // 100}.{c % 100:02d}"

it = records(text)
first = next(it, None)
if first is None:
    sys.stderr.write("error: empty input\n"); sys.exit(2)
if first[1] is None:
    sys.stderr.write("error: empty input\n"); sys.exit(2)
header = first[1]
for col in ("amount", by):
    if col not in header:
        sys.stderr.write(f"error: missing column {col}\n"); sys.exit(2)
ai, bi = header.index("amount"), header.index(by)
totals = {}; bad = False
for line, fields, reason in it:
    if fields is not None and len(fields) != len(header):
        reason = f"expected {len(header)} fields, got {len(fields)}"; fields = None
    if fields is not None:
        c = cents(fields[ai])
        if c is None: reason = f"invalid amount {fields[ai]!r}"; fields = None
    if fields is None:
        sys.stderr.write(f"line {line}: {reason}\n"); bad = True; continue
    totals[fields[bi]] = totals.get(fields[bi], 0) + c
out = [q(by) + ",total"]
for k in sorted(totals):
    out.append(q(k) + "," + money(totals[k]))
sys.stdout.write("\n".join(out) + "\n")
sys.exit(1 if bad else 0)
