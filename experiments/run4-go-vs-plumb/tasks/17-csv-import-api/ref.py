#!/usr/bin/env python3
import json, os, re, socketserver, sqlite3, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

lock = threading.Lock()
db = sqlite3.connect(os.environ["DB_PATH"], check_same_thread=False)
db.execute("CREATE TABLE IF NOT EXISTS users (email TEXT PRIMARY KEY, name TEXT NOT NULL, age INTEGER NOT NULL)")
db.commit()


class CsvError(Exception):
    pass


def parse_csv(text):
    records, rec, i, n = [], [], 0, len(text)
    if n == 0:
        raise CsvError()
    while True:
        # start of a field
        if i < n and text[i] == '"':
            i += 1
            buf = []
            while True:
                if i >= n:
                    raise CsvError()
                if text[i] == '"':
                    if i + 1 < n and text[i + 1] == '"':
                        buf.append('"'); i += 2; continue
                    i += 1
                    break
                buf.append(text[i]); i += 1
            if i < n and text[i] not in ",\n" and text[i:i + 2] != "\r\n":
                raise CsvError()
            field = "".join(buf)
        else:
            j = i
            while j < n and text[j] not in ",\n" and text[j:j + 2] != "\r\n":
                if text[j] == '"':
                    raise CsvError()
                j += 1
            field, i = text[i:j], j
        rec.append(field)
        if i >= n:
            records.append(rec)
            return records
        if text[i] == ",":
            i += 1
            continue
        i += 2 if text[i] == "\r" else 1
        records.append(rec)
        rec = []
        if i >= n:
            return records


EMAIL_RE = re.compile(r"[^@\s]+@[^@\s]+")


def email_ok(e):
    if len(e) > 254 or not EMAIL_RE.fullmatch(e):
        return False
    dom = e.split("@")[1]
    return "." in dom[1:-1]


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

    def do_import(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        try:
            recs = parse_csv(raw.decode("utf-8"))
        except (CsvError, UnicodeDecodeError):
            return self.send(400, {"error": "invalid csv"})
        hdr = recs[0]
        if sorted(hdr) != ["age", "email", "name"]:
            return self.send(400, {"error": "invalid csv"})
        idx = {h: k for k, h in enumerate(hdr)}
        with lock:
            errors, seen, rows = [], set(), []
            for num, rec in enumerate(recs[1:], 1):
                if len(rec) != 3:
                    errors.append({"row": num, "field": "row", "message": "wrong number of fields"})
                    continue
                email, name, age = rec[idx["email"]], rec[idx["name"]], rec[idx["age"]]
                low = email.lower()
                if not email_ok(email):
                    errors.append({"row": num, "field": "email", "message": "invalid email"})
                elif db.execute("SELECT 1 FROM users WHERE email = ?", (low,)).fetchone():
                    errors.append({"row": num, "field": "email", "message": "email already exists"})
                elif low in seen:
                    errors.append({"row": num, "field": "email", "message": "duplicate email in file"})
                seen.add(low)
                if not 1 <= len(name) <= 100:
                    errors.append({"row": num, "field": "name", "message": "invalid name"})
                if not re.fullmatch(r"[0-9]+", age) or not 13 <= int(age) <= 120:
                    errors.append({"row": num, "field": "age", "message": "invalid age"})
                rows.append((low, name, age))
            if errors:
                return self.send(422, {"imported": 0, "errors": errors})
            with db:
                db.executemany("INSERT INTO users VALUES (?,?,?)", [(e, n, int(a)) for e, n, a in rows])
        return self.send(201, {"imported": len(rows)})

    def route(self, method):
        p = urlsplit(self.path).path
        allowed = {"/import": "POST", "/users": "GET"}
        if p not in allowed:
            return self.send(404, {"error": "not found"})
        if method != allowed[p]:
            return self.send(405, {"error": "method not allowed"})
        if p == "/import":
            return self.do_import()
        with lock:
            rs = db.execute("SELECT email, name, age FROM users ORDER BY email").fetchall()
        rs.sort(key=lambda r: r[0])  # code point order regardless of collation
        self.send(200, {"users": [{"email": e, "name": n, "age": a} for e, n, a in rs]})

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
