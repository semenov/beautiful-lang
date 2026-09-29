// zlib: gzip / deflate.

#include <zlib.h>

static lt_bytes *lt_zlib_compress(lt_bytes *in, bool gzip) {
    z_stream s;
    memset(&s, 0, sizeof s);
    deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, gzip ? 15 + 16 : 15, 8, Z_DEFAULT_STRATEGY);
    uLong bound = deflateBound(&s, (uLong)in->len) + 32;
    lt_bytes *out = lt_bytes_new((int64_t)bound);
    s.next_in = in->data;
    s.avail_in = (uInt)in->len;
    s.next_out = out->data;
    s.avail_out = (uInt)bound;
    deflate(&s, Z_FINISH);
    out->len = (int64_t)s.total_out;
    deflateEnd(&s);
    return out;
}

static lt_err lt_zlib_decompress(lt_bytes *in, bool gzip_only, lt_bytes **outp) {
    z_stream s;
    memset(&s, 0, sizeof s);
    inflateInit2(&s, gzip_only ? 15 + 16 : 15 + 32);
    int64_t cap = in->len * 4 + 64;
    lt_bytes *out = lt_bytes_new(cap);
    s.next_in = in->data;
    s.avail_in = (uInt)in->len;
    int r;
    for (;;) {
        if (out->len == out->cap) {
            lt_bytes_reserve(&out, out->cap);
        }
        s.next_out = out->data + out->len;
        s.avail_out = (uInt)(out->cap - out->len);
        r = inflate(&s, Z_NO_FLUSH);
        out->len = (int64_t)s.total_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (r == Z_BUF_ERROR && s.avail_in == 0) break;
    }
    inflateEnd(&s);
    if (r != Z_STREAM_END) {
        lt_bytes_drop(out);
        char buf[128];
        snprintf(buf, sizeof buf, "zlib: the data is not valid %s", gzip_only ? "gzip" : "compressed data");
        return lt_make_failure(lt_text_cstr(buf));
    }
    *outp = out;
    return (lt_err){ 0 };
}

// ---- gzip files as streams

typedef struct {
    z_stream z;
    bool writing, done;
    unsigned char buf[65536];
} lt_gz;

static ssize_t lt_gz_recv(lt_conn *c, void *d, size_t n, lt_err *err) {
    lt_gz *g = (lt_gz *)c->tls;
    if (g->done) return 0;
    g->z.next_out = (unsigned char *)d;
    g->z.avail_out = (uInt)n;
    while (g->z.avail_out == n) {
        if (g->z.avail_in == 0) {
            ssize_t r = read(c->fd, g->buf, sizeof g->buf);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) {
                *err = lt_conn_error(c, "read", strerror(errno));
                return 0;
            }
            if (r == 0) {
                *err = lt_conn_error(c, "read", "the gzip data ends too early");
                return 0;
            }
            g->z.next_in = g->buf;
            g->z.avail_in = (uInt)r;
        }
        int rc = inflate(&g->z, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            // another gzip member may follow (concatenated .gz files)
            if (g->z.avail_in == 0) {
                ssize_t r;
                while ((r = read(c->fd, g->buf, sizeof g->buf)) < 0 && errno == EINTR) {
                }
                if (r <= 0) {
                    g->done = true;
                    break;
                }
                g->z.next_in = g->buf;
                g->z.avail_in = (uInt)r;
            }
            inflateReset(&g->z);
        } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
            *err = lt_conn_error(c, "read", "the data is not valid gzip");
            return 0;
        }
    }
    return (ssize_t)(n - g->z.avail_out);
}

static bool lt_gz_deflate(lt_conn *c, int flush, lt_err *err) {
    lt_gz *g = (lt_gz *)c->tls;
    int rc;
    do {
        g->z.next_out = g->buf;
        g->z.avail_out = sizeof g->buf;
        rc = deflate(&g->z, flush);
        size_t have = sizeof g->buf - g->z.avail_out;
        if (have && !lt_sock_write_all(c->fd, (const char *)g->buf, have)) {
            *err = lt_conn_error(c, "write", strerror(errno));
            return false;
        }
    } while (g->z.avail_out == 0 || (flush == Z_FINISH && rc != Z_STREAM_END));
    return true;
}

static bool lt_gz_send(lt_conn *c, const void *d, size_t n, lt_err *err) {
    lt_gz *g = (lt_gz *)c->tls;
    g->z.next_in = (unsigned char *)d;
    g->z.avail_in = (uInt)n;
    return lt_gz_deflate(c, Z_NO_FLUSH, err);
}

static lt_err lt_gz_finish(lt_conn *c) {
    lt_err e = { 0 };
    lt_gz *g = (lt_gz *)c->tls;
    if (g && g->writing && !g->done) {
        g->done = true;
        lt_gz_deflate(c, Z_FINISH, &e);
    }
    return e;
}

static void lt_gz_free(lt_conn *c) {
    lt_gz *g = (lt_gz *)c->tls;
    c->tls = NULL;
    if (g->writing) deflateEnd(&g->z);
    else inflateEnd(&g->z);
    free(g);
}

static lt_err lt_gzip_open(lt_text *path, bool writing, lt_handle **out) {
    lt_err e = lt_stream_open(path, writing ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, out);
    if (e.obj) return e;
    lt_conn *c = (lt_conn *)*out;
    lt_gz *g = (lt_gz *)calloc(1, sizeof(lt_gz));
    g->writing = writing;
    if (writing) deflateInit2(&g->z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    else inflateInit2(&g->z, 15 + 32);
    c->tls = g;
    c->buffered = false;
    c->tls_recv = lt_gz_recv;
    c->tls_send = lt_gz_send;
    c->tls_free = lt_gz_free;
    c->finish = lt_gz_finish;
    return (lt_err){ 0 };
}

// ---- for zip files: raw deflate (no zlib or gzip wrapper) and CRC-32

static lt_bytes *lt_deflate_raw(lt_bytes *in) {
    z_stream s;
    memset(&s, 0, sizeof s);
    deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    uLong bound = deflateBound(&s, (uLong)in->len) + 32;
    lt_bytes *out = lt_bytes_new((int64_t)bound);
    s.next_in = in->data;
    s.avail_in = (uInt)in->len;
    s.next_out = out->data;
    s.avail_out = (uInt)bound;
    deflate(&s, Z_FINISH);
    out->len = (int64_t)s.total_out;
    deflateEnd(&s);
    return out;
}

static lt_err lt_inflate_raw(lt_bytes *in, int64_t size, lt_bytes **outp) {
    z_stream s;
    memset(&s, 0, sizeof s);
    inflateInit2(&s, -15);
    lt_bytes *out = lt_bytes_new(size > 0 ? size : 64);
    s.next_in = in->data;
    s.avail_in = (uInt)in->len;
    s.next_out = out->data;
    s.avail_out = (uInt)out->cap;
    int r = inflate(&s, Z_FINISH);
    out->len = (int64_t)s.total_out;
    inflateEnd(&s);
    if (r != Z_STREAM_END || out->len != size) {
        lt_bytes_drop(out);
        return lt_make_failure(lt_text_cstr("archive: a zip entry's data is damaged"));
    }
    *outp = out;
    return (lt_err){ 0 };
}

static int64_t lt_crc32(lt_bytes *d) { return (int64_t)crc32(0L, d->data, (uInt)d->len); }
