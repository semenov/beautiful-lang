#!/usr/bin/env python3
import sys, re, time, socket, http.client, threading
from urllib.parse import urlsplit, urljoin
from concurrent.futures import ThreadPoolExecutor

def usage():
    sys.stderr.write("usage: app [--concurrency N] [--timeout SECONDS] [FILE]\n"); sys.exit(64)

args = sys.argv[1:]; conc, timeout, files = 4, 10.0, []
i = 0
while i < len(args):
    a = args[i]
    if a in ("--concurrency", "--timeout"):
        if i + 1 >= len(args): usage()
        v = args[i + 1]
        if a == "--concurrency":
            if not re.fullmatch(r"[0-9]+", v) or int(v) < 1: usage()
            conc = int(v)
        else:
            if not re.fullmatch(r"[0-9]+(\.[0-9]+)?|\.[0-9]+", v) or float(v) <= 0: usage()
            timeout = float(v)
        i += 2
    elif a.startswith("--"): usage()
    else: files.append(a); i += 1
if len(files) > 1: usage()
if files:
    try: data = open(files[0], "rb").read()
    except OSError:
        sys.stderr.write(f"error: cannot read {files[0]}\n"); sys.exit(2)
else:
    data = sys.stdin.buffer.read()
urls = [l.strip() for l in data.decode("utf-8", "replace").split("\n")]
urls = [u for u in urls if u and not u.startswith("#")]

class Fail(Exception):
    pass

def valid(u):
    try: s = urlsplit(u)
    except ValueError: return False
    return s.scheme in ("http", "https") and bool(s.hostname) and u.lower().startswith(s.scheme + "://")

def fetch_one(url, deadline):
    """Returns (status, nbytes, location). A watchdog shuts the socket down at the deadline."""
    s = urlsplit(url)
    left = deadline - time.monotonic()
    if left <= 0: raise Fail("timeout")
    cls = http.client.HTTPSConnection if s.scheme == "https" else http.client.HTTPConnection
    conn = cls(s.hostname, s.port, timeout=left)
    try:
        conn.connect()
    except (socket.timeout, TimeoutError):
        conn.close(); raise Fail("timeout")
    except OSError:
        conn.close(); raise Fail("connect")
    guard = conn.sock.dup()
    fired = threading.Event()
    def kill():
        fired.set()
        try: guard.shutdown(socket.SHUT_RDWR)
        except OSError: pass
    timer = threading.Timer(max(deadline - time.monotonic(), 0), kill)
    timer.start()
    path = s.path or "/"
    if s.query: path += "?" + s.query
    try:
        conn.request("GET", path, headers={"Connection": "close"})
        resp = conn.getresponse()
        n = 0
        while True:
            chunk = resp.read1(65536)
            if not chunk: break
            n += len(chunk)
        if fired.is_set(): raise Fail("timeout")
        return resp.status, n, resp.getheader("Location")
    except (socket.timeout, TimeoutError):
        raise Fail("timeout")
    except (OSError, http.client.HTTPException):
        raise Fail("timeout" if fired.is_set() else "other")
    finally:
        timer.cancel(); conn.close(); guard.close()

def fetch(url):
    if not valid(url): return "error invalid-url", "failed"
    deadline = time.monotonic() + timeout
    cur = url
    try:
        for redirects in range(11):
            status, n, loc = fetch_one(cur, deadline)
            if status in (301, 302, 303, 307, 308) and loc:
                if redirects == 10: raise Fail("redirects")
                cur = urljoin(cur, loc)
                if not valid(cur): raise Fail("other")
                continue
            return f"{status} {n}", "ok" if 200 <= status < 300 else "bad"
    except Fail as e:
        return f"error {e}", "failed"

with ThreadPoolExecutor(max_workers=conc) as ex:
    results = list(ex.map(fetch, urls))
counts = {"ok": 0, "bad": 0, "failed": 0}
out = []
for u, (text, kind) in zip(urls, results):
    counts[kind] += 1
    out.append(f"{u} {text}\n")
out.append(f"total {len(urls)} ok {counts['ok']} bad-status {counts['bad']} failed {counts['failed']}\n")
sys.stdout.write("".join(out))
sys.exit(0 if counts["bad"] == 0 and counts["failed"] == 0 else 1)
