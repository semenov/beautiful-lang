// net: TCP connections and UDP sockets on the task scheduler.

#include <netdb.h>

static lt_text *lt_error_message(lt_err e);

typedef struct lt_conn {
    lt_handle h;
    int fd;
    char *buf; // bytes read ahead (for read_line)
    size_t blen, bcap;
    char peer[64];
} lt_conn;

static void lt_conn_free(lt_handle *h) {
    lt_conn *c = (lt_conn *)h;
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    free(c);
}

static lt_conn *lt_conn_new(int fd) {
    lt_conn *c = (lt_conn *)calloc(1, sizeof(lt_conn));
    c->h.rc = 1;
    c->h.free = lt_conn_free;
    c->fd = fd;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0) {
        char host[48] = "?";
        int port = 0;
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
            port = ntohs(a->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
            port = ntohs(a->sin6_port);
            if (strncmp(host, "::ffff:", 7) == 0) memmove(host, host + 7, strlen(host + 7) + 1);
        }
        snprintf(c->peer, sizeof c->peer, "%s:%d", host, port);
    }
    return c;
}

static lt_err lt_net_error(const char *what, const char *detail) {
    char buf[512];
    snprintf(buf, sizeof buf, "net: %s: %s", what, detail);
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_net_connect(lt_text *host, int64_t port, lt_handle **out) {
    char ps[16];
    snprintf(ps, sizeof ps, "%lld", (long long)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host->data, ps, &hints, &res);
    char what[300];
    snprintf(what, sizeof what, "can't connect to %s:%lld", host->data, (long long)port);
    if (g != 0) return lt_net_error(what, gai_strerror(g));
    int err = ECONNREFUSED;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        lt_set_nonblocking(fd);
        int one = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS) {
            lt_io_wait(fd, true);
            socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            r = err == 0 ? 0 : -1;
        } else if (r != 0) {
            err = errno;
        }
        if (r == 0) {
            freeaddrinfo(res);
            *out = &lt_conn_new(fd)->h;
            return (lt_err){ 0 };
        }
        close(fd);
    }
    freeaddrinfo(res);
    return lt_net_error(what, strerror(err));
}

// fills the read-ahead buffer with at least one more byte; false at the end
static lt_err lt_conn_fill(lt_conn *c, bool *eof) {
    if (c->fd < 0) return lt_net_error("read", "the connection is closed");
    if (c->bcap - c->blen < 4096) {
        c->bcap = c->bcap ? c->bcap * 2 : 8192;
        c->buf = (char *)realloc(c->buf, c->bcap);
    }
    for (;;) {
        ssize_t n = read(c->fd, c->buf + c->blen, c->bcap - c->blen);
        if (n > 0) {
            c->blen += (size_t)n;
            *eof = false;
            return (lt_err){ 0 };
        }
        if (n == 0) {
            *eof = true;
            return (lt_err){ 0 };
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (lt_is_cancelled()) return lt_make_cancelled();
            lt_io_wait(c->fd, false);
            continue;
        }
        if (errno == EINTR) continue;
        return lt_net_error("read", strerror(errno));
    }
}

static lt_bytes *lt_conn_take(lt_conn *c, size_t n) {
    lt_bytes *b = lt_bytes_from(c->buf, (int64_t)n);
    memmove(c->buf, c->buf + n, c->blen - n);
    c->blen -= n;
    return b;
}

static lt_err lt_conn_read(lt_handle *h, int64_t max, lt_bytes **out) {
    lt_conn *c = (lt_conn *)h;
    if (max <= 0) {
        *out = LT_EMPTY_BYTES;
        return (lt_err){ 0 };
    }
    if (c->blen == 0) {
        bool eof = false;
        lt_err e = lt_conn_fill(c, &eof);
        if (e.obj) return e;
        if (eof) {
            *out = LT_EMPTY_BYTES;
            return (lt_err){ 0 };
        }
    }
    *out = lt_conn_take(c, c->blen < (size_t)max ? c->blen : (size_t)max);
    return (lt_err){ 0 };
}

static lt_err lt_conn_read_exact(lt_handle *h, int64_t n, lt_bytes **out) {
    lt_conn *c = (lt_conn *)h;
    while (c->blen < (size_t)n) {
        bool eof = false;
        lt_err e = lt_conn_fill(c, &eof);
        if (e.obj) return e;
        if (eof) return lt_net_error("read", "the connection closed in the middle of a message");
    }
    *out = lt_conn_take(c, (size_t)n);
    return (lt_err){ 0 };
}

static lt_err lt_conn_read_line(lt_handle *h, lt_text **out) {
    lt_conn *c = (lt_conn *)h;
    size_t scanned = 0;
    for (;;) {
        char *nl = c->blen > scanned ? memchr(c->buf + scanned, '\n', c->blen - scanned) : NULL;
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            size_t len = n > 0 && c->buf[n - 1] == '\r' ? n - 1 : n;
            *out = lt_text_from(c->buf, (int64_t)len);
            memmove(c->buf, c->buf + n + 1, c->blen - n - 1);
            c->blen -= n + 1;
            return (lt_err){ 0 };
        }
        scanned = c->blen;
        bool eof = false;
        lt_err e = lt_conn_fill(c, &eof);
        if (e.obj) return e;
        if (eof) {
            if (c->blen == 0) {
                *out = NULL;
                return (lt_err){ 0 };
            }
            *out = lt_text_from(c->buf, (int64_t)c->blen);
            c->blen = 0;
            return (lt_err){ 0 };
        }
    }
}

static lt_err lt_conn_write_raw(lt_handle *h, const void *d, int64_t n) {
    lt_conn *c = (lt_conn *)h;
    if (c->fd < 0) return lt_net_error("write", "the connection is closed");
    if (!lt_sock_write_all(c->fd, (const char *)d, (size_t)n)) return lt_net_error("write", strerror(errno));
    return (lt_err){ 0 };
}

static lt_err lt_conn_close(lt_handle *h) {
    lt_conn *c = (lt_conn *)h;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    return (lt_err){ 0 };
}

// ---- serve: a task per connection

typedef struct {
    int fd;
    lt_fn handler;
} lt_net_conn_arg;

static void lt_net_conn_task(lt_task *t) {
    lt_net_conn_arg a;
    memcpy(&a, t->result, sizeof a);
    lt_conn *c = lt_conn_new(a.fd);
    // the handler takes one reference; ours closes the connection after it
    LT_INC(&c->h);
    lt_err e = ((lt_err (*)(lt_env *, lt_handle *))a.handler.fn)(a.handler.env, &c->h);
    if (e.obj) {
        lt_text *m = lt_error_message(e);
        char pre[96];
        snprintf(pre, sizeof pre, "connection from %s: ", c->peer);
        lt_text *p = lt_text_cstr(pre);
        lt_text *parts[2] = { p, m };
        lt_text *line = lt_text_concat_n(2, parts);
        lt_log("ERROR", line);
        lt_text_drop(line);
        lt_text_drop(p);
        lt_text_drop(m);
        lt_iface_drop(e);
    }
    lt_conn_close(&c->h);
    lt_handle_drop(&c->h);
    lt_fn_drop(a.handler);
}

static lt_err lt_net_serve(int64_t port, lt_fn handler) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1, zero = 0;
    if (fd < 0) return lt_net_error("serve", strerror(errno));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof addr);
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)port);
    addr.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 1024) != 0) {
        char w[64];
        snprintf(w, sizeof w, "can't listen on port %lld", (long long)port);
        lt_err e = lt_net_error(w, strerror(errno));
        close(fd);
        return e;
    }
    lt_set_nonblocking(fd);
    lt_http_listen_fd = fd;
    lt_http_port = (int)port;
    signal(SIGINT, lt_http_on_signal);
    signal(SIGTERM, lt_http_on_signal);
    signal(SIGPIPE, SIG_IGN);
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
#ifdef SO_NOSIGPIPE
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        lt_net_conn_arg ca = { c, handler };
        lt_fn_dup(handler);
        lt_spawn_detached(lt_net_conn_task, &ca, sizeof ca);
    }
    lt_http_listen_fd = -1;
    close(fd);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    return (lt_err){ 0 };
}

// ---- UDP

typedef struct {
    lt_handle h;
    int fd;
    int64_t port;
} lt_udp;

static void lt_udp_free(lt_handle *h) {
    lt_udp *u = (lt_udp *)h;
    if (u->fd >= 0) close(u->fd);
    free(u);
}

static lt_err lt_net_udp(int64_t port, lt_handle **out) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    int zero = 0;
    if (fd < 0) return lt_net_error("udp", strerror(errno));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof addr);
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons((uint16_t)port);
    addr.sin6_addr = in6addr_any;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        lt_err e = lt_net_error("udp: can't bind", strerror(errno));
        close(fd);
        return e;
    }
    socklen_t sl = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &sl);
    lt_set_nonblocking(fd);
    lt_udp *u = (lt_udp *)calloc(1, sizeof(lt_udp));
    u->h.rc = 1;
    u->h.free = lt_udp_free;
    u->fd = fd;
    u->port = ntohs(addr.sin6_port);
    *out = &u->h;
    return (lt_err){ 0 };
}

static lt_err lt_udp_send_to(lt_handle *h, lt_bytes *data, lt_text *address) {
    lt_udp *u = (lt_udp *)h;
    const char *colon = strrchr(address->data, ':');
    if (!colon) return lt_net_error("send_to", "the address must be \"host:port\"");
    char host[256];
    size_t hl = (size_t)(colon - address->data);
    if (hl >= sizeof host) hl = sizeof host - 1;
    memcpy(host, address->data, hl);
    host[hl] = 0;
    if (host[0] == '[') {
        memmove(host, host + 1, strlen(host));
        host[strlen(host) - 1] = 0;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_V4MAPPED | AI_ALL;
    int g = getaddrinfo(host, colon + 1, &hints, &res);
    if (g != 0) return lt_net_error("send_to", gai_strerror(g));
    for (;;) {
        ssize_t n = sendto(u->fd, data->data, (size_t)data->len, 0, res->ai_addr, res->ai_addrlen);
        if (n >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            lt_io_wait(u->fd, true);
            continue;
        }
        freeaddrinfo(res);
        return lt_net_error("send_to", strerror(errno));
    }
    freeaddrinfo(res);
    return (lt_err){ 0 };
}

static lt_err lt_udp_receive(lt_handle *h, lt_bytes **data, lt_text **from) {
    lt_udp *u = (lt_udp *)h;
    unsigned char buf[65536];
    struct sockaddr_storage ss;
    for (;;) {
        socklen_t sl = sizeof ss;
        ssize_t n = recvfrom(u->fd, buf, sizeof buf, 0, (struct sockaddr *)&ss, &sl);
        if (n >= 0) {
            char host[64] = "?";
            int port = 0;
            if (ss.ss_family == AF_INET6) {
                struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
                inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
                port = ntohs(a->sin6_port);
                if (strncmp(host, "::ffff:", 7) == 0) memmove(host, host + 7, strlen(host + 7) + 1);
            } else if (ss.ss_family == AF_INET) {
                struct sockaddr_in *a = (struct sockaddr_in *)&ss;
                inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
                port = ntohs(a->sin_port);
            }
            char f[96];
            snprintf(f, sizeof f, strchr(host, ':') ? "[%s]:%d" : "%s:%d", host, port);
            *data = lt_bytes_from(buf, n);
            *from = lt_text_cstr(f);
            return (lt_err){ 0 };
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (lt_is_cancelled()) return lt_make_cancelled();
            lt_io_wait(u->fd, false);
            continue;
        }
        if (errno == EINTR) continue;
        return lt_net_error("receive", strerror(errno));
    }
}

static lt_err lt_udp_close(lt_handle *h) {
    lt_udp *u = (lt_udp *)h;
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
    return (lt_err){ 0 };
}
