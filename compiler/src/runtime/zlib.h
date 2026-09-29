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
