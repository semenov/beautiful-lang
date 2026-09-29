// net: TCP connections and UDP sockets on the task scheduler.

#include <netdb.h>

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

// ---- TLS clients: Secure Transport on macOS, OpenSSL elsewhere

// waits until the socket is ready; false when the task is cancelled
static bool lt_tls_wait(lt_conn *c, bool for_write, lt_err *err) {
    if (lt_is_cancelled()) {
        *err = lt_make_cancelled();
        return false;
    }
    lt_io_wait(c->fd, for_write);
    return true;
}

#if !defined(LT_TLS_ON)

#elif defined(__APPLE__) && !defined(LT_TLS_OPENSSL)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>
#include <Security/SecureTransport.h>

// Secure Transport calls these to move encrypted bytes; they wait on the
// scheduler, so a slow server blocks only this task.
static OSStatus lt_st_read(SSLConnectionRef ref, void *d, size_t *len) {
    lt_conn *c = (lt_conn *)ref;
    size_t want = *len, got = 0;
    while (got < want) {
        ssize_t n = read(c->fd, (char *)d + got, want - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            *len = got;
            return errSSLClosedGraceful;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (lt_is_cancelled()) break;
            lt_io_wait(c->fd, false);
            continue;
        }
        *len = got;
        return errSSLClosedAbort;
    }
    *len = got;
    return got == want ? noErr : errSSLClosedAbort;
}

static OSStatus lt_st_write(SSLConnectionRef ref, const void *d, size_t *len) {
    lt_conn *c = (lt_conn *)ref;
    if (!lt_sock_write_all(c->fd, (const char *)d, *len)) return errSSLClosedAbort;
    return noErr;
}

static lt_err lt_st_error(const char *what, OSStatus s) {
    char detail[256] = "";
    CFStringRef m = SecCopyErrorMessageString(s, NULL);
    if (m) {
        CFStringGetCString(m, detail, sizeof detail, kCFStringEncodingUTF8);
        CFRelease(m);
    }
    if (!detail[0]) snprintf(detail, sizeof detail, "TLS error %d", (int)s);
    return lt_net_error(what, detail);
}

static void lt_tls_free(lt_conn *c) {
    SSLContextRef ctx = (SSLContextRef)c->tls;
    c->tls = NULL;
    SSLClose(ctx);
    CFRelease(ctx);
}

static lt_err lt_tls_start(lt_conn *c, const char *host, const char *what) {
    SSLContextRef ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!ctx) return lt_net_error(what, "can't start TLS");
    c->tls = ctx;
    SSLSetIOFuncs(ctx, lt_st_read, lt_st_write);
    SSLSetConnection(ctx, (SSLConnectionRef)c);
    SSLSetPeerDomainName(ctx, host, strlen(host));
    SSLSetProtocolVersionMin(ctx, kTLSProtocol12);
    OSStatus s = SSLHandshake(ctx);
    if (s != noErr) return lt_st_error(what, s);
    return (lt_err){ 0 };
}

static ssize_t lt_tls_recv(lt_conn *c, void *d, size_t n, lt_err *err) {
    size_t got = 0;
    OSStatus s = SSLRead((SSLContextRef)c->tls, d, n, &got);
    if (got > 0) return (ssize_t)got;
    if (s == errSSLClosedGraceful || s == errSSLClosedNoNotify) return 0;
    if (lt_is_cancelled()) {
        *err = lt_make_cancelled();
        return 0;
    }
    *err = lt_st_error("read", s);
    return 0;
}

static bool lt_tls_send(lt_conn *c, const void *d, size_t n, lt_err *err) {
    size_t done = 0;
    while (done < n) {
        size_t k = 0;
        OSStatus s = SSLWrite((SSLContextRef)c->tls, (const char *)d + done, n - done, &k);
        if (s != noErr) {
            *err = lt_st_error("write", s);
            return false;
        }
        done += k;
    }
    return true;
}

#pragma clang diagnostic pop

#else

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

static SSL_CTX *lt_ssl_ctx;
static pthread_once_t lt_ssl_once = PTHREAD_ONCE_INIT;

static void lt_ssl_init(void) {
    lt_ssl_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(lt_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(lt_ssl_ctx);
    SSL_CTX_set_verify(lt_ssl_ctx, SSL_VERIFY_PEER, NULL);
}

static lt_err lt_ssl_error(const char *what, SSL *ssl) {
    char detail[256] = "";
    long v = ssl ? SSL_get_verify_result(ssl) : X509_V_OK;
    unsigned long e = ERR_get_error();
    if (v != X509_V_OK) {
        snprintf(detail, sizeof detail, "the server's certificate is not trusted: %s", X509_verify_cert_error_string(v));
    } else if (e) {
        ERR_error_string_n(e, detail, sizeof detail);
    } else {
        snprintf(detail, sizeof detail, "%s", errno ? strerror(errno) : "the connection was closed");
    }
    ERR_clear_error();
    return lt_net_error(what, detail);
}

static void lt_tls_free(lt_conn *c) {
    SSL *ssl = (SSL *)c->tls;
    c->tls = NULL;
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

// runs an SSL call until it stops asking to wait; returns its result
#define LT_SSL_RETRY(c, err, call, r)                                   \
    for (;;) {                                                          \
        r = (call);                                                     \
        if (r > 0) break;                                               \
        int k_ = SSL_get_error((SSL *)(c)->tls, r);                     \
        if (k_ == SSL_ERROR_WANT_READ || k_ == SSL_ERROR_WANT_WRITE) {  \
            if (!lt_tls_wait(c, k_ == SSL_ERROR_WANT_WRITE, err)) break; \
            continue;                                                   \
        }                                                               \
        break;                                                          \
    }

static lt_err lt_tls_start(lt_conn *c, const char *host, const char *what) {
    pthread_once(&lt_ssl_once, lt_ssl_init);
    SSL *ssl = SSL_new(lt_ssl_ctx);
    c->tls = ssl;
    SSL_set_fd(ssl, c->fd);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set1_host(ssl, host);
    lt_err err = { 0 };
    int r;
    LT_SSL_RETRY(c, &err, SSL_connect(ssl), r);
    if (err.obj) return err;
    if (r != 1) return lt_ssl_error(what, ssl);
    return (lt_err){ 0 };
}

static ssize_t lt_tls_recv(lt_conn *c, void *d, size_t n, lt_err *err) {
    int r;
    LT_SSL_RETRY(c, err, SSL_read((SSL *)c->tls, d, (int)(n > INT32_MAX ? INT32_MAX : n)), r);
    if (r > 0 || err->obj) return r > 0 ? r : 0;
    int k = SSL_get_error((SSL *)c->tls, r);
    if (k == SSL_ERROR_ZERO_RETURN || (k == SSL_ERROR_SYSCALL && ERR_peek_error() == 0)) return 0;
    *err = lt_ssl_error("read", NULL);
    return 0;
}

static bool lt_tls_send(lt_conn *c, const void *d, size_t n, lt_err *err) {
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done > INT32_MAX ? INT32_MAX : n - done;
        int r;
        LT_SSL_RETRY(c, err, SSL_write((SSL *)c->tls, (const char *)d + done, (int)chunk), r);
        if (err->obj) return false;
        if (r <= 0) {
            *err = lt_ssl_error("write", NULL);
            return false;
        }
        done += (size_t)r;
    }
    return true;
}

#endif

#if defined(LT_TLS_ON)
static lt_err lt_net_connect_tls(lt_text *host, int64_t port, lt_handle **out) {
    lt_handle *h = NULL;
    lt_err e = lt_net_connect(host, port, &h);
    if (e.obj) return e;
    lt_conn *c = (lt_conn *)h;
    c->tls_recv = lt_tls_recv;
    c->tls_send = lt_tls_send;
    c->tls_free = lt_tls_free;
    char what[300];
    snprintf(what, sizeof what, "TLS with %s:%lld", host->data, (long long)port);
    e = lt_tls_start(c, host->data, what);
    if (e.obj) {
        lt_handle_drop(h);
        return e;
    }
    *out = h;
    return (lt_err){ 0 };
}
#endif
