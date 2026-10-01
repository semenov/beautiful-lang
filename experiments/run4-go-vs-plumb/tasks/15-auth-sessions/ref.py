#!/usr/bin/env python3
import hashlib, hmac, json, os, re, secrets, socketserver, sqlite3, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

TTL = int(os.environ.get("SESSION_TTL_SECONDS") or 3600)
lock = threading.Lock()
db = sqlite3.connect(os.environ["DB_PATH"], check_same_thread=False)
db.executescript("""
CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT NOT NULL, ukey TEXT NOT NULL UNIQUE,
                                  salt BLOB NOT NULL, hash BLOB NOT NULL);
CREATE TABLE IF NOT EXISTS sessions (token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, expires REAL NOT NULL);
""")
db.commit()


def hash_pw(pw, salt):
    return hashlib.pbkdf2_hmac("sha256", pw.encode("utf-8"), salt, 50000)


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

    def session_user(self):
        m = re.fullmatch(r"Bearer ([A-Za-z0-9]+)", self.headers.get("Authorization") or "")
        if not m:
            return None, None
        with lock:
            r = db.execute("SELECT u.username FROM sessions s JOIN users u ON u.id = s.user_id "
                           "WHERE s.token = ? AND s.expires > ?", (m.group(1), time.time())).fetchone()
        return (m.group(1), r[0]) if r else (None, None)

    def route(self, method):
        p = urlsplit(self.path).path
        allowed = {"/register": "POST", "/login": "POST", "/logout": "POST", "/me": "GET"}
        if p not in allowed:
            return self.send(404, {"error": "not found"})
        if method != allowed[p]:
            return self.send(405, {"error": "method not allowed"})
        if p in ("/register", "/login"):
            b = self.body()
            if b is None:
                return self.send(400, {"error": "invalid JSON"})
            u, pw = b.get("username"), b.get("password")
            if not isinstance(u, str) or (p == "/register" and not re.fullmatch(r"[A-Za-z0-9_]{3,32}", u)):
                return self.send(400, {"error": "invalid username"})
            if not isinstance(pw, str) or (p == "/register" and not 8 <= len(pw) <= 128):
                return self.send(400, {"error": "invalid password"})
            if p == "/register":
                salt = secrets.token_bytes(16)
                h = hash_pw(pw, salt)
                with lock:
                    try:
                        db.execute("INSERT INTO users(username, ukey, salt, hash) VALUES (?,?,?,?)",
                                   (u, u.lower(), salt, h))
                        db.commit()
                    except sqlite3.IntegrityError:
                        db.rollback()
                        return self.send(409, {"error": "username taken"})
                return self.send(201, {"username": u})
            with lock:
                r = db.execute("SELECT id, salt, hash FROM users WHERE ukey = ?", (u.lower(),)).fetchone()
            if r is None or not hmac.compare_digest(hash_pw(pw, r[1]), r[2]):
                return self.send(401, {"error": "invalid credentials"})
            tok = secrets.token_hex(24)
            with lock:
                db.execute("INSERT INTO sessions VALUES (?,?,?)", (tok, r[0], time.time() + TTL))
                db.commit()
            return self.send(200, {"token": tok})
        tok, user = self.session_user()
        if user is None:
            return self.send(401, {"error": "unauthorized"})
        if p == "/me":
            return self.send(200, {"username": user})
        with lock:
            db.execute("DELETE FROM sessions WHERE token = ?", (tok,))
            db.commit()
        return self.send(204)

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
