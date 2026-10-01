#!/usr/bin/env python3
import hashlib, json, os, re, secrets, socketserver
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, unquote

STORE = os.environ["STORE_DIR"]
MAX = int(os.environ["MAX_BYTES"])
os.makedirs(STORE, exist_ok=True)
NAME_RE = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9._-]{0,99}")


def meta(name, data):
    return {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None, raw=None):
        if raw is not None:
            data, ctype = raw, "application/octet-stream"
        elif obj is not None:
            data, ctype = json.dumps(obj).encode(), "application/json"
        else:
            data, ctype = b"", None
        self.send_response(code)
        if ctype:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def route(self, method):
        path = urlsplit(self.path).path
        length = int(self.headers.get("Content-Length") or 0)
        if path == "/files":
            if method != "GET":
                return self.send(405, {"error": "method not allowed"})
            out = []
            for n in sorted(os.listdir(STORE), key=lambda x: x.encode()):
                p = os.path.join(STORE, n)
                if NAME_RE.fullmatch(n) and os.path.isfile(p):
                    with open(p, "rb") as f:
                        out.append(meta(n, f.read()))
            return self.send(200, {"files": out})
        if not path.startswith("/files/"):
            return self.send(404, {"error": "not found"})
        if method not in ("GET", "PUT", "DELETE"):
            return self.send(405, {"error": "method not allowed"})
        name = unquote(path[len("/files/"):])
        if not NAME_RE.fullmatch(name):
            return self.send(400, {"error": "invalid name"})
        p = os.path.join(STORE, name)
        if method == "GET":
            try:
                with open(p, "rb") as f:
                    return self.send(200, raw=f.read())
            except (FileNotFoundError, IsADirectoryError):
                return self.send(404, {"error": "not found"})
        if method == "DELETE":
            try:
                os.remove(p)
            except FileNotFoundError:
                return self.send(404, {"error": "not found"})
            return self.send(204)
        if length > MAX:
            self.close_connection = True
            return self.send(413, {"error": "too large"})
        data = self.rfile.read(length)
        tmp = os.path.join(STORE, ".tmp-" + secrets.token_hex(8))
        with open(tmp, "wb") as f:
            f.write(data)
        existed = os.path.exists(p)
        os.replace(tmp, p)
        return self.send(200 if existed else 201, meta(name, data))

    def do_GET(self): self.route("GET")
    def do_POST(self): self.route("POST")
    def do_PUT(self): self.route("PUT")
    def do_DELETE(self): self.route("DELETE")


class Server(ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup HTTPServer does
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
