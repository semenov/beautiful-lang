#!/usr/bin/env python3
import json, os, re, socketserver, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, parse_qs

LO, HI = -(1 << 63), (1 << 63) - 1
lock = threading.Lock()
data = {}
MISSING = object()


def valid(v):
    if isinstance(v, str):
        return True
    return isinstance(v, int) and not isinstance(v, bool) and LO <= v <= HI


def same(a, b):
    return type(a) is type(b) and a == b


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        if obj is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            obj = json.loads(raw)
        except ValueError:
            return None
        return obj if isinstance(obj, dict) else None

    def handle_any(self, method):
        path, _, query = self.path.partition("?")
        routes = {"/kv/": ("GET", "PUT", "DELETE"), "/incr/": ("POST",), "/cas/": ("POST",)}
        if path == "/snapshot":
            if method != "GET":
                return self.send(405, {"error": "method not allowed"})
            with lock:
                items = dict(data)
            return self.send(200, {"count": len(items), "items": items})
        for prefix, methods in routes.items():
            if path.startswith(prefix):
                break
        else:
            return self.send(404, {"error": "not found"})
        if method not in methods:
            return self.send(405, {"error": "method not allowed"})
        key = unquote(path[len(prefix):])
        if key == "":
            return self.send(400, {"error": "empty key"})
        if prefix == "/kv/":
            if method == "GET":
                with lock:
                    v = data.get(key, MISSING)
                if v is MISSING:
                    return self.send(404, {"error": "not found"})
                return self.send(200, {"key": key, "value": v})
            if method == "DELETE":
                with lock:
                    v = data.pop(key, MISSING)
                return self.send(404, {"error": "not found"}) if v is MISSING else self.send(204)
            obj = self.body()
            if obj is None:
                return self.send(400, {"error": "invalid JSON"})
            if "value" not in obj or not valid(obj["value"]):
                return self.send(400, {"error": "invalid value"})
            with lock:
                data[key] = obj["value"]
            return self.send(200, {"key": key, "value": obj["value"]})
        if prefix == "/incr/":
            q = parse_qs(query, keep_blank_values=True)
            by = q.get("by", ["1"])[0]
            if not re.fullmatch(r"-?[0-9]+", by) or not LO <= int(by) <= HI:
                return self.send(400, {"error": "invalid by"})
            by = int(by)
            with lock:
                cur = data.get(key, 0)
                if not (isinstance(cur, int) and not isinstance(cur, bool)):
                    return self.send(409, {"error": "value is not an integer", "value": cur})
                if not LO <= cur + by <= HI:
                    return self.send(409, {"error": "integer overflow", "value": cur})
                data[key] = cur + by
                return self.send(200, {"key": key, "value": cur + by})
        obj = self.body()
        if obj is None:
            return self.send(400, {"error": "invalid JSON"})
        if "value" not in obj or not valid(obj["value"]):
            return self.send(400, {"error": "invalid value"})
        if "expected" not in obj or not (obj["expected"] is None or valid(obj["expected"])):
            return self.send(400, {"error": "invalid expected"})
        exp = obj["expected"]
        with lock:
            cur = data.get(key, MISSING)
            if (exp is None and cur is not MISSING) or (exp is not None and (cur is MISSING or not same(cur, exp))):
                return self.send(409, {"error": "value does not match", "value": None if cur is MISSING else cur})
            data[key] = obj["value"]
        return self.send(200, {"key": key, "value": obj["value"]})

    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PUT(self): self.handle_any("PUT")
    def do_DELETE(self): self.handle_any("DELETE")
    def do_PATCH(self): self.handle_any("PATCH")


class Server(ThreadingHTTPServer):
    request_queue_size = 1024
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup of HTTPServer
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
