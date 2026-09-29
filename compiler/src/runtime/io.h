// io: streams of bytes. Files, network connections, other programs' input
// and output, and this program's standard streams are all one C type,
// lt_conn: a file descriptor with a read-ahead buffer (for read_line) and,
// for files, a write buffer.

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <signal.h>
#if defined(__APPLE__)
#include <sys/uio.h>
#else
#include <sys/sendfile.h>
#endif

#ifndef LT_THREADS
// without tasks, waiting on a descriptor simply blocks the program
static void lt_io_wait(int fd, bool write) {
    struct pollfd p = { fd, (short)(write ? POLLOUT : POLLIN), 0 };
    while (poll(&p, 1, -1) < 0 && errno == EINTR) {
    }
}
LT_INLINE bool lt_is_cancelled(void) { return false; }
static lt_err lt_make_cancelled(void);
#endif

static lt_text *lt_error_message(lt_err e);

static void lt_set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

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

typedef struct lt_conn {
    lt_handle h;
    int fd;
    char *buf; // bytes read ahead (for read_line)
    size_t blen, bcap;
    char *wbuf; // bytes not yet written (files)
    size_t wlen;
    bool buffered;  // files: collect writes, flush at 64 KB and on close
    bool borrowed;  // standard streams: close doesn't close the descriptor
    bool is_stdout; // flush print's buffer first, to keep the order
    char peer[64];
    lt_text *path; // files and programs: for messages
    const char *label; // standard streams: for messages
    // TLS (net.connect_tls)
    void *tls;
    ssize_t (*tls_recv)(struct lt_conn *, void *, size_t, lt_err *);
    bool (*tls_send)(struct lt_conn *, const void *, size_t, lt_err *);
    void (*tls_free)(struct lt_conn *);
} lt_conn;

#define LT_WBUF (64 * 1024)

static lt_err lt_net_error(const char *what, const char *detail) {
    char buf[512];
    snprintf(buf, sizeof buf, "net: %s: %s", what, detail);
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_conn_error(lt_conn *c, const char *what, const char *detail) {
    char buf[1024];
    if (c->label) snprintf(buf, sizeof buf, "can't %s %s: %s", what, c->label, detail);
    else if (c->path) snprintf(buf, sizeof buf, "can't %s \"%.*s\": %s", what, (int)c->path->len, c->path->data, detail);
    else return lt_net_error(what, detail);
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_conn_flush(lt_conn *c) {
    if (c->wlen == 0) return (lt_err){ 0 };
    size_t n = c->wlen;
    c->wlen = 0;
    if (!lt_sock_write_all(c->fd, c->wbuf, n)) return lt_conn_error(c, "write", strerror(errno));
    return (lt_err){ 0 };
}

static void lt_conn_release(lt_conn *c) {
    if (c->tls) c->tls_free(c);
    if (c->fd >= 0 && !c->borrowed) close(c->fd);
    c->fd = -1;
}

static void lt_conn_free(lt_handle *h) {
    lt_conn *c = (lt_conn *)h;
    if (c->fd >= 0) lt_conn_flush(c);
    lt_conn_release(c);
    free(c->buf);
    free(c->wbuf);
    lt_text_drop(c->path);
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

// fills the read-ahead buffer with at least one more byte; *eof at the end
static lt_err lt_conn_fill(lt_conn *c, bool *eof) {
    if (c->fd < 0) return lt_conn_error(c, "read", "it is closed");
    if (c->wlen) {
        lt_err e = lt_conn_flush(c);
        if (e.obj) return e;
    }
    if (c->bcap - c->blen < 4096) {
        c->bcap = c->bcap ? c->bcap * 2 : 16384;
        c->buf = (char *)realloc(c->buf, c->bcap);
    }
    if (c->tls) {
        lt_err e = { 0 };
        ssize_t n = c->tls_recv(c, c->buf + c->blen, c->bcap - c->blen, &e);
        if (e.obj) return e;
        c->blen += (size_t)n;
        *eof = n == 0;
        return (lt_err){ 0 };
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
        return lt_conn_error(c, "read", strerror(errno));
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
        if (eof) return lt_conn_error(c, "read", "it ended in the middle of a message");
    }
    *out = lt_conn_take(c, (size_t)n);
    return (lt_err){ 0 };
}

static lt_err lt_conn_read_all(lt_handle *h, lt_bytes **out) {
    lt_conn *c = (lt_conn *)h;
    for (;;) {
        bool eof = false;
        lt_err e = lt_conn_fill(c, &eof);
        if (e.obj) return e;
        if (eof) break;
    }
    *out = lt_conn_take(c, c->blen);
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
    if (c->fd < 0) return lt_conn_error(c, "write", "it is closed");
    if (c->tls) {
        lt_err e = { 0 };
        c->tls_send(c, d, (size_t)n, &e);
        return e;
    }
    if (c->buffered) {
        if (c->wlen + (size_t)n <= LT_WBUF) {
            if (!c->wbuf) c->wbuf = (char *)malloc(LT_WBUF);
            memcpy(c->wbuf + c->wlen, d, (size_t)n);
            c->wlen += (size_t)n;
            return (lt_err){ 0 };
        }
        lt_err e = lt_conn_flush(c);
        if (e.obj) return e;
    }
    if (c->is_stdout) fflush(stdout);
    if (!lt_sock_write_all(c->fd, (const char *)d, (size_t)n)) return lt_conn_error(c, "write", strerror(errno));
    return (lt_err){ 0 };
}

static lt_err lt_conn_close(lt_handle *h) {
    lt_conn *c = (lt_conn *)h;
    if (c->fd < 0) return (lt_err){ 0 };
    lt_err e = lt_conn_flush(c);
    lt_conn_release(c);
    return e;
}

// ---- files

static lt_err lt_stream_open(lt_text *path, int flags, lt_handle **out) {
    int fd = open(path->data, flags | O_CLOEXEC, 0644);
    if (fd < 0) return lt_os_error(flags == O_RDONLY ? "can't open" : "can't create", path);
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return lt_os_error("can't open", path);
    }
    lt_conn *c = lt_conn_new(fd);
    c->buffered = flags != O_RDONLY;
    c->path = path;
    lt_text_dup(path);
    *out = &c->h;
    return (lt_err){ 0 };
}

// ---- standard streams

static lt_conn *lt_stdin_conn;

static lt_handle *lt_io_stdin(void) {
    lt_conn *c = __atomic_load_n(&lt_stdin_conn, __ATOMIC_ACQUIRE);
    if (!c) {
        // one for the whole program, so no input is lost between readers
        lt_conn *n = lt_conn_new(0);
        n->borrowed = true;
        n->label = "the standard input";
        lt_conn *expected = NULL;
        if (__atomic_compare_exchange_n(&lt_stdin_conn, &expected, n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            c = n;
        } else {
            lt_conn_free(&n->h);
            c = expected;
        }
    }
    lt_handle_dup(&c->h);
    return &c->h;
}

static lt_handle *lt_io_std(int fd) {
    lt_conn *c = lt_conn_new(fd);
    c->borrowed = true;
    c->is_stdout = fd == 1;
    c->label = fd == 1 ? "the standard output" : "the standard error";
    return &c->h;
}

static lt_err lt_io_read_line(lt_text **out) {
    lt_handle *h = lt_io_stdin();
    lt_err e = lt_conn_read_line(h, out);
    lt_handle_drop(h);
    return e;
}

static lt_err lt_io_read_all(lt_bytes **out) {
    lt_handle *h = lt_io_stdin();
    lt_err e = lt_conn_read_all(h, out);
    lt_handle_drop(h);
    return e;
}

// ---- copying

// Sends `count` bytes of a file from `offset` straight to a socket. Returns
// the number sent; fewer means the socket failed (errno).
static int64_t lt_sendfile(int file, int sock, int64_t offset, int64_t count) {
    int64_t sent = 0;
    while (sent < count) {
#if defined(__APPLE__)
        off_t len = (off_t)(count - sent);
        int r = sendfile(file, sock, (off_t)(offset + sent), &len, NULL, 0);
        sent += (int64_t)len;
        if (r == 0) {
            if (len == 0) break; // the file is shorter than it was
            continue;
        }
#else
        off_t off = (off_t)(offset + sent);
        size_t chunk = (size_t)(count - sent > (1 << 30) ? (1 << 30) : count - sent);
        ssize_t r = sendfile(sock, file, &off, chunk);
        if (r > 0) {
            sent += (int64_t)r;
            continue;
        }
        if (r == 0) break;
#endif
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            lt_io_wait(sock, true);
            continue;
        }
        if (errno == EINTR) continue;
        break;
    }
    return sent;
}

static bool lt_is_socket(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}

static bool lt_is_file(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

static lt_err lt_io_copy(lt_handle *fh, lt_handle *th, int64_t *copied) {
    lt_conn *from = (lt_conn *)fh, *to = (lt_conn *)th;
    int64_t total = 0;
    // what was already read ahead
    if (from->blen) {
        lt_err e = lt_conn_write_raw(th, from->buf, (int64_t)from->blen);
        if (e.obj) return e;
        total += (int64_t)from->blen;
        from->blen = 0;
    }
    if (from->fd >= 0 && to->fd >= 0 && !from->tls && !to->tls && lt_is_file(from->fd) && lt_is_socket(to->fd)) {
        lt_err e = lt_conn_flush(to);
        if (e.obj) return e;
        off_t pos = lseek(from->fd, 0, SEEK_CUR);
        struct stat st;
        fstat(from->fd, &st);
        int64_t want = (int64_t)st.st_size - (int64_t)pos;
        int64_t n = want > 0 ? lt_sendfile(from->fd, to->fd, (int64_t)pos, want) : 0;
        lseek(from->fd, pos + n, SEEK_SET);
        total += n;
        if (n < want) return lt_conn_error(to, "write", strerror(errno));
        *copied = total;
        return (lt_err){ 0 };
    }
    for (;;) {
        bool eof = false;
        lt_err e = lt_conn_fill(from, &eof);
        if (e.obj) return e;
        if (eof) break;
        e = lt_conn_write_raw(th, from->buf, (int64_t)from->blen);
        if (e.obj) return e;
        total += (int64_t)from->blen;
        from->blen = 0;
    }
    *copied = total;
    return (lt_err){ 0 };
}

// ---- running programs piece by piece

typedef struct lt_proc {
    lt_handle h;
    lt_conn *out; // its standard output
    lt_conn *in;  // its standard input
    pid_t pid;
    int64_t status;
    bool waited;
    lt_text *name;
} lt_proc;

static int64_t lt_exit_status(int st) {
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void lt_proc_reap(lt_proc *p, bool stop) {
    if (p->waited) return;
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    if (r == 0 && stop) {
        kill(p->pid, SIGTERM);
        r = -1;
    }
    if (r == 0 || r < 0) {
        while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR) {
        }
    }
    p->status = lt_exit_status(st);
    p->waited = true;
}

static void lt_proc_free(lt_handle *h) {
    lt_proc *p = (lt_proc *)h;
    lt_handle_drop(&p->in->h);
    lt_handle_drop(&p->out->h);
    lt_proc_reap(p, true);
    lt_text_drop(p->name);
    free(p);
}

static lt_err lt_process_start(lt_text *program, lt_texts *args, lt_handle **out) {
    int inp[2], outp[2];
    if (pipe(inp) != 0) return lt_os_error("can't run", program);
    if (pipe(outp) != 0) {
        close(inp[0]);
        close(inp[1]);
        return lt_os_error("can't run", program);
    }
    // our ends must not leak into other programs we start
    fcntl(inp[1], F_SETFD, FD_CLOEXEC);
    fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_addclose(&fa, inp[0]);
    posix_spawn_file_actions_addclose(&fa, outp[1]);
    char **argv = (char **)calloc((size_t)args->len + 2, sizeof(char *));
    argv[0] = program->data;
    for (int64_t i = 0; i < args->len; i++) argv[i + 1] = args->items[i]->data;
    pid_t pid;
    fflush(stdout);
    int r = posix_spawnp(&pid, program->data, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    free(argv);
    close(inp[0]);
    close(outp[1]);
    if (r != 0) {
        close(inp[1]);
        close(outp[0]);
        errno = r;
        return lt_os_error("can't run", program);
    }
#ifdef LT_THREADS
    lt_set_nonblocking(inp[1]);
    lt_set_nonblocking(outp[0]);
#endif
    signal(SIGPIPE, SIG_IGN);
    lt_proc *p = (lt_proc *)calloc(1, sizeof(lt_proc));
    p->h.rc = 1;
    p->h.free = lt_proc_free;
    p->pid = pid;
    p->name = program;
    lt_text_dup(program);
    p->in = lt_conn_new(inp[1]);
    p->in->path = program;
    lt_text_dup(program);
    p->out = lt_conn_new(outp[0]);
    p->out->path = program;
    lt_text_dup(program);
    *out = &p->h;
    return (lt_err){ 0 };
}

#define LT_PROC(h) ((lt_proc *)(h))

static lt_err lt_proc_close_input(lt_handle *h) { return lt_conn_close(&LT_PROC(h)->in->h); }

// Waits for the program to end. Its input is closed first (so it sees the
// end), and output it still writes is kept for reading afterwards.
static lt_err lt_proc_wait(lt_handle *h, int64_t *status) {
    lt_proc *p = LT_PROC(h);
    lt_conn_close(&p->in->h);
    if (p->out->fd >= 0) {
        for (;;) {
            bool eof = false;
            lt_err e = lt_conn_fill(p->out, &eof);
            if (e.obj) return e;
            if (eof) break;
        }
    }
#ifdef LT_THREADS
    // the output is closed, so it is ending: poll instead of blocking a worker
    for (int i = 0; !p->waited && i < 200; i++) {
        int st = 0;
        if (waitpid(p->pid, &st, WNOHANG) == p->pid) {
            p->status = lt_exit_status(st);
            p->waited = true;
            break;
        }
        lt_err se = lt_sleep_nanos(i < 20 ? 1000000 : 10000000);
        if (se.obj) {
            lt_iface_drop(se);
            break;
        }
    }
#endif
    lt_proc_reap(p, false);
    *status = p->status;
    return (lt_err){ 0 };
}

// Leaving `with`: stop the program if it is still running.
static lt_err lt_proc_close(lt_handle *h) {
    lt_proc *p = LT_PROC(h);
    lt_conn_close(&p->in->h);
    lt_conn_close(&p->out->h);
    lt_proc_reap(p, true);
    return (lt_err){ 0 };
}
