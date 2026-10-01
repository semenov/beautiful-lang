import sys, os, json
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

for k in list(os.environ):
    if k.startswith("APP_"):
        del os.environ[k]
c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def files(**named):
    for n, text in named.items():
        (c.work / (n + ".json")).write_text(text)
    return [n + ".json" for n in named]

def pretty(v):
    return json.dumps(v, indent=2, sort_keys=True, ensure_ascii=False) + "\n"

OK = '{"service": {"name": "svc"}, "db": {"host": "h", "port": 5432}}'

expect("example",
       c.run(files(base='{"service": {"name": "api", "tags": ["a", "b"]}, "db": {"host": "db1", "port": 5432}}',
                    prod='{"service": {"tags": ["prod"]}, "db": {"host": "db.prod"}}'),
             env={"APP_DB__PORT": "5433", "APP_DEBUG": "false"}),
       '{\n  "db": {\n    "host": "db.prod",\n    "port": 5433\n  },\n  "debug": false,\n  "service": {\n'
       '    "name": "api",\n    "tags": [\n      "prod"\n    ]\n  }\n}\n')
expect("deep merge over three files",
       c.run(files(m1=OK[:-1] + ', "a": {"x": 1, "y": {"z": [1, 2], "keep": true}}, "b": {"o": 1}, "c": 3, "d": "s"}',
                   m2='{"a": {"y": {"z": [3]}, "w": 2}, "b": 7, "c": {"n": 1}}',
                   m3='{"a": {"x": null}, "d": [], "e": {}}')),
       pretty({"service": {"name": "svc"}, "db": {"host": "h", "port": 5432},
               "a": {"x": None, "y": {"z": [3], "keep": True}, "w": 2}, "b": 7, "c": {"n": 1}, "d": [], "e": {}}))
expect("env values: JSON when valid, else string",
       c.run(files(e1=OK), env={"APP_A": "12", "APP_B": "localhost", "APP_C": '"quoted"', "APP_D": "[1, {\"k\": null}]",
                               "APP_E": "", "APP_F": "{", "APP_G": " true ", "APP_H": "{\"in\": 1}", "APP_I": "nul"}),
       pretty({"service": {"name": "svc"}, "db": {"host": "h", "port": 5432}, "a": 12, "b": "localhost",
               "c": "quoted", "d": [1, {"k": None}], "e": "", "f": "{", "g": True, "h": {"in": 1}, "i": "nul"}))
expect("env paths: nesting, lowercase, single underscore, replacing non-objects",
       c.run(files(e2=OK[:-1] + ', "cache": 5, "log": {"x": 1}}'),
             env={"APP_DB__HOST": "db.local", "APP_CACHE__TTL__SECONDS": "30", "APP_LOG__FILE_PATH": "/var/log/x",
                  "APP_NEW__DEEP__KEY": "v"}),
       pretty({"service": {"name": "svc"}, "db": {"host": "db.local", "port": 5432}, "cache": {"ttl": {"seconds": 30}},
               "log": {"x": 1, "file_path": "/var/log/x"}, "new": {"deep": {"key": "v"}}}))
expect("env applied in name order; empty keys ignored",
       c.run(files(e3=OK), env={"APP_DB__PORT": "7", "APP_DB": '{"host": "h1", "port": 1}', "APP_db__host": "low",
                               "APP_": "1", "APP_X__": "1", "APP___Y": "1", "APPX": "1", "app_z": "1"}),
       pretty({"service": {"name": "svc"}, "db": {"host": "low", "port": 7}}))
expect("keys sorted by code point, unicode and escapes",
       c.run(files(u='{"service": {"name": "s\\u00e9rvice \\"x\\"\\t\\\\ \\u0001 /"}, "db": {"host": "h", "port": 1},'
                     ' "b": 1, "B": 2, "é": 3, "z": 4, "日本": [{"b": 1, "a": []}, [], "x"]}')),
       '{\n  "B": 2,\n  "b": 1,\n  "db": {\n    "host": "h",\n    "port": 1\n  },\n'
       '  "service": {\n    "name": "sérvice \\"x\\"\\t\\\\ \\u0001 /"\n  },\n  "z": 4,\n  "é": 3,\n'
       '  "日本": [\n    {\n      "a": [],\n      "b": 1\n    },\n    [],\n    "x"\n  ]\n}\n')
expect("numbers keep their text",
       c.run(files(n=OK[:-1] + ', "r": 1.50, "big": 12345678901234567890, "e": 1E+5, "neg": -0.0}'),
             env={"APP_RATIO": "0.10", "APP_HUGE": "1e400"}),
       '{\n  "big": 12345678901234567890,\n  "db": {\n    "host": "h",\n    "port": 5432\n  },\n  "e": 1E+5,\n'
       '  "huge": 1e400,\n  "neg": -0.0,\n  "r": 1.50,\n  "ratio": 0.10,\n  "service": {\n    "name": "svc"\n  }\n}\n')
expect("port bounds 1 and 65535 accepted",
       c.run(files(p1='{"service": {"name": "a"}, "db": {"host": "h", "port": 1}}'), env={"APP_DB__PORT": "65535"}),
       pretty({"service": {"name": "a"}, "db": {"host": "h", "port": 65535}}))

r = c.run(files(v1='{"other": 1}'))
c.case("all required keys missing", r.code == 1 and r.out == "" and r.err.splitlines() ==
       ["error: invalid service.name", "error: invalid db.host", "error: invalid db.port"], r)
for name, doc, env, bad in [
    ("empty name, numeric host", '{"service": {"name": ""}, "db": {"host": 5, "port": 80}}', {},
     ["service.name", "db.host"]),
    ("port 0", '{"service": {"name": "a"}, "db": {"host": "h", "port": 0}}', {}, ["db.port"]),
    ("port 65536", '{"service": {"name": "a"}, "db": {"host": "h", "port": 65536}}', {}, ["db.port"]),
    ("port with fraction", '{"service": {"name": "a"}, "db": {"host": "h", "port": 5432.0}}', {}, ["db.port"]),
    ("port as string", OK, {"APP_DB__PORT": '"5433"'}, ["db.port"]),
    ("null replaces required value", OK, {"APP_SERVICE__NAME": "null", "APP_DB__PORT": "-1"},
     ["service.name", "db.port"]),
]:
    r = c.run(files(v2=doc), env=env)
    c.case("invalid: " + name, r.code == 1 and r.out == "" and r.err.splitlines() == [f"error: invalid {k}" for k in bad], r)

files(good=OK, bad='{"a": 1,}', arr='[1, 2]', nan='{"a": NaN}')
r = c.run(["good.json", "missing.json", "bad.json"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read missing.json" in r.err and "bad.json" not in r.err, r)
r = c.run(["good.json", "bad.json", "missing.json"])
c.case("invalid JSON", r.code == 2 and r.out == "" and "error: invalid JSON in bad.json" in r.err and "missing" not in r.err, r)
r = c.run(["nan.json"])
c.case("NaN is not JSON", r.code == 2 and r.out == "" and "error: invalid JSON in nan.json" in r.err, r)
r = c.run(["good.json", "arr.json"])
c.case("top level not an object", r.code == 2 and r.out == "" and "error: arr.json is not an object" in r.err, r)
for args in ([], ["--verbose", "good.json"]):
    r = c.run(args)
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

layers = []
want = {"service": {"name": "svc"}, "db": {"host": "h", "port": 5432}}
for i in range(20):
    layer = {f"k{j}": {"v": i, f"only{i}": j} for j in range(i * 500, i * 500 + 2000)}
    layers.append(files(**{f"big{i}": json.dumps(layer)})[0])
    for k, v in layer.items():
        want.setdefault(k, {}).update(v)
expect("many large files", c.run(files(bigbase=OK) + layers, timeout=30), pretty(want))
c.finish()
