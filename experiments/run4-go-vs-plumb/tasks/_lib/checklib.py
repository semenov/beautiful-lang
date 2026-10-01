"""Helpers for hidden checks. A check is `python3 check.py <binary> <workdir>`;
it prints one JSON line {"cases": [{"name", "ok", "detail"}]} as its last line.

    from checklib import Checks
    c = Checks()                      # reads argv
    r = c.run(["--top", "3"], stdin="a b a")
    c.case("counts words", r.code == 0 and r.out == "a: 2\nb: 1\n", r)
    c.finish()
"""
import json, os, socket, subprocess, sys, time, traceback
import http.client
from pathlib import Path


class Result:
    def __init__(self, code, out, err, timed_out=False):
        self.code, self.out, self.err, self.timed_out = code, out, err, timed_out

    def __repr__(self):
        return f"exit={self.code} timed_out={self.timed_out} stdout={self.out[:400]!r} stderr={self.err[:300]!r}"


class Resp:
    def __init__(self, status, body, headers):
        self.status, self.body, self.headers = status, body, headers

    def json(self):
        try:
            return json.loads(self.body)
        except ValueError:
            return None

    def __repr__(self):
        return f"HTTP {self.status} {self.body[:400]!r}"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    """A solution running as a server on PORT, with its cwd in the workdir."""

    def __init__(self, binary, cwd, env=None, args=()):
        self.port = free_port()
        e = dict(os.environ, PORT=str(self.port))
        e.update(env or {})
        self.log = open(Path(cwd) / f"server-{self.port}.log", "w")
        self.proc = subprocess.Popen([binary, *args], cwd=cwd, env=e,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited with {self.proc.returncode}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start listening within 10 s")

    def req(self, method, path, body=None, headers=None, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        h = dict(headers or {})
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
            h.setdefault("Content-Type", "application/json")
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        data = r.read().decode("utf-8", "replace")
        conn.close()
        return Resp(r.status, data, dict((k.lower(), v) for k, v in r.getheaders()))

    def get(self, path, **kw):
        return self.req("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.req("POST", path, body, **kw)

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()


class Checks:
    def __init__(self):
        self.binary = os.path.abspath(sys.argv[1])
        self.work = Path(sys.argv[2]).resolve()
        self.work.mkdir(parents=True, exist_ok=True)
        self.cases = []
        self.servers = []

    def run(self, args, stdin="", env=None, cwd=None, timeout=20):
        e = dict(os.environ)
        e.update(env or {})
        try:
            r = subprocess.run([self.binary, *args], input=stdin.encode() if isinstance(stdin, str) else stdin,
                               capture_output=True, cwd=cwd or self.work, env=e, timeout=timeout)
            return Result(r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace"))
        except subprocess.TimeoutExpired as t:
            return Result(None, (t.stdout or b"").decode("utf-8", "replace"), (t.stderr or b"").decode("utf-8", "replace"), True)

    def server(self, env=None, args=()):
        s = Server(self.binary, self.work, env, args)
        self.servers.append(s)
        return s

    def case(self, name, ok, detail=""):
        self.cases.append({"name": name, "ok": bool(ok), "detail": "" if ok else str(detail)[:1500]})

    def test(self, name):
        """Decorator: run fn(); an exception fails the case. fn returns (ok, detail) or ok."""
        def deco(fn):
            try:
                r = fn()
                ok, detail = r if isinstance(r, tuple) else (r, "")
            except Exception:
                ok, detail = False, traceback.format_exc(limit=3)
            self.case(name, ok, detail)
            return fn
        return deco

    def finish(self):
        for s in self.servers:
            s.stop()
        print(json.dumps({"cases": self.cases}))
