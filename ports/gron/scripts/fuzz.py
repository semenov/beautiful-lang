#!/usr/bin/env python3
"""Random JSON (and gron text) to compare the lang port with the Go original.
Usage: fuzz.py OUTDIR N"""
import json, random, sys, os
out, n = sys.argv[1], int(sys.argv[2])
os.makedirs(out, exist_ok=True)
R = random.Random(42)
CHARS = ['a', 'Z', '_', '$', '0', '9', ' ', '-', '.', '"', '\\', '\n', '\t', '\x01', '\x7f', '\x85',
         'é', 'ಠ', ' ', ' ', '😀', 'Ⅻ', '١', '[', ']', '=', ';', '/', '<', '&']
WORDS = ['var', 'true', 'null', 'json', 'a', 'b', 'key', 'x1', '1x', '', 'ಠ_ಠ', '$', '_']
def s():
    if R.random() < .4:
        return R.choice(WORDS)
    return ''.join(R.choice(CHARS) for _ in range(R.randint(0, 6)))
def num():
    return R.choice(['0', '-0', '1', '-1', '1.0', '1e5', '1E+2', '-2.5e-3', '12345678901234567890',
                     '0.1', '3.14159', '1e400', '100', str(R.randint(-10**6, 10**6))])
def val(d):
    r = R.random()
    if d > 5 or r < .35:
        return R.choice([lambda: json.dumps(s(), ensure_ascii=R.random() < .5), num,
                         lambda: 'true', lambda: 'false', lambda: 'null'])()
    if r < .7:
        return '[' + ','.join(val(d + 1) for _ in range(R.randint(0, 4))) + ']'
    return '{' + ','.join(json.dumps(s(), ensure_ascii=R.random() < .5) + ':' + val(d + 1)
                          for _ in range(R.randint(0, 4))) + '}'
for i in range(n):
    doc = val(0)
    ws = R.choice(['', ' ', '\n'])
    with open(f'{out}/{i}.json', 'w') as f:
        f.write(ws + doc + ws)
    # a stream: several documents, one per line
    with open(f'{out}/{i}.stream', 'w') as f:
        f.write('\n'.join(val(1) for _ in range(R.randint(1, 4))) + '\n')
