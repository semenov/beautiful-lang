#!/usr/bin/env python3
import base64, hashlib, hmac, json, os, re, socketserver, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs, unquote


def fail(msg):
    sys.stderr.write(f"error: {msg}\n")
    sys.exit(1)


try:
    raw = open(os.environ.get("CATALOG_PATH", ""), "rb").read()
except OSError:
    fail("cannot read catalog")
try:
    catalog = json.loads(raw.decode("utf-8"))
except (ValueError, UnicodeDecodeError):
    fail("catalog is not valid JSON")
if not isinstance(catalog, list):
    fail("catalog must be an array")
by_id = {}
for p in catalog:
    ok = (isinstance(p, dict) and isinstance(p.get("id"), str) and re.fullmatch(r"[A-Za-z0-9_-]+", p["id"])
          and isinstance(p.get("name"), str) and isinstance(p.get("category"), str)
          and type(p.get("price_cents")) is int and p["price_cents"] >= 0 and type(p.get("in_stock")) is bool)
    if not ok:
        fail(f"invalid product {json.dumps(p)[:100]}")
    if p["id"] in by_id:
        fail(f"duplicate id {p['id']}")
    p = {k: p[k] for k in ("id", "name", "category", "price_cents", "in_stock")}
    by_id[p["id"]] = p

KEY = hashlib.sha256(raw).digest()
SORTS = {"id": lambda p: [p["id"]], "price_asc": lambda p: [p["price_cents"], p["id"]],
         "price_desc": lambda p: [-p["price_cents"], p["id"]], "name": lambda p: [p["name"], p["id"]]}
ordered = {name: sorted(by_id.values(), key=k) for name, k in SORTS.items()}


def b64(b):
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


def make_cursor(ctx, key):
    body = b64(json.dumps({"c": ctx, "k": key}).encode())
    sig = b64(hmac.new(KEY, body.encode(), hashlib.sha256).digest()[:16])
    return body + "." + sig


def read_cursor(cur, ctx):
    try:
        body, sig = cur.split(".")
        want = b64(hmac.new(KEY, body.encode(), hashlib.sha256).digest()[:16])
        if not hmac.compare_digest(sig, want):
            return None
        d = json.loads(base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)))
        return d["k"] if d["c"] == ctx else None
    except (ValueError, KeyError, TypeError):
        return None


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def bad(self, name):
        self.send(400, {"error": f"invalid parameter: {name}"})

    def route(self, method):
        u = urlsplit(self.path)
        m = re.fullmatch(r"/products(?:/([^/]+))?", u.path)
        if not m:
            return self.send(404, {"error": "not found"})
        if method != "GET":
            return self.send(405, {"error": "method not allowed"})
        if m.group(1) is not None:
            p = by_id.get(unquote(m.group(1)))
            return self.send(200, p) if p else self.send(404, {"error": "not found"})
        q = {k: v[0] for k, v in parse_qs(u.query, keep_blank_values=True).items()}
        nums = {}
        for name in ("min_price", "max_price"):
            if name in q:
                if not re.fullmatch(r"[0-9]+", q[name]):
                    return self.bad(name)
                nums[name] = int(q[name])
        if "min_price" in nums and "max_price" in nums and nums["min_price"] > nums["max_price"]:
            return self.bad("max_price")
        in_stock = q.get("in_stock")
        if in_stock is not None and in_stock not in ("true", "false"):
            return self.bad("in_stock")
        sort = q.get("sort", "id")
        if sort not in SORTS:
            return self.bad("sort")
        limit = q.get("limit", "20")
        if not re.fullmatch(r"[0-9]+", limit) or not 1 <= int(limit) <= 100:
            return self.bad("limit")
        limit = int(limit)
        ctx = [q.get("category"), nums.get("min_price"), nums.get("max_price"), in_stock, sort]
        after = None
        if "cursor" in q:
            after = read_cursor(q["cursor"], ctx)
            if after is None:
                return self.bad("cursor")
        key = SORTS[sort]
        page, more = [], False
        for p in ordered[sort]:
            if after is not None and key(p) <= after:
                continue
            if ctx[0] is not None and p["category"] != ctx[0]:
                continue
            if ctx[1] is not None and p["price_cents"] < ctx[1]:
                continue
            if ctx[2] is not None and p["price_cents"] > ctx[2]:
                continue
            if in_stock is not None and p["in_stock"] != (in_stock == "true"):
                continue
            if len(page) == limit:
                more = True
                break
            page.append(p)
        nxt = make_cursor(ctx, key(page[-1])) if more else None
        self.send(200, {"items": page, "next_cursor": nxt})

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
