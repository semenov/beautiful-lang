import sys, os, json, hashlib, random, socket, threading
import http.client
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()
store = c.work / "store"
if store.exists():
    for p in store.iterdir():
        p.unlink()
for p in ("x", "escape", "etc"):
    if (c.work / p).exists():
        (c.work / p).unlink()
MAX = 100000
store.mkdir(exist_ok=True)
(store / "preexisting.txt").write_bytes(b"placed by hand\n")
(store / ".hidden").write_bytes(b"secret")
env = {"STORE_DIR": str(store), "MAX_BYTES": str(MAX)}
s = c.server(env)
rnd = random.Random(18)
stored = {"preexisting.txt": b"placed by hand\n"}


class R:
    def __init__(self, status, body, headers):
        self.status, self.body, self.headers = status, body, headers

    def json(self):
        try:
            return json.loads(self.body)
        except ValueError:
            return None

    def __repr__(self):
        return f"HTTP {self.status} {self.body[:200]!r}"


def req(method, path, body=None):
    conn = http.client.HTTPConnection("127.0.0.1", s.port, timeout=20)
    conn.request(method, path, body=body)
    r = conn.getresponse()
    data = r.read()
    conn.close()
    return R(r.status, data, {k.lower(): v for k, v in r.getheaders()})


def raw_put(path, body):
    """PUT that tolerates the server answering before reading the whole body."""
    sock = socket.create_connection(("127.0.0.1", s.port), timeout=20)
    head = f"PUT {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    try:
        sock.sendall(head.encode() + body)
    except OSError:
        pass
    data = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    sock.close()
    head, _, rest = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ")[1])
    except (IndexError, ValueError):
        status = None
    return R(status, rest, {})


def meta(name, data):
    return {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def disk_names():
    return sorted(p.name for p in store.iterdir() if not p.name.startswith("."))


@c.test("PUT new file: 201, metadata, stored on disk as given")
def _():
    r = req("PUT", "/files/greeting.txt", b"hello")
    g = req("GET", "/files/greeting.txt")
    stored["greeting.txt"] = b"hello"
    return (r.status == 201 and r.json() == meta("greeting.txt", b"hello") and g.status == 200 and g.body == b"hello"
            and g.headers.get("content-type", "").startswith("application/octet-stream")
            and (store / "greeting.txt").read_bytes() == b"hello"), (r, g)


@c.test("PUT existing file replaces it with 200")
def _():
    r = req("PUT", "/files/greeting.txt", b"hello again")
    g = req("GET", "/files/greeting.txt")
    stored["greeting.txt"] = b"hello again"
    return r.status == 200 and r.json() == meta("greeting.txt", b"hello again") and g.body == b"hello again", (r, g)


@c.test("binary content round-trips exactly")
def _():
    data = bytes(range(256)) * 4 + b"\x00\r\n\r\n\xff" + rnd.randbytes(90000)
    r = req("PUT", "/files/blob.bin", data)
    g = req("GET", "/files/blob.bin")
    stored["blob.bin"] = data
    return r.status == 201 and r.json() == meta("blob.bin", data) and g.body == data, (r, len(g.body))


@c.test("empty file and a file of exactly MAX_BYTES are accepted")
def _():
    big = b"m" * MAX
    r1, r2 = req("PUT", "/files/empty", b""), req("PUT", "/files/max.bin", big)
    g1, g2 = req("GET", "/files/empty"), req("GET", "/files/max.bin")
    stored["empty"], stored["max.bin"] = b"", big
    return (r1.status == 201 and r1.json() == meta("empty", b"") and g1.status == 200 and g1.body == b""
            and r2.status == 201 and g2.body == big), (r1, r2, g1)


@c.test("over MAX_BYTES: 413 and nothing stored or changed")
def _():
    before = disk_names()
    r1 = raw_put("/files/toolarge.bin", b"z" * (MAX + 1))
    r2 = raw_put("/files/greeting.txt", b"q" * (MAX + 1))
    g = req("GET", "/files/toolarge.bin")
    g2 = req("GET", "/files/greeting.txt")
    ok = (r1.status == 413 and json.loads(r1.body or b"null") == {"error": "too large"} and r2.status == 413
          and g.status == 404 and g2.body == b"hello again" and disk_names() == before)
    return ok, (r1, r2, g, disk_names())


@c.test("invalid names are 400 for every method and touch nothing")
def _():
    before = disk_names()
    names = ["..", ".", ".env", "a/b", "a%20b", "%C3%BC.txt", "", "x" * 101, "a%00b", "name*", "a\\b".replace("\\", "%5C")]
    bad = []
    for n in names:
        for m, b in (("PUT", b"data"), ("GET", None), ("DELETE", None)):
            r = req(m, "/files/" + n, b)
            if not (r.status == 400 and r.json() == {"error": "invalid name"}):
                bad.append((m, n, r))
    ok100 = req("PUT", "/files/" + "x" * 100, b"long name")
    stored["x" * 100] = b"long name"
    return not bad and ok100.status == 201 and disk_names() == sorted(before + ["x" * 100]), bad[:5]


@c.test("encoded path traversal is refused")
def _():
    paths = ["/files/..%2Fx", "/files/..%2F..%2Fescape", "/files/%2E%2E", "/files/%2E%2E%2Fetc", "/files/%2Fetc"]
    rs = [req("PUT", p, b"evil") for p in paths] + [req("GET", "/files/..%2Fcheck.py")]
    outside = [p for p in ("x", "escape", "etc") if (c.work / p).exists()]
    return all(r.status == 400 and r.json() == {"error": "invalid name"} for r in rs) and not outside, (rs, outside)


@c.test("GET and DELETE of unknown names are 404; delete works")
def _():
    req("PUT", "/files/temp.txt", b"temp")
    d = req("DELETE", "/files/temp.txt")
    g = req("GET", "/files/temp.txt")
    d2 = req("DELETE", "/files/temp.txt")
    g2 = req("GET", "/files/never-was")
    return (d.status == 204 and d.body == b"" and g.status == 404 and g.json() == {"error": "not found"}
            and d2.status == 404 and g2.status == 404 and not (store / "temp.txt").exists()), (d, g, d2, g2)


@c.test("listing: all files, sizes, sha256, ordered by name; pre-existing included, dot files not")
def _():
    for n in ("B-upper", "_under", "0zero", "a.lower", "-dash"):
        req("PUT", "/files/" + n, n.encode() * 3)
        stored[n] = n.encode() * 3
    r = req("GET", "/files")
    want = [meta(n, stored[n]) for n in sorted(stored, key=lambda x: x.encode())]
    return r.status == 200 and r.json() == {"files": want}, (r, want)


@c.test("pre-existing file is served; dot file is not")
def _():
    g = req("GET", "/files/preexisting.txt")
    h = req("GET", "/files/.hidden")
    return g.status == 200 and g.body == b"placed by hand\n" and h.status == 400, (g, h)


@c.test("parallel uploads of different files all succeed")
def _():
    blobs = {f"par-{i:02d}.bin": random.Random(i).randbytes(60000) for i in range(30)}
    with ThreadPoolExecutor(15) as ex:
        rs = list(ex.map(lambda kv: req("PUT", "/files/" + kv[0], kv[1]), blobs.items()))
    stored.update(blobs)
    with ThreadPoolExecutor(15) as ex:
        gs = list(ex.map(lambda n: req("GET", "/files/" + n), blobs))
    return (all(r.status == 201 for r in rs) and all(g.body == blobs[n] for g, n in zip(gs, blobs))), [r for r in rs if r.status != 201][:3]


@c.test("concurrent PUTs to one name: readers see whole contents, one body wins")
def _():
    bodies = [bytes([65 + i]) * 90000 for i in range(8)]
    seen, lock = [], threading.Lock()
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            g = req("GET", "/files/contested.bin")
            with lock:
                seen.append((g.status, g.body))

    readers = [threading.Thread(target=reader) for _ in range(4)]
    for t in readers:
        t.start()
    with ThreadPoolExecutor(8) as ex:
        rs = list(ex.map(lambda b: req("PUT", "/files/contested.bin", b), bodies * 3))
    stop.set()
    for t in readers:
        t.join()
    final = req("GET", "/files/contested.bin").body
    stored["contested.bin"] = final
    bad_reads = [(st, len(b), b[:1], b[-1:]) for st, b in seen if not (st == 404 or (st == 200 and b in bodies))]
    return (all(r.status in (200, 201) for r in rs) and sum(r.status == 201 for r in rs) >= 1
            and final in bodies and not bad_reads), (bad_reads[:5], [r.status for r in rs])


@c.test("404 for other paths, 405 for other methods")
def _():
    r404 = [req("GET", "/"), req("GET", "/file/a"), req("GET", "/filesx")]
    r405 = [req("POST", "/files/a", b"x"), req("DELETE", "/files"), req("PUT", "/files", b"x")]
    return (all(r.status == 404 and r.json() == {"error": "not found"} for r in r404)
            and all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in r405)), (r404, r405)


@c.test("files survive a restart")
def _():
    global s
    before = req("GET", "/files").json()
    s.stop()
    s = c.server(env)
    after = req("GET", "/files").json()
    g = req("GET", "/files/blob.bin")
    return after == before and g.body == stored["blob.bin"], (len(before["files"]), len(after["files"]))


c.finish()
