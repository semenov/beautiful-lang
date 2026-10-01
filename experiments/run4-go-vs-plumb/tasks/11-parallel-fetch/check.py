import sys, os, time, threading
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks, free_port
from http.server import BaseHTTPRequestHandler
from socketserver import ThreadingMixIn, TCPServer

c = Checks()

lock = threading.Lock()
state = {"inflight": 0, "max": 0, "hits": {}, "gen": 0}

def reset():
    with lock:
        state.update(inflight=0, max=0, hits={}, gen=state["gen"] + 1)

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def send(self, code, body, headers=(), length=True):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if length:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path.split("?")[0].split("/")[1:]
        with lock:
            state["hits"][self.path] = state["hits"].get(self.path, 0) + 1
        try:
            self.route(p)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def tracked(self, seconds):
        with lock:
            gen = state["gen"]
            state["inflight"] += 1
            state["max"] = max(state["max"], state["inflight"])
        try:
            time.sleep(seconds)
        finally:
            with lock:
                if state["gen"] == gen:    # requests left over from an earlier case don't count
                    state["inflight"] -= 1

    def route(self, p):
        if p[0] == "ok":                       # /ok/<n>: n bytes
            self.send(200, b"x" * int(p[1]))
        elif p[0] == "status":                 # /status/<code>
            self.send(int(p[1]), b"status %s" % p[1].encode())
        elif p[0] == "slow":                   # /slow/<ms>
            self.tracked(int(p[1]) / 1000)
            self.send(200, b"slow")
        elif p[0] == "delay":                  # /delay/<ms>/<n>
            self.tracked(int(p[1]) / 1000)
            self.send(200, b"d" * int(p[2]))
        elif p[0] == "trickle":                # headers now, then 1 byte every 0.2 s for 4 s
            self.send_response(200)
            self.send_header("Content-Length", "20")
            self.end_headers()
            for _ in range(20):
                self.wfile.write(b"t")
                self.wfile.flush()
                time.sleep(0.2)
        elif p[0] == "redirect":               # /redirect/<k>: k relative redirects, then 200
            k = int(p[1])
            if k == 0:
                self.send(200, b"landed")
            else:
                self.send(302 if k % 2 else 307, b"", [("Location", f"/redirect/{k - 1}")])
        elif p[0] == "absredirect":
            self.send(301, b"moved", [("Location", f"http://127.0.0.1:{PORT}/ok/7")])
        elif p[0] == "loop":
            self.send(302, b"", [("Location", "/loop")])
        elif p[0] == "big":                    # 8 MiB
            self.send(200, b"B" * (8 << 20))
        elif p[0] == "nolength":               # body ends when the connection closes
            self.send(200, b"n" * 1000, length=False)
        elif p[0] == "drop":                   # close without any response
            self.connection.shutdown(2)
        else:
            self.send(404, b"not found")

PORT = free_port()
class Srv(ThreadingMixIn, TCPServer):   # like ThreadingHTTPServer, without its slow getfqdn()
    daemon_threads = True
    request_queue_size = 256

srv = Srv(("127.0.0.1", PORT), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
B = f"http://127.0.0.1:{PORT}"
DEAD = f"http://127.0.0.1:{free_port()}/nothing-listens"

def expect(name, r, out, code):
    c.case(name, r.code == code and r.out == out, r)

r = c.run(["--concurrency", "2"], stdin=f"{B}/ok/1234\n\n# skip\nftp://x\n{B}/missing\n")
expect("example", r, f"{B}/ok/1234 200 1234\nftp://x error invalid-url\n{B}/missing 404 9\n"
       "total 3 ok 1 bad-status 1 failed 1\n", 1)

r = c.run([], stdin=f"  {B}/ok/0  \n{B}/ok/5\n")
expect("all 2xx exit 0, whitespace trimmed", r, f"{B}/ok/0 200 0\n{B}/ok/5 200 5\ntotal 2 ok 2 bad-status 0 failed 0\n", 0)

expect("no URLs", c.run([], stdin="\n# nothing\n\n"), "total 0 ok 0 bad-status 0 failed 0\n", 0)

r = c.run([], stdin=f"{B}/status/500\n{B}/status/201\n{B}/status/403\n")
expect("any status is a response", r, f"{B}/status/500 500 10\n{B}/status/201 201 10\n{B}/status/403 403 10\n"
       "total 3 ok 1 bad-status 2 failed 0\n", 1)

r = c.run([], stdin=f"{B}/redirect/5\n{B}/absredirect\n{B}/redirect/10\n{B}/redirect/11\n{B}/loop\n")
expect("redirects followed, at most 10", r,
       f"{B}/redirect/5 200 6\n{B}/absredirect 200 7\n{B}/redirect/10 200 6\n{B}/redirect/11 error redirects\n"
       f"{B}/loop error redirects\ntotal 5 ok 3 bad-status 0 failed 2\n", 1)

r = c.run([], stdin=f"{DEAD}\nnot a url\nhttp://\nftp://example.com/f\nhttps://\n{B}/ok/3\n")
expect("connect and invalid-url errors", r,
       f"{DEAD} error connect\nnot a url error invalid-url\nhttp:// error invalid-url\n"
       f"ftp://example.com/f error invalid-url\nhttps:// error invalid-url\n{B}/ok/3 200 3\n"
       "total 6 ok 1 bad-status 0 failed 5\n", 1)

r = c.run([], stdin=f"{B}/drop\n{B}/ok/2\n")
expect("connection closed without response", r, f"{B}/drop error other\n{B}/ok/2 200 2\ntotal 2 ok 1 bad-status 0 failed 1\n", 1)

r = c.run([], stdin=f"{B}/big\n{B}/nolength\n", timeout=30)
expect("large and unsized bodies", r, f"{B}/big 200 {8 << 20}\n{B}/nolength 200 1000\n"
       "total 2 ok 2 bad-status 0 failed 0\n", 0)

t = time.time()
r = c.run(["--timeout", "0.5"], stdin=f"{B}/slow/4000\n{B}/ok/4\n")
el = time.time() - t
c.case("timeout on a slow response", r.code == 1 and el < 3 and r.out ==
       f"{B}/slow/4000 error timeout\n{B}/ok/4 200 4\ntotal 2 ok 1 bad-status 0 failed 1\n", (el, r))
t = time.time()
r = c.run(["--timeout", "1"], stdin=f"{B}/trickle\n")
el = time.time() - t
c.case("timeout covers a trickling body", r.code == 1 and el < 3.3 and r.out ==
       f"{B}/trickle error timeout\ntotal 1 ok 0 bad-status 0 failed 1\n", (el, r))
r = c.run(["--timeout", "2.5"], stdin=f"{B}/slow/300\n")
expect("fetch within the timeout", r, f"{B}/slow/300 200 4\ntotal 1 ok 1 bad-status 0 failed 0\n", 0)

def limit_case(name, args, count, ms, want):
    reset()
    urls = "".join(f"{B}/slow/{ms}?i={i}\n" for i in range(count))
    t = time.time()
    r = c.run(args, stdin=urls, timeout=30)
    el = time.time() - t
    out = "".join(f"{B}/slow/{ms}?i={i} 200 4\n" for i in range(count)) + f"total {count} ok {count} bad-status 0 failed 0\n"
    c.case(name, r.code == 0 and r.out == out and state["max"] == want, (f"max in flight {state['max']}, want {want}", el, r))

limit_case("concurrency 3 is reached and not exceeded", ["--concurrency", "3"], 12, 300, 3)
limit_case("default concurrency 4", [], 10, 300, 4)
limit_case("concurrency 1 is sequential", ["--concurrency", "1"], 4, 150, 1)
limit_case("concurrency above the number of URLs", ["--concurrency", "50"], 6, 300, 6)

reset()
urls = [f"{B}/delay/{(i * 37) % 60}/{i}" for i in range(200)]
(c.work / "urls.txt").write_text("".join(u + "\n" for u in urls))
r = c.run(["urls.txt", "--concurrency", "16"], timeout=30)
expect("200 URLs from a file, in input order", r,
       "".join(f"{u} 200 {i}\n" for i, u in enumerate(urls)) + "total 200 ok 200 bad-status 0 failed 0\n", 0)
c.case("200 URLs: limit 16 respected", state["max"] <= 16, state["max"])

reset()
r = c.run([], stdin=f"{B}/ok/9\n{B}/ok/9\n{B}/ok/9\n")
expect("duplicate lines fetched separately", r, f"{B}/ok/9 200 9\n" * 3 + "total 3 ok 3 bad-status 0 failed 0\n", 0)
c.case("each line fetched exactly once", state["hits"].get("/ok/9") == 3, state["hits"])

r = c.run(["missing.txt"])
c.case("unreadable file", r.code == 2 and r.out == "" and "error: cannot read missing.txt" in r.err, r)

bad = []
for args in (["--concurrency", "0"], ["--concurrency", "x"], ["--concurrency"], ["--timeout", "0"],
             ["--timeout", "-1"], ["--timeout", "soon"], ["--bogus"], ["a.txt", "b.txt"]):
    r = c.run(args, stdin="")
    if not (r.code == 64 and r.err.startswith("usage:") and r.out == ""):
        bad.append((args, r))
c.case("usage errors", not bad, bad)
srv.shutdown()
c.finish()
