#!/usr/bin/env python3
import json, os, re, socketserver, sqlite3, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

lock = threading.Lock()
db = sqlite3.connect(os.environ["DB_PATH"], check_same_thread=False)
db.executescript("""
CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL,
                                   capacity INTEGER NOT NULL, remaining INTEGER NOT NULL CHECK (remaining >= 0));
CREATE TABLE IF NOT EXISTS reservations (id INTEGER PRIMARY KEY AUTOINCREMENT, event_id INTEGER NOT NULL,
                                         seats INTEGER NOT NULL, status TEXT NOT NULL, idem_key TEXT UNIQUE);
""")
db.commit()


def is_int(v):
    return type(v) is int


def ev(r):
    return {"id": r[0], "name": r[1], "capacity": r[2], "remaining": r[3]}


def res(r):
    return {"id": r[0], "event_id": r[1], "seats": r[2], "status": r[3]}


def get_res(rid):
    return db.execute("SELECT id, event_id, seats, status FROM reservations WHERE id = ?", (rid,)).fetchone()


def get_ev(eid):
    return db.execute("SELECT id, name, capacity, remaining FROM events WHERE id = ?", (eid,)).fetchone()


ID = r"([1-9][0-9]*)"


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

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            v = json.loads(self.rfile.read(n).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None
        return v if isinstance(v, dict) else None

    def route(self, method):
        p = urlsplit(self.path).path
        routes = [(r"/events", "POST", self.create_event), (r"/events/[^/]+", "GET", self.show_event),
                  (r"/events/[^/]+/reservations", "POST", self.reserve),
                  (r"/reservations/[^/]+", "GET", self.show_res), (r"/reservations/[^/]+", "DELETE", self.cancel)]
        matched = [(m, f) for pat, m, f in routes if re.fullmatch(pat, p)]
        if not matched:
            return self.send(404, {"error": "not found"})
        fn = next((f for m, f in matched if m == method), None)
        if fn is None:
            return self.send(405, {"error": "method not allowed"})
        parts = p.split("/")
        if len(parts) > 2 and not re.fullmatch(ID, parts[2]):
            return self.send(404, {"error": "not found"})
        return fn(int(parts[2]) if len(parts) > 2 else None)

    def create_event(self, _):
        b = self.body()
        if b is None:
            return self.send(400, {"error": "invalid JSON"})
        name, cap = b.get("name"), b.get("capacity")
        if not isinstance(name, str) or name == "":
            return self.send(400, {"error": "invalid name"})
        if not is_int(cap) or not 1 <= cap <= 100000:
            return self.send(400, {"error": "invalid capacity"})
        with lock, db:
            cur = db.execute("INSERT INTO events(name, capacity, remaining) VALUES (?,?,?)", (name, cap, cap))
        return self.send(201, {"id": cur.lastrowid, "name": name, "capacity": cap, "remaining": cap})

    def show_event(self, eid):
        with lock:
            r = get_ev(eid)
        return self.send(200, ev(r)) if r else self.send(404, {"error": "not found"})

    def show_res(self, rid):
        with lock:
            r = get_res(rid)
        return self.send(200, res(r)) if r else self.send(404, {"error": "not found"})

    def reserve(self, eid):
        b = self.body()
        if b is None:
            return self.send(400, {"error": "invalid JSON"})
        seats = b.get("seats")
        if not is_int(seats) or seats < 1:
            return self.send(400, {"error": "invalid seats"})
        key = self.headers.get("Idempotency-Key")
        with lock:
            if key:
                r = db.execute("SELECT id, event_id, seats, status FROM reservations WHERE idem_key = ?", (key,)).fetchone()
                if r:
                    if r[1] != eid or r[2] != seats:
                        return self.send(422, {"error": "idempotency key reused"})
                    return self.send(201, res(r))
            e = get_ev(eid)
            if e is None:
                return self.send(404, {"error": "not found"})
            if e[3] < seats:
                return self.send(409, {"error": "not enough seats"})
            with db:
                db.execute("UPDATE events SET remaining = remaining - ? WHERE id = ?", (seats, eid))
                cur = db.execute("INSERT INTO reservations(event_id, seats, status, idem_key) VALUES (?,?,'active',?)",
                                 (eid, seats, key or None))
        return self.send(201, {"id": cur.lastrowid, "event_id": eid, "seats": seats, "status": "active"})

    def cancel(self, rid):
        with lock:
            r = get_res(rid)
            if r is None:
                return self.send(404, {"error": "not found"})
            if r[3] == "cancelled":
                return self.send(409, {"error": "already cancelled"})
            with db:
                db.execute("UPDATE reservations SET status = 'cancelled' WHERE id = ?", (rid,))
                db.execute("UPDATE events SET remaining = remaining + ? WHERE id = ?", (r[2], r[1]))
        return self.send(200, {"id": rid, "event_id": r[1], "seats": r[2], "status": "cancelled"})

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
