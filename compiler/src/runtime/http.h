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
    lt_text *body;
    // headers as alternating names and values
    lt_texts *headers;
} lt_http_out;

#ifdef LT_THREADS

static void lt_http_dispatch(lt_fn handler, const lt_http_raw *req, lt_http_out *out);

static volatile sig_atomic_t lt_http_stop;
static volatile int lt_http_listen_fd = -1;
static volatile int lt_http_port;

// Ctrl-C: stop accepting. Connecting to our own port wakes the accept loop
// (socket and connect are safe to call in a signal handler).
static void lt_http_on_signal(int sig) {
    (void)sig;
    lt_http_stop = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)lt_http_port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(s, (struct sockaddr *)&a, sizeof a);
        close(s);
    }
}

static void lt_set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

// read at least one byte into buf; 0 on end of stream / error
static ssize_t lt_sock_read(int fd, char *buf, size_t cap) {
    for (;;) {
        ssize_t n = read(fd, buf, cap);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            lt_io_wait(fd, false);
            continue;
        }
        if (errno == EINTR) continue;
        return 0;
    }
}

static bool lt_sock_write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            lt_io_wait(fd, true);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static const char *lt_http_reason(int64_t s) {
    switch (s) {
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

static bool lt_ieq(const char *a, int64_t al, const char *b) {
    int64_t bl = (int64_t)strlen(b);
    if (al != bl) return false;
    for (int64_t i = 0; i < al; i++)
        if (tolower((unsigned char)a[i]) != b[i]) return false;
    return true;
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
        // the response
        size_t hcap = 256;
        for (int64_t i = 0; out.headers && i < out.headers->len; i++) hcap += (size_t)out.headers->items[i]->len + 4;
        char *head = (char *)malloc(hcap + 128);
        int hl = snprintf(head, hcap + 128, "HTTP/1.1 %lld %s\r\n", (long long)out.status, lt_http_reason(out.status));
        bool has_type = false;
        for (int64_t i = 0; out.headers && i + 1 < out.headers->len; i += 2) {
            lt_text *hn = out.headers->items[i], *hv = out.headers->items[i + 1];
            if (lt_ieq(hn->data, hn->len, "content-type")) has_type = true;
            if (lt_ieq(hn->data, hn->len, "content-length") || lt_ieq(hn->data, hn->len, "connection")) continue;
            hl += snprintf(head + hl, hcap + 128 - (size_t)hl, "%.*s: %.*s\r\n", (int)hn->len, hn->data, (int)hv->len, hv->data);
        }
        if (!has_type) hl += snprintf(head + hl, hcap + 128 - (size_t)hl, "content-type: text/plain; charset=utf-8\r\n");
        hl += snprintf(head + hl, hcap + 128 - (size_t)hl, "content-length: %lld\r\nconnection: %s\r\n\r\n", (long long)(out.body ? out.body->len : 0), keep ? "keep-alive" : "close");
        bool ok = lt_sock_write_all(fd, head, (size_t)hl) && (!out.body || lt_sock_write_all(fd, out.body->data, (size_t)out.body->len));
        free(head);
        lt_text_drop(out.body);
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
    signal(SIGINT, lt_http_on_signal);
    signal(SIGTERM, lt_http_on_signal);
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
                lt_io_wait(fd, false);
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
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
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

static void curl_global_init_void(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static lt_err lt_http_fetch(lt_text *method, lt_text *url, lt_text *body, lt_http_out *out) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, (void (*)(void))curl_global_init_void);
    CURL *c = curl_easy_init();
    if (!c) return lt_make_failure(lt_text_cstr("http: can't start the client"));
    lt_grow g = { NULL, 0, 0 };
    lt_texts *hs = lt_texts_new(8);
    curl_easy_setopt(c, CURLOPT_URL, url->data);
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method->data);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "lang-http/0.1");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, lt_curl_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, lt_curl_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hs);
    struct curl_slist *req_headers = NULL;
    if (body->len > 0 || strcmp(method->data, "POST") == 0 || strcmp(method->data, "PUT") == 0) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body->len);
        const char *ct = (body->len > 0 && (body->data[0] == '{' || body->data[0] == '[')) ? "content-type: application/json" : "content-type: text/plain; charset=utf-8";
        req_headers = curl_slist_append(req_headers, ct);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);
    }
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        char buf[512];
        snprintf(buf, sizeof buf, "http: %s %s failed: %s", method->data, url->data, curl_easy_strerror(rc));
        curl_easy_cleanup(c);
        curl_slist_free_all(req_headers);
        free(g.d);
        for (int64_t i = 0; i < hs->len; i++) lt_text_drop(hs->items[i]);
        lt_free(hs, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)hs->cap);
        return lt_make_failure(lt_text_cstr(buf));
    }
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(req_headers);
    out->status = status;
    out->body = lt_text_from(g.d ? g.d : "", (int64_t)g.len);
    free(g.d);
    out->headers = hs;
    return (lt_err){ 0 };
}
#endif
