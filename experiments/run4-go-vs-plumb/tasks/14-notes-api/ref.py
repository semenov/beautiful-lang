#!/usr/bin/env python3
import json, os, re, socketserver, sqlite3, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

lock = threading.Lock()
db = sqlite3.connect(os.environ["DB_PATH"], check_same_thread=False)
db.execute("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, body TEXT NOT NULL, tag TEXT)")
db.commit()


def row(r):
    return {"id": r[0], "title": r[1], "body": r[2], "tag": r[3]}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None):
        data = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        if obj is not None:
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

    def handle_any(self, method):
        u = urlsplit(self.path)
        p = u.path
        if p == "/notes":
            if method == "GET":
                q = parse_qs(u.query, keep_blank_values=True).get("q", [""])[0]
                with lock:
                    rs = db.execute("SELECT id,title,body,tag FROM notes WHERE instr(title, ?) > 0 OR instr(body, ?) > 0 OR ? = '' ORDER BY id", (q, q, q)).fetchall()
                return self.send(200, {"notes": [row(r) for r in rs]})
            if method == "POST":
                b = self.body()
                if b is None:
                    return self.send(400, {"error": "invalid JSON"})
                t = b.get("title")
                if not isinstance(t, str) or not 1 <= len(t) <= 200:
                    return self.send(400, {"error": "invalid field: title"})
                bd = b.get("body", "")
                if not isinstance(bd, str):
                    return self.send(400, {"error": "invalid field: body"})
                tag = b.get("tag")
                if tag is not None and not isinstance(tag, str):
                    return self.send(400, {"error": "invalid field: tag"})
                with lock:
                    cur = db.execute("INSERT INTO notes(title,body,tag) VALUES (?,?,?)", (t, bd, tag))
                    db.commit()
                return self.send(201, {"id": cur.lastrowid, "title": t, "body": bd, "tag": tag})
            return self.send(405, {"error": "method not allowed"})
        m = re.fullmatch(r"/notes/([^/]+)", p)
        if not m:
            return self.send(404, {"error": "not found"})
        if method not in ("GET", "PATCH", "DELETE"):
            return self.send(405, {"error": "method not allowed"})
        sid = m.group(1)
        if not re.fullmatch(r"[0-9]+", sid) or int(sid) == 0:
            return self.send(404, {"error": "not found"})
        nid = int(sid)
        if method == "PATCH":
            b = self.body()
            if b is None:
                return self.send(400, {"error": "invalid JSON"})
            if "title" in b and (not isinstance(b["title"], str) or not 1 <= len(b["title"]) <= 200):
                return self.send(400, {"error": "invalid field: title"})
            if "body" in b and not isinstance(b["body"], str):
                return self.send(400, {"error": "invalid field: body"})
            if "tag" in b and b["tag"] is not None and not isinstance(b["tag"], str):
                return self.send(400, {"error": "invalid field: tag"})
        with lock:
            r = db.execute("SELECT id,title,body,tag FROM notes WHERE id=?", (nid,)).fetchone()
            if r is None:
                return self.send(404, {"error": "not found"})
            if method == "GET":
                return self.send(200, row(r))
            if method == "DELETE":
                db.execute("DELETE FROM notes WHERE id=?", (nid,))
                db.commit()
                return self.send(204)
            n = row(r)
            for k in ("title", "body", "tag"):
                if k in b:
                    n[k] = b[k]
            db.execute("UPDATE notes SET title=?, body=?, tag=? WHERE id=?", (n["title"], n["body"], n["tag"], nid))
            db.commit()
            return self.send(200, n)

    def do_GET(self): self.handle_any("GET")
    def do_POST(self): self.handle_any("POST")
    def do_PATCH(self): self.handle_any("PATCH")
    def do_DELETE(self): self.handle_any("DELETE")
    def do_PUT(self): self.handle_any("PUT")


class Server(ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True

    def server_bind(self):  # skip the reverse DNS lookup HTTPServer does
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


Server(("127.0.0.1", int(os.environ["PORT"])), H).serve_forever()
