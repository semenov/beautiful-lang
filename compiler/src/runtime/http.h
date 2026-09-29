// HTTP/1.1 server (tasks + non-blocking sockets) and client (libcurl).
// The compiler generates lt_http_dispatch, which turns a raw request into
// the language's `http.Request`, calls the handler and turns the
// `http.Response` back into raw parts.

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ctype.h>

typedef struct {
    const char *method;
    int64_t method_len;
    const char *path;
    int64_t path_len;
    const char *query;
    int64_t query_len;
    int64_t nheaders;
    const char *hname[64];
    int64_t hname_len[64];
    const char *hvalue[64];
    int64_t hvalue_len[64];
    const char *body;
    int64_t body_len;
} lt_http_raw;

typedef struct {
    int64_t status;
    lt_bytes *body;
    // headers as alternating names and values
    lt_texts *headers;
    // a file to send as the body (http.file)
    lt_text *file;
    // client requests: see lt_http_timing
    int64_t timing[5];
    bool reused;
    // writes the body (http.stream); fn is NULL when there is none
    lt_fn writer;
} lt_http_out;

static bool lt_ieq(const char *a, int64_t al, const char *b) {
    int64_t bl = (int64_t)strlen(b);
    if (al != bl) return false;
    for (int64_t i = 0; i < al; i++)
        if (tolower((unsigned char)a[i]) != b[i]) return false;
    return true;
}

static const char *lt_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "application/octet-stream";
    static const char *types[][2] = {
        { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" },
        { "css", "text/css; charset=utf-8" }, { "js", "text/javascript; charset=utf-8" },
        { "mjs", "text/javascript; charset=utf-8" }, { "json", "application/json" },
        { "txt", "text/plain; charset=utf-8" }, { "md", "text/markdown; charset=utf-8" },
        { "csv", "text/csv; charset=utf-8" }, { "xml", "application/xml" },
        { "svg", "image/svg+xml" }, { "png", "image/png" }, { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "webp", "image/webp" },
        { "avif", "image/avif" }, { "ico", "image/x-icon" }, { "pdf", "application/pdf" },
        { "wasm", "application/wasm" }, { "zip", "application/zip" }, { "gz", "application/gzip" },
        { "tar", "application/x-tar" }, { "mp4", "video/mp4" }, { "webm", "video/webm" },
        { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" }, { "wav", "audio/wav" },
        { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "ttf", "font/ttf" },
        { "otf", "font/otf" },
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcasecmp(dot + 1, types[i][0]) == 0) return types[i][1];
    return "application/octet-stream";
}

#ifdef LT_THREADS

static void lt_http_dispatch(lt_fn handler, const lt_http_raw *req, lt_http_out *out);

static volatile sig_atomic_t lt_http_stop;
static volatile int lt_http_listen_fd = -1;
static volatile int lt_http_port;

// Ctrl-C: stop accepting. Connecting to our own port wakes the accept loop
// (socket and connect are safe to call in a signal handler). The address
// family is the listener's: another program may hold the same port on the
// other family.
static void lt_http_on_signal(int sig) {
    (void)sig;
    lt_http_stop = 1;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (lt_http_listen_fd < 0 || getsockname(lt_http_listen_fd, (struct sockaddr *)&ss, &sl) != 0) return;
    int s = socket(ss.ss_family, SOCK_STREAM, 0);
    if (s < 0) return;
    if (ss.ss_family == AF_INET6) {
        ((struct sockaddr_in6 *)&ss)->sin6_addr = in6addr_loopback;
    } else {
        ((struct sockaddr_in *)&ss)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    connect(s, (struct sockaddr *)&ss, sl);
    close(s);
}

// read at least one byte into buf; 0 on end of stream / error
static ssize_t lt_sock_read(int fd, char *buf, size_t cap) {
    for (;;) {
        ssize_t n = read(fd, buf, cap);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!lt_io_wait(fd, false)) return 0;
            continue;
        }
        if (errno == EINTR) continue;
        return 0;
    }
}

static const char *lt_http_reason(int64_t s) {
    switch (s) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "";
    }
}


static void lt_http_simple(int fd, int status, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "HTTP/1.1 %d %s\r\ncontent-type: text/plain\r\ncontent-length: %d\r\nconnection: close\r\n\r\n%s", status, lt_http_reason(status), (int)strlen(msg), msg);
    lt_sock_write_all(fd, buf, (size_t)n);
}

typedef struct {
    int fd;
    lt_fn handler;
} lt_conn_arg;

// Writes the status line and headers: the handler's, then `extra` (lines
// ending in \r\n), content-length (chunked when length < 0) and
// connection; then `body` if given.
static bool lt_http_send(int fd, lt_http_out *out, const char *extra, int64_t length, bool keep, lt_bytes *body) {
    lt_buf b = { 0 };
    char line[256];
    int n = snprintf(line, sizeof line, "HTTP/1.1 %lld %s\r\n", (long long)out->status, lt_http_reason(out->status));
    lt_buf_put(&b, line, n);
    bool has_type = false;
    for (int64_t i = 0; out->headers && i + 1 < out->headers->len; i += 2) {
        lt_text *hn = out->headers->items[i], *hv = out->headers->items[i + 1];
        if (lt_ieq(hn->data, hn->len, "content-type")) has_type = true;
        if (lt_ieq(hn->data, hn->len, "content-length") || lt_ieq(hn->data, hn->len, "connection")) continue;
        // a value with line breaks is several headers (Set-Cookie)
        const char *v = hv->data, *end = hv->data + hv->len;
        while (v <= end) {
            const char *nl = memchr(v, '\n', (size_t)(end - v));
            const char *stop = nl ? nl : end;
            lt_buf_put(&b, hn->data, hn->len);
            lt_buf_put(&b, ": ", 2);
            lt_buf_put(&b, v, stop - v);
            lt_buf_put(&b, "\r\n", 2);
            if (!nl) break;
            v = nl + 1;
        }
    }
    if (!has_type && !extra && length != -2) lt_buf_put(&b, "content-type: text/plain; charset=utf-8\r\n", 41);
    if (extra) lt_buf_put(&b, extra, (int64_t)strlen(extra));
    if (length == -2) n = snprintf(line, sizeof line, "connection: Upgrade\r\n\r\n"); // 101: another protocol follows
    else if (length < 0) n = snprintf(line, sizeof line, "transfer-encoding: chunked\r\nconnection: %s\r\n\r\n", keep ? "keep-alive" : "close");
    else n = snprintf(line, sizeof line, "content-length: %lld\r\nconnection: %s\r\n\r\n", (long long)length, keep ? "keep-alive" : "close");
    lt_buf_put(&b, line, n);
    // small bodies go in the same write
    if (body && body->len <= 16384) {
        lt_buf_put(&b, (const char *)body->data, body->len);
        body = NULL;
    }
    bool ok = lt_sock_write_all(fd, b.d, (size_t)b.len) && (!body || lt_sock_write_all(fd, (const char *)body->data, (size_t)body->len));
    free(b.d);
    return ok;
}

static const char *lt_http_header(const lt_http_raw *r, const char *name) {
    static __thread char v[256];
    for (int64_t i = 0; i < r->nheaders; i++) {
        if (lt_ieq(r->hname[i], r->hname_len[i], name)) {
            snprintf(v, sizeof v, "%.*s", (int)r->hvalue_len[i], r->hvalue[i]);
            return v;
        }
    }
    return NULL;
}


// http.stream: the headers, then the writer's writes as chunks.
// A 101 answer (WebSockets) hands the writer the raw connection instead.
static bool lt_http_send_stream(int fd, const lt_http_raw *r, lt_http_out *out, bool keep, bool head_only) {
    bool upgrade = out->status == 101;
    if (!lt_http_send(fd, out, NULL, upgrade ? -2 : -1, keep, NULL)) return false;
    if (head_only && !upgrade) return lt_sock_write_all(fd, "0\r\n\r\n", 5);
    lt_conn *c = lt_conn_new(fd);
    c->borrowed = true;
    c->chunked = !upgrade;
    // the writer takes one reference; ours ends the stream after it
    lt_handle_dup(&c->h);
    lt_task *t = lt_current();
    jmp_buf jb;
    lt_err e;
    t->panic_jmp = &jb;
    if (setjmp(jb) != 0) {
        e = lt_make_failure(lt_text_cstr("the writer panicked"));
    } else {
        e = ((lt_err (*)(lt_env *, lt_handle *))out->writer.fn)(out->writer.env, &c->h);
    }
    t->panic_jmp = NULL;
    c->fd = -1;
    bool gone = c->gone;
    lt_handle_drop(&c->h);
    if (e.obj && gone) {
        // the client went away: normal for event streams
        lt_iface_drop(e);
        return false;
    }
    if (e.obj) {
        lt_text *m = lt_error_message(e);
        char pre[128];
        snprintf(pre, sizeof pre, "%.*s %.*s: streaming the response: ", (int)r->method_len, r->method, (int)(r->path_len > 40 ? 40 : r->path_len), r->path);
        lt_text *p = lt_text_cstr(pre);
        lt_text *parts[2] = { p, m };
        lt_text *line = lt_text_concat_n(2, parts);
        lt_log("ERROR", line);
        lt_text_drop(line);
        lt_text_drop(p);
        lt_text_drop(m);
        lt_iface_drop(e);
        return false; // cut: the client sees an unfinished body
    }
    if (upgrade) return false; // the connection ends with the other protocol
    return lt_sock_write_all(fd, "0\r\n\r\n", 5);
}

// http.file: 404, 304, 206 (Range) or 200 with the file sent by sendfile.
static bool lt_http_send_file(int fd, const lt_http_raw *r, lt_http_out *out, bool keep, bool head_only) {
    char path[4096];
    snprintf(path, sizeof path, "%.*s", (int)out->file->len, out->file->data);
    int f = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (f >= 0 && fstat(f, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(f);
        size_t pl = strlen(path);
        snprintf(path + pl, sizeof path - pl, "%sindex.html", pl && path[pl - 1] == '/' ? "" : "/");
        f = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (f < 0 || fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f >= 0) close(f);
        static struct { int64_t rc, len, cap; char d[9]; } nf = { -1, 9, 9, "not found" };
        lt_http_out o = { 404, NULL, NULL, NULL };
        return lt_http_send(fd, &o, NULL, 9, keep, head_only ? NULL : (lt_bytes *)&nf);
    }
    char extra[1024];
    int n = 0;
    bool has_type = false;
    for (int64_t i = 0; out->headers && i + 1 < out->headers->len; i += 2)
        if (lt_ieq(out->headers->items[i]->data, out->headers->items[i]->len, "content-type")) has_type = true;
    if (!has_type) n += snprintf(extra + n, sizeof extra - (size_t)n, "content-type: %s\r\n", lt_mime_type(path));
    char modified[64];
    struct tm tm;
    time_t mt = st.st_mtime;
    gmtime_r(&mt, &tm);
    strftime(modified, sizeof modified, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    n += snprintf(extra + n, sizeof extra - (size_t)n, "last-modified: %s\r\naccept-ranges: bytes\r\n", modified);
    const char *since = lt_http_header(r, "if-modified-since");
    if (since && strcmp(since, modified) == 0) {
        close(f);
        lt_http_out o = *out;
        o.status = 304;
        return lt_http_send(fd, &o, extra, 0, keep, NULL);
    }
    int64_t size = (int64_t)st.st_size, from = 0, count = size;
    int64_t status = out->status;
    const char *range = lt_http_header(r, "range");
    if (range && status == 200 && strncmp(range, "bytes=", 6) == 0 && !strchr(range, ',')) {
        const char *s = range + 6;
        char *e;
        int64_t a = -1, b = -1;
        if (*s == '-') {
            b = strtoll(s + 1, &e, 10); // the last b bytes
            a = size - b < 0 ? 0 : size - b;
            b = size - 1;
        } else {
            a = strtoll(s, &e, 10);
            b = (*e == '-' && e[1]) ? strtoll(e + 1, NULL, 10) : size - 1;
            if (b >= size) b = size - 1;
        }
        if (a < 0 || a >= size || b < a) {
            close(f);
            n += snprintf(extra + n, sizeof extra - (size_t)n, "content-range: bytes */%lld\r\n", (long long)size);
            lt_http_out o = *out;
            o.status = 416;
            return lt_http_send(fd, &o, extra, 0, keep, NULL);
        }
        from = a;
        count = b - a + 1;
        status = 206;
        n += snprintf(extra + n, sizeof extra - (size_t)n, "content-range: bytes %lld-%lld/%lld\r\n", (long long)a, (long long)b, (long long)size);
    }
    lt_http_out o = *out;
    o.status = status;
    bool ok;
    if (!head_only && count > 0 && count <= 16384) {
        // small: one write with the headers
        lt_bytes *small = lt_bytes_new(count);
        ssize_t got = pread(f, small->data, (size_t)count, (off_t)from);
        small->len = got > 0 ? got : 0;
        ok = got == count && lt_http_send(fd, &o, extra, count, keep, small);
        lt_bytes_drop(small);
    } else {
        ok = lt_http_send(fd, &o, extra, count, keep, NULL);
        if (ok && !head_only && count > 0) ok = lt_sendfile(f, fd, from, count) == count;
    }
    close(f);
    return ok;
}

#define LT_HTTP_MAX_HEAD (64 * 1024)
#define LT_HTTP_MAX_BODY (64 * 1024 * 1024)

static void lt_http_conn(lt_task *t) {
    lt_conn_arg a;
    memcpy(&a, t->result, sizeof a);
    int fd = a.fd;
    size_t cap = 16384, len = 0;
    char *buf = (char *)malloc(cap);
    bool keep = true;
    while (keep && !lt_http_stop) {
        // the head: up to the empty line
        char *end = NULL;
        size_t scanned = 0;
        for (;;) {
            if (len >= 4) {
                size_t from = scanned > 3 ? scanned - 3 : 0;
                for (size_t i = from; i + 3 < len; i++) {
                    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        end = buf + i;
                        break;
                    }
                }
                scanned = len;
            }
            if (end) break;
            if (len >= LT_HTTP_MAX_HEAD) {
                lt_http_simple(fd, 413, "request head too large");
                goto done;
            }
            if (len == cap) buf = (char *)realloc(buf, cap *= 2);
            ssize_t n = lt_sock_read(fd, buf + len, cap - len);
            if (n <= 0) goto done;
            len += (size_t)n;
        }
        lt_http_raw r;
        memset(&r, 0, sizeof r);
        char *p = buf, *head_end = end;
        // request line: METHOD SP TARGET SP VERSION
        char *sp1 = memchr(p, ' ', (size_t)(head_end - p));
        char *eol = memchr(p, '\r', (size_t)(head_end - p + 1));
        if (!sp1 || !eol) {
            lt_http_simple(fd, 400, "bad request line");
            goto done;
        }
        char *sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
        if (!sp2) {
            lt_http_simple(fd, 400, "bad request line");
            goto done;
        }
        r.method = p;
        r.method_len = sp1 - p;
        r.path = sp1 + 1;
        r.path_len = sp2 - sp1 - 1;
        char *q = memchr(r.path, '?', (size_t)r.path_len);
        if (q) {
            r.query = q + 1;
            r.query_len = r.path + r.path_len - q - 1;
            r.path_len = q - r.path;
        }
        bool http10 = (eol - sp2 - 1 == 8) && memcmp(sp2 + 1, "HTTP/1.0", 8) == 0;
        keep = !http10;
        int64_t content_length = 0;
        char *line = eol + 2;
        while (line < head_end) {
            char *le = memchr(line, '\r', (size_t)(head_end - line + 1));
            if (!le) le = head_end;
            char *colon = memchr(line, ':', (size_t)(le - line));
            if (colon && r.nheaders < 64) {
                char *v = colon + 1;
                while (v < le && (*v == ' ' || *v == '\t')) v++;
                char *ve = le;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                for (char *c = line; c < colon; c++) *c = (char)tolower((unsigned char)*c);
                r.hname[r.nheaders] = line;
                r.hname_len[r.nheaders] = colon - line;
                r.hvalue[r.nheaders] = v;
                r.hvalue_len[r.nheaders] = ve - v;
                if (lt_ieq(line, colon - line, "content-length")) content_length = strtoll(v, NULL, 10);
                if (lt_ieq(line, colon - line, "connection")) {
                    if (ve - v == 5 && strncasecmp(v, "close", 5) == 0) keep = false;
                    if (ve - v == 10 && strncasecmp(v, "keep-alive", 10) == 0) keep = true;
                }
                r.nheaders++;
            }
            line = le + 2;
        }
        if (content_length < 0 || content_length > LT_HTTP_MAX_BODY) {
            lt_http_simple(fd, 413, "request body too large");
            goto done;
        }
        size_t head_len = (size_t)(head_end - buf) + 4;
        size_t need = head_len + (size_t)content_length;
        while (len < need) {
            if (need > cap) {
                // the parsed pointers move with the buffer
                char *old = buf;
                cap = need;
                buf = (char *)realloc(buf, cap);
                ptrdiff_t d = buf - old;
                r.method += d;
                r.path += d;
                if (r.query) r.query += d;
                for (int64_t i = 0; i < r.nheaders; i++) {
                    r.hname[i] += d;
                    r.hvalue[i] += d;
                }
            }
            ssize_t n = lt_sock_read(fd, buf + len, cap - len);
            if (n <= 0) goto done;
            len += (size_t)n;
        }
        r.body = buf + head_len;
        r.body_len = content_length;
        lt_http_out out;
        memset(&out, 0, sizeof out);
        lt_http_dispatch(a.handler, &r, &out);
        bool head_only = r.method_len == 4 && memcmp(r.method, "HEAD", 4) == 0;
        bool ok;
        if (out.writer.fn) ok = lt_http_send_stream(fd, &r, &out, keep, head_only);
        else if (out.file && out.file->len > 0) ok = lt_http_send_file(fd, &r, &out, keep, head_only);
        else ok = lt_http_send(fd, &out, NULL, out.body ? out.body->len : 0, keep, head_only ? NULL : out.body);
        lt_fn_drop(out.writer);
        lt_bytes_drop(out.body);
        lt_text_drop(out.file);
        if (out.headers) {
            for (int64_t i = 0; i < out.headers->len; i++) lt_text_drop(out.headers->items[i]);
            lt_free(out.headers, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)out.headers->cap);
        }
        if (!ok) break;
        // keep the rest (a pipelined next request)
        memmove(buf, buf + need, len - need);
        len -= need;
    }
done:
    close(fd);
    free(buf);
    lt_fn_drop(a.handler);
}

static lt_err lt_http_serve(int64_t port, lt_fn handler) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = fd >= 0;
    if (!v6) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return lt_make_failure(lt_text_cstr("http: can't create a socket"));
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int r;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof addr);
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons((uint16_t)port);
        addr.sin6_addr = in6addr_any;
        r = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    } else {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        r = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    }
    if (r != 0 || listen(fd, 1024) != 0) {
        char buf[128];
        snprintf(buf, sizeof buf, "http: can't listen on port %lld: %s", (long long)port, strerror(errno));
        close(fd);
        return lt_make_failure(lt_text_cstr(buf));
    }
    lt_set_nonblocking(fd);
    lt_http_listen_fd = fd;
    lt_http_port = (int)port;
    struct sigaction old_sigs[2];
    lt_signals_take(lt_http_on_signal, old_sigs);
    signal(SIGPIPE, SIG_IGN);
    {
        char msg[96];
        snprintf(msg, sizeof msg, "listening on http://localhost:%lld", (long long)port);
        lt_text *m = lt_text_cstr(msg);
        lt_log("INFO", m);
        lt_text_drop(m);
    }
    while (!lt_http_stop) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!lt_io_wait(fd, false)) break;
                continue;
            }
            if (errno == EINTR || errno == ECONNABORTED) continue;
            break;
        }
        if (lt_http_stop) {
            close(c);
            break;
        }
        lt_set_nonblocking(c);
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        lt_conn_arg ca = { c, handler };
        lt_fn_dup(handler);
        lt_spawn_detached(lt_http_conn, &ca, sizeof ca);
    }
    lt_http_listen_fd = -1;
    close(fd);
    lt_signals_restore(old_sigs);
    lt_text *m = lt_text_cstr("stopped");
    lt_log("INFO", m);
    lt_text_drop(m);
    return (lt_err){ 0 };
}

#endif // LT_THREADS

// ---------------------------------------------------------------- client

#ifdef LT_CURL
#include <curl/curl.h>
#include <pthread.h>

typedef struct {
    char *d;
    size_t len, cap;
} lt_grow;

static size_t lt_curl_body(char *p, size_t size, size_t n, void *ud) {
    lt_grow *g = (lt_grow *)ud;
    size_t add = size * n;
    if (g->len + add + 1 > g->cap) {
        g->cap = (g->len + add + 1) * 2;
        g->d = (char *)realloc(g->d, g->cap);
    }
    memcpy(g->d + g->len, p, add);
    g->len += add;
    return add;
}

static size_t lt_curl_header(char *p, size_t size, size_t n, void *ud) {
    lt_texts **hs = (lt_texts **)ud;
    size_t len = size * n;
    char *colon = memchr(p, ':', len);
    if (colon) {
        char *v = colon + 1;
        char *e = p + len;
        while (v < e && (*v == ' ' || *v == '\t')) v++;
        while (e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) e--;
        lt_text *name = lt_text_from(p, colon - p);
        for (int64_t i = 0; i < name->len; i++) name->data[i] = (char)tolower((unsigned char)name->data[i]);
        lt_texts_push(hs, name);
        lt_texts_push(hs, lt_text_from(v, e - v));
    } else if (len >= 5 && memcmp(p, "HTTP/", 5) == 0) {
        // a new response (after a redirect): forget earlier headers
        for (int64_t i = 0; i < (*hs)->len; i++) lt_text_drop((*hs)->items[i]);
        (*hs)->len = 0;
    }
    return len;
}

// One connection cache (and DNS cache, TLS sessions) for the whole
// program: requests reuse open connections (keep-alive) whatever task or
// thread they run on.
static CURLSH *lt_curl_share;
static pthread_mutex_t lt_share_mu[CURL_LOCK_DATA_LAST];

static void lt_share_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u) {
    (void)h, (void)a, (void)u;
    pthread_mutex_lock(&lt_share_mu[d]);
}
static void lt_share_unlock(CURL *h, curl_lock_data d, void *u) {
    (void)h, (void)u;
    pthread_mutex_unlock(&lt_share_mu[d]);
}

static void curl_global_init_void(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&lt_share_mu[i], NULL);
    lt_curl_share = curl_share_init();
    curl_share_setopt(lt_curl_share, CURLSHOPT_LOCKFUNC, lt_share_lock);
    curl_share_setopt(lt_curl_share, CURLSHOPT_UNLOCKFUNC, lt_share_unlock);
    curl_share_setopt(lt_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(lt_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(lt_curl_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}

static size_t lt_curl_file(char *p, size_t size, size_t n, void *ud) {
    return fwrite(p, size, n, (FILE *)ud) * size;
}

static void lt_texts_free_all(lt_texts *l) {
    for (int64_t i = 0; i < l->len; i++) lt_text_drop(l->items[i]);
    if (l->cap) lt_free(l, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)l->cap);
}

// Where the time went, in microseconds from the start: DNS, connected,
// TLS done, first byte, total; and whether the connection was reused.
static void lt_http_timing(CURL *c, int64_t *t, bool *reused) {
    curl_off_t v = 0;
    curl_easy_getinfo(c, CURLINFO_NAMELOOKUP_TIME_T, &v);
    t[0] = v;
    curl_easy_getinfo(c, CURLINFO_CONNECT_TIME_T, &v);
    t[1] = v;
    curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME_T, &v);
    t[2] = v;
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME_T, &v);
    t[3] = v;
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME_T, &v);
    t[4] = v;
    long n = 0;
    curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &n);
    *reused = n == 0;
}

// A request's options. `headers` alternate names and values; the body, the
// header list and `errbuf` (CURL_ERROR_SIZE) must live until the end.
static CURL *lt_http_setup(const char *method, const char *url, lt_bytes *body, lt_texts *headers, int64_t timeout_ms, bool follow, char *errbuf, struct curl_slist **slist) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, (void (*)(void))curl_global_init_void);
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_SHARE, lt_curl_share);
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (strcmp(method, "HEAD") == 0) curl_easy_setopt(c, CURLOPT_NOBODY, 1L); // no body will come
    else if (strcmp(method, "GET") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    // after a 301/302/303 a POST becomes a GET, as browsers (and Go) do
    curl_easy_setopt(c, CURLOPT_POSTREDIR, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    if (errbuf) {
        errbuf[0] = 0;
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    }
    if (timeout_ms > 0) {
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_ms < 10000 ? timeout_ms : 10000));
    }
    curl_easy_setopt(c, CURLOPT_USERAGENT, "lang-http/0.1");
#if !defined(__APPLE__)
    const char *ca = lt_ca_file();
    if (ca) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
#endif
    bool has_type = false;
    *slist = NULL;
    for (int64_t i = 0; headers && i + 1 < headers->len; i += 2) {
        lt_text *n = headers->items[i], *v = headers->items[i + 1];
        if (lt_ieq(n->data, n->len, "content-type")) has_type = true;
        size_t len = (size_t)(n->len + v->len + 3);
        char *line = (char *)malloc(len);
        snprintf(line, len, "%s: %s", n->data, v->data);
        *slist = curl_slist_append(*slist, line);
        free(line);
    }
    bool sends = body && (body->len > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0);
    if (sends) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, (const char *)body->data);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->len);
        if (!has_type) {
            bool js = body->len > 0 && (body->data[0] == '{' || body->data[0] == '[');
            *slist = curl_slist_append(*slist, js ? "content-type: application/json" : "content-type: text/plain; charset=utf-8");
        }
    }
    if (*slist) curl_easy_setopt(c, CURLOPT_HTTPHEADER, *slist);
    return c;
}

static lt_err lt_http_failed(const char *method, const char *url, CURLcode rc, const char *errbuf) {
    char buf[768];
    const char *why = errbuf && errbuf[0] ? errbuf : curl_easy_strerror(rc);
#if !defined(__APPLE__)
    if (!lt_ca_file() && (rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_PEER_FAILED_VERIFICATION)) why = LT_NO_CA;
#endif
    if (rc == CURLE_OPERATION_TIMEDOUT) why = "no answer in time (see `timeout`)";
    if (rc == CURLE_TOO_MANY_REDIRECTS) why = "more than 10 redirects";
    snprintf(buf, sizeof buf, "http: %s %s failed: %s", method, url, why);
    return lt_make_failure(lt_text_cstr(buf));
}

static int lt_curl_cancel_check(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a, (void)b, (void)c, (void)d;
    return __atomic_load_n((volatile int *)ud, __ATOMIC_ACQUIRE) ? 1 : 0;
}

// One request; the body goes to `sink` (a growing buffer or a FILE).
// `cancel`, if given, stops the transfer when it becomes non-zero.
static lt_err lt_http_perform_on(const char *method, const char *url, lt_bytes *body, lt_texts *headers, int64_t timeout_ms, bool follow, size_t (*write)(char *, size_t, size_t, void *), void *sink, lt_http_out *out, volatile int *cancel) {
    struct curl_slist *slist;
    char errbuf[CURL_ERROR_SIZE];
    CURL *c = lt_http_setup(method, url, body, headers, timeout_ms, follow, errbuf, &slist);
    if (!c) return lt_make_failure(lt_text_cstr("http: can't start the client"));
    if (cancel) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, lt_curl_cancel_check);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, (void *)cancel);
    }
    lt_texts *hs = lt_texts_new(8);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, lt_curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hs);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    lt_http_timing(c, out->timing, &out->reused);
    lt_err e = { 0 };
    if (rc != CURLE_OK) e = lt_http_failed(method, url, rc, errbuf);
    curl_easy_cleanup(c);
    curl_slist_free_all(slist);
    if (e.obj) {
        lt_texts_free_all(hs);
        return e;
    }
    out->status = status;
    out->headers = hs;
    return (lt_err){ 0 };
}

// ---- a pool of threads for blocking work (curl), reused between requests

typedef struct lt_pool_job {
    void (*run)(void *);
    void *arg;
    struct lt_pool_job *next;
} lt_pool_job;

static pthread_mutex_t lt_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lt_pool_cv = PTHREAD_COND_INITIALIZER;
static lt_pool_job *lt_pool_head, *lt_pool_tail;
static int lt_pool_idle;

static void *lt_pool_thread(void *u) {
    (void)u;
    pthread_mutex_lock(&lt_pool_mu);
    for (;;) {
        while (!lt_pool_head) {
            lt_pool_idle++;
            pthread_cond_wait(&lt_pool_cv, &lt_pool_mu);
            lt_pool_idle--;
        }
        lt_pool_job *j = lt_pool_head;
        lt_pool_head = j->next;
        if (!lt_pool_head) lt_pool_tail = NULL;
        pthread_mutex_unlock(&lt_pool_mu);
        j->run(j->arg);
        free(j);
        pthread_mutex_lock(&lt_pool_mu);
    }
    return NULL;
}

static bool lt_pool_submit(void (*run)(void *), void *arg) {
    lt_pool_job *j = (lt_pool_job *)malloc(sizeof(lt_pool_job));
    j->run = run;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(&lt_pool_mu);
    if (lt_pool_tail) lt_pool_tail->next = j;
    else lt_pool_head = j;
    lt_pool_tail = j;
    bool ok = true;
    if (lt_pool_idle > 0) {
        pthread_cond_signal(&lt_pool_cv);
    } else {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        ok = pthread_create(&th, &a, lt_pool_thread, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&lt_pool_mu);
    return ok;
}

// With tasks, a request runs on a pool thread while the task waits on a
// pipe: other tasks keep running, any number of requests run at once, and
// cancelling the task (time.timeout) stops the transfer.
typedef struct {
    const char *method, *url;
    lt_bytes *body;
    lt_texts *headers;
    int64_t timeout_ms;
    bool follow;
    size_t (*write)(char *, size_t, size_t, void *);
    void *sink;
    lt_http_out out;
    lt_err err;
    volatile int cancel;
    int done_w;
} lt_hjob;

static void lt_hjob_run(void *ud) {
    lt_hjob *j = (lt_hjob *)ud;
    j->err = lt_http_perform_on(j->method, j->url, j->body, j->headers, j->timeout_ms, j->follow, j->write, j->sink, &j->out, &j->cancel);
    int w = j->done_w; // `j` may be gone once the byte is written
    char x = 1;
    while (write(w, &x, 1) < 0 && errno == EINTR) {
    }
}

static lt_err lt_http_perform(const char *method, const char *url, lt_bytes *body, lt_texts *headers, int64_t timeout_ms, bool follow, size_t (*write)(char *, size_t, size_t, void *), void *sink, lt_http_out *out) {
#ifdef LT_THREADS
    int p[2];
    if (pipe(p) != 0) return lt_http_perform_on(method, url, body, headers, timeout_ms, follow, write, sink, out, NULL);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    fcntl(p[1], F_SETFD, FD_CLOEXEC);
    lt_set_nonblocking(p[0]);
    lt_hjob j;
    memset(&j, 0, sizeof j);
    j.method = method;
    j.url = url;
    j.body = body;
    j.headers = headers;
    j.timeout_ms = timeout_ms;
    j.follow = follow;
    j.write = write;
    j.sink = sink;
    j.done_w = p[1];
    if (!lt_pool_submit(lt_hjob_run, &j)) {
        close(p[0]);
        close(p[1]);
        return lt_http_perform_on(method, url, body, headers, timeout_ms, follow, write, sink, out, NULL);
    }
    bool cancelled = false;
    for (;;) {
        char x;
        ssize_t r = read(p[0], &x, 1);
        if (r == 1) break;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!cancelled && lt_io_wait(p[0], false)) continue;
            // cancelled: stop the transfer, then wait for the thread to let
            // go of `j` (on this thread: it takes at most a moment)
            __atomic_store_n(&j.cancel, 1, __ATOMIC_RELEASE);
            cancelled = true;
            struct pollfd pf = { p[0], POLLIN, 0 };
            poll(&pf, 1, -1);
            continue;
        }
        break;
    }
    close(p[0]);
    close(p[1]);
    if (cancelled) {
        if (!j.err.obj) lt_texts_free_all(j.out.headers);
        else lt_iface_drop(j.err);
        return lt_make_cancelled();
    }
    *out = j.out;
    return j.err;
#else
    return lt_http_perform_on(method, url, body, headers, timeout_ms, follow, write, sink, out, NULL);
#endif
}

static lt_err lt_http_send_request(lt_text *method, lt_text *url, lt_texts *headers, lt_bytes *body, int64_t timeout_ms, bool follow, lt_http_out *out) {
    lt_grow g = { NULL, 0, 0 };
    lt_err e = lt_http_perform(method->data, url->data, body, headers, timeout_ms, follow, lt_curl_body, &g, out);
    if (!e.obj) out->body = lt_bytes_from(g.d ? g.d : "", (int64_t)g.len);
    free(g.d);
    return e;
}

// Into `path` through a temporary file, so a failed download leaves no
// half-written file behind.
static lt_err lt_http_download(lt_text *url, lt_text *path, lt_http_out *out) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.download-%d", path->data, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return lt_os_error("can't create", path);
    lt_err e = lt_http_perform("GET", url->data, NULL, NULL, 0, true, lt_curl_file, f, out);
    bool written = fclose(f) == 0;
    if (!e.obj && out->status / 100 == 2) {
        if (!written || rename(tmp, path->data) != 0) {
            unlink(tmp);
            lt_texts_free_all(out->headers);
            return lt_os_error("can't write", path);
        }
    } else {
        unlink(tmp);
    }
    if (!e.obj) out->body = LT_EMPTY_BYTES;
    return e;
}

// ---- http.open: the body read piece by piece.
// curl runs on its own thread and writes the body into a pipe; the program
// reads the other end as a stream, so waiting doesn't block other tasks.

typedef struct lt_hstream {
    lt_handle h;
    lt_conn *body; // the pipe's reading end
    int wfd;       // its writing end (the curl thread's)
    int ready_r, ready_w;
    bool ready_sent, joined, reused, follow;
    volatile int cancel;
    pthread_t th;
    CURL *c;
    struct curl_slist *slist;
    lt_bytes *req_body;
    lt_texts *hs;
    int64_t status;
    int64_t timing[5];
    CURLcode result;
    char *method, *url;
    char errbuf[CURL_ERROR_SIZE];
} lt_hstream;

static void lt_hstream_ready(lt_hstream *s) {
    if (s->ready_sent) return;
    long code = 0;
    curl_easy_getinfo(s->c, CURLINFO_RESPONSE_CODE, &code);
    s->status = code;
    lt_http_timing(s->c, s->timing, &s->reused);
    s->ready_sent = true;
    char x = 1;
    while (write(s->ready_w, &x, 1) < 0 && errno == EINTR) {
    }
}

static size_t lt_hstream_header(char *p, size_t size, size_t n, void *ud) {
    lt_hstream *s = (lt_hstream *)ud;
    if (s->ready_sent) return size * n; // trailers
    size_t len = lt_curl_header(p, size, n, &s->hs);
    if (len <= 2 && (p[0] == '\r' || p[0] == '\n')) {
        // the end of a header block: final unless it's 1xx or a redirect curl follows
        long code = 0;
        curl_easy_getinfo(s->c, CURLINFO_RESPONSE_CODE, &code);
        bool has_location = false;
        for (int64_t i = 0; i + 1 < s->hs->len; i += 2)
            if (lt_ieq(s->hs->items[i]->data, s->hs->items[i]->len, "location")) has_location = true;
        // a redirect curl will follow isn't the answer yet
        if (code >= 200 && !(code >= 300 && code < 400 && has_location && s->follow)) lt_hstream_ready(s);
    }
    return len;
}

static size_t lt_hstream_write(char *p, size_t size, size_t n, void *ud) {
    lt_hstream *s = (lt_hstream *)ud;
    lt_hstream_ready(s);
    size_t len = size * n, done = 0;
    while (done < len) {
        ssize_t w = write(s->wfd, p + done, len - done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return 0; // the reader closed: stop the transfer
    }
    return len;
}

static int lt_hstream_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a, (void)b, (void)c, (void)d;
    return ((lt_hstream *)ud)->cancel ? 1 : 0;
}

static void *lt_hstream_run(void *ud) {
    lt_hstream *s = (lt_hstream *)ud;
    s->result = curl_easy_perform(s->c);
    lt_hstream_ready(s);
    int64_t t[5];
    bool r;
    lt_http_timing(s->c, t, &r);
    s->timing[4] = t[4];
    close(s->wfd);
    s->wfd = -1;
    return NULL;
}

static void lt_hstream_join(lt_hstream *s) {
    if (s->joined) return;
    s->cancel = 1;
    // the curl thread may be writing: let it fail
    lt_conn_release(s->body);
    pthread_join(s->th, NULL);
    s->joined = true;
}

// at the end of the body: was it all there?
static lt_err lt_hstream_eof(lt_conn *c) {
    lt_hstream *s = (lt_hstream *)c->owner;
    if (!s->joined) {
        pthread_join(s->th, NULL);
        s->joined = true;
    }
    if (s->result != CURLE_OK) return lt_http_failed(s->method, s->url, s->result, s->errbuf);
    return (lt_err){ 0 };
}

static void lt_hstream_free(lt_handle *h) {
    lt_hstream *s = (lt_hstream *)h;
    lt_hstream_join(s);
    close(s->ready_r);
    close(s->ready_w);
    curl_easy_cleanup(s->c);
    curl_slist_free_all(s->slist);
    lt_bytes_drop(s->req_body);
    lt_texts_free_all(s->hs);
    lt_handle_drop(&s->body->h);
    free(s->method);
    free(s->url);
    free(s);
}

static lt_err lt_http_open(lt_text *method, lt_text *url, lt_texts *headers, lt_bytes *body, int64_t timeout_ms, bool follow, lt_handle **out) {
    int bp[2], rp[2];
    if (pipe(bp) != 0) return lt_make_failure(lt_text_cstr("http: can't make a pipe"));
    if (pipe(rp) != 0) {
        close(bp[0]);
        close(bp[1]);
        return lt_make_failure(lt_text_cstr("http: can't make a pipe"));
    }
    for (int i = 0; i < 2; i++) {
        fcntl(bp[i], F_SETFD, FD_CLOEXEC);
        fcntl(rp[i], F_SETFD, FD_CLOEXEC);
    }
    lt_set_nonblocking(bp[0]);
    lt_set_nonblocking(rp[0]);
    signal(SIGPIPE, SIG_IGN);
    lt_hstream *s = (lt_hstream *)calloc(1, sizeof(lt_hstream));
    s->h.rc = 1;
    s->h.free = lt_hstream_free;
    s->method = strdup(method->data);
    s->url = strdup(url->data);
    s->follow = follow;
    s->req_body = body;
    lt_bytes_dup(body);
    s->hs = lt_texts_new(8);
    s->wfd = bp[1];
    s->ready_r = rp[0];
    s->ready_w = rp[1];
    s->body = lt_conn_new(bp[0]);
    s->body->owner = s;
    s->body->on_eof = lt_hstream_eof;
    s->body->label = "the response";
    s->c = lt_http_setup(s->method, s->url, body, headers, timeout_ms, follow, s->errbuf, &s->slist);
    curl_easy_setopt(s->c, CURLOPT_WRITEFUNCTION, lt_hstream_write);
    curl_easy_setopt(s->c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(s->c, CURLOPT_HEADERFUNCTION, lt_hstream_header);
    curl_easy_setopt(s->c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(s->c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(s->c, CURLOPT_XFERINFOFUNCTION, lt_hstream_progress);
    curl_easy_setopt(s->c, CURLOPT_XFERINFODATA, s);
    if (pthread_create(&s->th, NULL, lt_hstream_run, s) != 0) {
        s->joined = true;
        close(s->wfd);
        lt_hstream_free(&s->h);
        return lt_make_failure(lt_text_cstr("http: can't start a thread"));
    }
    // wait for the status and headers
    for (;;) {
        char x;
        ssize_t r = read(s->ready_r, &x, 1);
        if (r == 1) break;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!lt_io_wait(s->ready_r, false)) {
                lt_hstream_free(&s->h); // stops the transfer
                return lt_make_cancelled();
            }
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    if (s->status == 0) {
        // failed before any answer
        pthread_join(s->th, NULL);
        s->joined = true;
        lt_err e = lt_http_failed(s->method, s->url, s->result != CURLE_OK ? s->result : CURLE_GOT_NOTHING, s->errbuf);
        lt_hstream_free(&s->h);
        return e;
    }
    *out = &s->h;
    return (lt_err){ 0 };
}

static lt_text *lt_hstream_header_get(lt_handle *h, lt_text *name) {
    lt_hstream *s = (lt_hstream *)h;
    for (int64_t i = 0; i + 1 < s->hs->len; i += 2)
        if (lt_ieq(name->data, name->len, s->hs->items[i]->data)) {
            lt_text_dup(s->hs->items[i + 1]);
            return s->hs->items[i + 1];
        }
    return NULL;
}
#endif
