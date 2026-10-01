import sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

def errs(err):
    return re.findall(r"(?m)^error: line (\d+): (invalid JSON|not an object)", err)

r = c.run(["--where", "user.name=Ann", "--select", "id,total,user.age"],
          stdin='{"id":1,"user":{"name":"Ann"},"total":12.50}\n{"id":2,"user":{"name":"Bob"},"total":3}\n[1]\n')
c.case("example", r.code == 1 and r.out == '{"id":1,"total":12.50,"user.age":null}\n'
       and errs(r.err) == [("3", "not an object")], r)
expect("whole record compact, key order kept",
       c.run([], stdin='{ "zeta" : 1 , "alpha" : [ 1 , { "y" : null , "x" : true } ] , "m" : { } , "e" : [ ] }\n'),
       '{"zeta":1,"alpha":[1,{"y":null,"x":true}],"m":{},"e":[]}\n')
expect("numbers keep exact text",
       c.run([], stdin='{"a":12345678901234567890,"b":1.10,"c":1e400,"d":-0,"e":1E+2,"f":0.000001,"g":-12.50e-3}\n'),
       '{"a":12345678901234567890,"b":1.10,"c":1e400,"d":-0,"e":1E+2,"f":0.000001,"g":-12.50e-3}\n')
expect("string escapes and unicode",
       c.run([], stdin='{"s":"\\u00e9\\t\\"q\\" \\\\ \\/ \\u0001\\u001f \\ud83d\\ude00 日本 \\b\\f\\n\\r","k\\u00fc":"ü"}\n'),
       '{"s":"é\\t\\"q\\" \\\\ / \\u0001\\u001f 😀 日本 \\b\\f\\n\\r","kü":"ü"}\n')
recs = ('{"id":1,"n":1.10,"ok":true,"name":"Zoë","u":{"c":"x"}}\n'
        '{"id":2,"n":1.1,"ok":"true","name":"Zoe","u":{"c":"y"},"z":null}\n'
        '{"id":3,"n":"1.10","ok":false,"name":"a=b","u":"c"}\n')
def ids(args):
    r = c.run(args + ["--select", "id"], stdin=recs)
    return r, [int(x) for x in re.findall(r'\{"id":(\d+)\}', r.out)] if r.code == 0 else None
for name, args, want in [
    ("where number by exact text", ["--where", "n=1.10"], [1, 3]),
    ("where true matches boolean and string", ["--where", "ok=true"], [1, 2]),
    ("where unicode string, nested path", ["--where", "name=Zoë", "--where", "u.c=x"], [1]),
    ("where filters are AND-ed", ["--where", "ok=true", "--where", "u.c=y"], [2]),
    ("where value may contain =", ["--where", "name=a=b"], [3]),
    ("where null matches null, not missing", ["--where", "z=null"], [2]),
    ("where through non-object finds nothing", ["--where", "u.c.d=x"], []),
]:
    r, got = ids(args)
    c.case(name, got == want, r)
expect("where empty value; objects and arrays never match",
       c.run(["--where", "e="], stdin='{"e":""}\n{"e":"x"}\n{"e":{}}\n{"e":[]}\n{"f":""}\n'), '{"e":""}\n')
expect("select: order, missing, nested, non-object path, arrays not indexed",
       c.run(["--select", "b.c,a,x.y,a.k,items.0,b"],
             stdin='{"a":5,"b":{"c":[1,{"d":"e"}],"q":1},"items":[7,8]}\n'),
       '{"b.c":[1,{"d":"e"}],"a":5,"x.y":null,"a.k":null,"items.0":null,"b":{"c":[1,{"d":"e"}],"q":1}}\n')
r = c.run([], stdin='{"a":1}\n\n   \n{"a":2,}\n{a:1}\n{"a":01}\n{"a":NaN}\n{"a":1} x\n"str"\n42\n{"a":"x\n{"a":3}\r\n')
c.case("bad lines reported with line numbers, others printed",
       r.code == 1 and r.out == '{"a":1}\n{"a":3}\n' and errs(r.err) == [
           ("4", "invalid JSON"), ("5", "invalid JSON"), ("6", "invalid JSON"), ("7", "invalid JSON"),
           ("8", "invalid JSON"), ("9", "not an object"), ("10", "not an object"), ("11", "invalid JSON")], r)
expect("empty input", c.run(["--where", "a=1"], stdin=""), "")
for args in (["--where"], ["--where", "abc"], ["--where", "=x"], ["--select", "a,,b"], ["--select", "a..b"],
             ["--bogus"]):
    r = c.run(args, stdin='{"a":1}\n')
    c.case("usage error " + " ".join(args), r.code == 64 and r.err.startswith("usage:") and r.out == "", r)

lines = "".join(f'{{"id":{i},"grp":"g{i % 10}","v":{{"x":{i}.50}}}}\n' for i in range(100000))
want = "".join(f'{{"id":{i},"v.x":{i}.50}}\n' for i in range(100000) if i % 10 == 3)
expect("large input", c.run(["--where", "grp=g3", "--select", "id,v.x"], stdin=lines, timeout=20), want)
c.finish()
