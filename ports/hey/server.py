import http.server, sys, time, threading
conns = set(); lock = threading.Lock()
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def handle_one(self):
        with lock: conns.add(self.client_address)
        p = self.path
        n = int(self.headers.get('content-length') or 0)
        body_in = self.rfile.read(n) if n else b''
        if p.startswith('/conns'):
            with lock: b = (str(len(conns))+"\n").encode(); conns.clear()
            return self.send(200, b)
        if p.startswith('/redirect'):
            self.send_response(302); self.send_header('Location','/hello'); self.send_header('Content-Length','0'); self.end_headers(); return
        if p.startswith('/slow'):
            time.sleep(float(p.split('=')[1]) if '=' in p else 2); return self.send(200, b'slow\n')
        if p.startswith('/trickle'):
            self.send_response(200); self.send_header('Content-Length','10'); self.end_headers()
            for i in range(10):
                self.wfile.write(b'x'); self.wfile.flush(); time.sleep(0.5)
            return
        if p.startswith('/404'): return self.send(404, b'nope\n')
        if p.startswith('/500'): return self.send(500, b'err\n')
        if p.startswith('/mixed'):
            import random
            return self.send(random.choice([200,200,200,404,503]), b'x'*100)
        if p.startswith('/echo'):
            info = f"{self.command} {self.path}\n" + "".join(f"{k}: {v}\n" for k,v in self.headers.items()) + f"\nbody={body_in!r}\n"
            print(info, file=sys.stderr, flush=True)
            return self.send(200, info.encode())
        return self.send(200, b'hello world, this is a 64-byte response body for load testing!!\n')
    def send(self, code, b):
        self.send_response(code); self.send_header('Content-Length', str(len(b))); self.send_header('Content-Type','text/plain'); self.end_headers()
        if self.command != 'HEAD': self.wfile.write(b)
    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = do_OPTIONS = do_PATCH = handle_one
http.server.ThreadingHTTPServer.request_queue_size = 1024
http.server.ThreadingHTTPServer(('127.0.0.1', 8099), H).serve_forever()
