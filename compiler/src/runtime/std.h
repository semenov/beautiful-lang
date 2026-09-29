// The standard library's system side: files, processes, environment, log,
// random numbers, wall-clock time.

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

static int lt_argc;
static char **lt_argv;

// an error "<what> \"<path>\": <reason from errno>"
static lt_err lt_os_error(const char *what, lt_text *path) {
    char buf[1024];
    int e = errno;
    snprintf(buf, sizeof buf, "%s \"%.*s\": %s", what, (int)(path ? path->len : 0), path ? path->data : "", strerror(e));
    return lt_make_failure(lt_text_cstr(buf));
}

// ---------------------------------------------------------------- handles
// Values of the standard library's built-in types (files, directories,
// connections) are reference-counted handles with their own `free`.

typedef struct lt_handle {
    int64_t rc;
    void (*free)(struct lt_handle *);
} lt_handle;

LT_INLINE void lt_handle_dup(lt_handle *h) {
    if (h) LT_INC(h);
}
LT_INLINE void lt_handle_drop(lt_handle *h) {
    if (h && LT_DEC_ZERO(h)) h->free(h);
}

// ---------------------------------------------------------------- files

static lt_err lt_files_read(lt_text *path, lt_text **out) {
    FILE *f = fopen(path->data, "rb");
    if (!f) return lt_os_error("can't read", path);
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0) {
            fseek(f, 0, SEEK_SET);
            lt_text *t = lt_text_new(n);
            size_t got = fread(t->data, 1, (size_t)n, f);
            t->len = (int64_t)got;
            t->data[got] = 0;
            fclose(f);
            *out = t;
            return (lt_err){ 0 };
        }
    }
    // not seekable (a pipe): read in chunks
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) buf = (char *)realloc(buf, cap *= 2);
    }
    fclose(f);
    *out = lt_text_from(buf, (int64_t)len);
    free(buf);
    return (lt_err){ 0 };
}

static lt_err lt_files_write_mode(lt_text *path, lt_text *text, const char *mode) {
    FILE *f = fopen(path->data, mode);
    if (!f) return lt_os_error("can't write", path);
    if (fwrite(text->data, 1, (size_t)text->len, f) != (size_t)text->len) {
        lt_err e = lt_os_error("can't write", path);
        fclose(f);
        return e;
    }
    if (fclose(f) != 0) return lt_os_error("can't write", path);
    return (lt_err){ 0 };
}

static bool lt_files_exists(lt_text *path) {
    struct stat st;
    return stat(path->data, &st) == 0;
}
static bool lt_files_is_dir(lt_text *path) {
    struct stat st;
    return stat(path->data, &st) == 0 && S_ISDIR(st.st_mode);
}

static int lt_cmp_texts(const void *a, const void *b) { return (int)lt_text_cmp(*(lt_text **)a, *(lt_text **)b); }

static lt_err lt_files_list(lt_text *dir, lt_texts **out) {
    DIR *d = opendir(dir->data);
    if (!d) return lt_os_error("can't list", dir);
    lt_texts *l = lt_texts_new(8);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        lt_texts_push(&l, lt_text_cstr(e->d_name));
    }
    closedir(d);
    qsort(l->items, (size_t)l->len, sizeof(lt_text *), lt_cmp_texts);
    *out = l;
    return (lt_err){ 0 };
}

static lt_err lt_files_delete(lt_text *path) {
    if (remove(path->data) != 0) return lt_os_error("can't delete", path);
    return (lt_err){ 0 };
}

static lt_err lt_files_make_dir(lt_text *path) {
    char buf[4096];
    if (path->len >= (int64_t)sizeof buf) {
        errno = ENAMETOOLONG;
        return lt_os_error("can't create the directory", path);
    }
    memcpy(buf, path->data, (size_t)path->len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return lt_os_error("can't create the directory", path);
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return lt_os_error("can't create the directory", path);
    return (lt_err){ 0 };
}

static lt_err lt_files_copy(lt_text *from, lt_text *to) {
    lt_text *data = NULL;
    lt_err e = lt_files_read(from, &data);
    if (e.obj) return e;
    e = lt_files_write_mode(to, data, "wb");
    lt_text_drop(data);
    return e;
}

static lt_err lt_files_rename(lt_text *from, lt_text *to) {
    if (rename(from->data, to->data) != 0) return lt_os_error("can't rename", from);
    return (lt_err){ 0 };
}

static lt_text *lt_files_join(lt_text *dir, lt_text *name) {
    if (dir->len == 0) return lt_text_ret(name);
    if (name->len > 0 && name->data[0] == '/') return lt_text_ret(name);
    bool slash = dir->data[dir->len - 1] == '/';
    lt_text *r = lt_text_new(dir->len + name->len + (slash ? 0 : 1));
    memcpy(r->data, dir->data, (size_t)dir->len);
    if (!slash) r->data[dir->len] = '/';
    memcpy(r->data + dir->len + (slash ? 0 : 1), name->data, (size_t)name->len);
    return r;
}

static int64_t lt_last_slash(lt_text *p) {
    int64_t end = p->len;
    while (end > 1 && p->data[end - 1] == '/') end--;
    for (int64_t i = end - 1; i >= 0; i--)
        if (p->data[i] == '/') return i;
    return -1;
}

static lt_text *lt_files_name(lt_text *p) {
    int64_t end = p->len;
    while (end > 1 && p->data[end - 1] == '/') end--;
    int64_t s = lt_last_slash(p);
    return lt_text_from(p->data + s + 1, end - s - 1);
}

static lt_text *lt_files_parent(lt_text *p) {
    int64_t s = lt_last_slash(p);
    if (s < 0) return lt_text_cstr(".");
    if (s == 0) return lt_text_cstr("/");
    return lt_text_from(p->data, s);
}

static lt_text *lt_files_extension(lt_text *p) {
    lt_text *n = lt_files_name(p);
    lt_text *r = NULL;
    for (int64_t i = n->len - 1; i > 0; i--) {
        if (n->data[i] == '.') {
            r = lt_text_from(n->data + i + 1, n->len - i - 1);
            break;
        }
    }
    lt_text_drop(n);
    return r;
}

typedef struct lt_openfile {
    lt_handle h;
    FILE *f;
    lt_text *path;
} lt_openfile;

static void lt_file_free(lt_handle *h) {
    lt_openfile *f = (lt_openfile *)h;
    if (f->f) fclose(f->f);
    lt_text_drop(f->path);
    free(f);
}

static lt_err lt_files_open_mode(lt_text *path, const char *mode, lt_handle **out) {
    FILE *fp = fopen(path->data, mode);
    if (!fp) return lt_os_error(mode[0] == 'r' ? "can't open" : "can't create", path);
    lt_openfile *f = (lt_openfile *)calloc(1, sizeof(lt_openfile));
    f->h.rc = 1;
    f->h.free = lt_file_free;
    f->f = fp;
    f->path = path;
    lt_text_dup(path);
    *out = &f->h;
    return (lt_err){ 0 };
}

static lt_err lt_file_read_line(lt_handle *h, lt_text **out) {
    lt_openfile *f = (lt_openfile *)h;
    if (!f->f) {
        errno = EBADF;
        return lt_os_error("can't read the closed file", f->path);
    }
    size_t cap = 128, len = 0;
    char *buf = (char *)malloc(cap);
    int c;
    bool any = false;
    while ((c = getc_unlocked(f->f)) != EOF) {
        any = true;
        if (c == '\n') break;
        if (len + 1 >= cap) buf = (char *)realloc(buf, cap *= 2);
        buf[len++] = (char)c;
    }
    if (ferror(f->f)) {
        free(buf);
        return lt_os_error("can't read", f->path);
    }
    if (!any) {
        free(buf);
        *out = NULL;
        return (lt_err){ 0 };
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    *out = lt_text_from(buf, (int64_t)len);
    free(buf);
    return (lt_err){ 0 };
}

static lt_err lt_file_write(lt_handle *h, lt_text *text) {
    lt_openfile *f = (lt_openfile *)h;
    if (!f->f || fwrite(text->data, 1, (size_t)text->len, f->f) != (size_t)text->len) return lt_os_error("can't write", f->path);
    return (lt_err){ 0 };
}

static lt_err lt_file_close(lt_handle *h) {
    lt_openfile *f = (lt_openfile *)h;
    if (!f->f) return (lt_err){ 0 };
    int r = fclose(f->f);
    f->f = NULL;
    if (r != 0) return lt_os_error("can't finish writing", f->path);
    return (lt_err){ 0 };
}

typedef struct lt_tempdir {
    lt_handle h;
    lt_text *path;
    bool closed;
} lt_tempdir;

static void lt_delete_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char sub[4096];
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                lt_delete_tree(sub);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void lt_tempdir_free(lt_handle *h) {
    lt_tempdir *t = (lt_tempdir *)h;
    if (!t->closed) lt_delete_tree(t->path->data);
    lt_text_drop(t->path);
    free(t);
}

static lt_err lt_files_temp_dir(lt_handle **out) {
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s%sprogram-XXXXXX", base, base[strlen(base) - 1] == '/' ? "" : "/");
    if (!mkdtemp(tmpl)) return lt_os_error("can't create a temporary directory in", lt_text_cstr(base));
    lt_tempdir *t = (lt_tempdir *)calloc(1, sizeof(lt_tempdir));
    t->h.rc = 1;
    t->h.free = lt_tempdir_free;
    t->path = lt_text_cstr(tmpl);
    *out = &t->h;
    return (lt_err){ 0 };
}

static lt_err lt_tempdir_close(lt_handle *h) {
    lt_tempdir *t = (lt_tempdir *)h;
    if (!t->closed) {
        lt_delete_tree(t->path->data);
        t->closed = true;
    }
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- processes

static lt_texts *lt_process_args(void) {
    lt_texts *l = lt_texts_new(lt_argc);
    for (int i = 1; i < lt_argc; i++) lt_texts_push(&l, lt_text_cstr(lt_argv[i]));
    return l;
}

static _Noreturn void lt_process_exit(int64_t status) {
    fflush(stdout);
    fflush(stderr);
    exit((int)status);
}

static lt_err lt_process_run(lt_text *program, lt_texts *args, int64_t *status, lt_text **out, lt_text **err) {
    int outp[2], errp[2];
    if (pipe(outp) != 0 || pipe(errp) != 0) return lt_os_error("can't run", program);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errp[1], 2);
    posix_spawn_file_actions_addclose(&fa, outp[0]);
    posix_spawn_file_actions_addclose(&fa, errp[0]);
    char **argv = (char **)calloc((size_t)args->len + 2, sizeof(char *));
    argv[0] = program->data;
    for (int64_t i = 0; i < args->len; i++) argv[i + 1] = args->items[i]->data;
    pid_t pid;
    fflush(stdout);
    int r = posix_spawnp(&pid, program->data, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    free(argv);
    close(outp[1]);
    close(errp[1]);
    if (r != 0) {
        close(outp[0]);
        close(errp[0]);
        errno = r;
        return lt_os_error("can't run", program);
    }
    // read both pipes until both are closed
    size_t cap[2] = { 4096, 1024 }, len[2] = { 0, 0 };
    char *buf[2] = { (char *)malloc(cap[0]), (char *)malloc(cap[1]) };
    struct pollfd fds[2] = { { outp[0], POLLIN, 0 }, { errp[0], POLLIN, 0 } };
    int open_fds = 2;
    while (open_fds > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (len[i] == cap[i]) buf[i] = (char *)realloc(buf[i], cap[i] *= 2);
            ssize_t n = read(fds[i].fd, buf[i] + len[i], cap[i] - len[i]);
            if (n <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                open_fds--;
            } else {
                len[i] += (size_t)n;
            }
        }
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    *out = lt_text_from(buf[0], (int64_t)len[0]);
    *err = lt_text_from(buf[1], (int64_t)len[1]);
    free(buf[0]);
    free(buf[1]);
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- environment

static lt_text *lt_env_get(lt_text *name) {
    const char *v = getenv(name->data);
    return v ? lt_text_cstr(v) : NULL;
}

// ---------------------------------------------------------------- time and log

static int64_t lt_unix_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

static void lt_log(const char *level, lt_text *msg) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
    fflush(stdout);
    flockfile(stderr);
    fprintf(stderr, "%s %s %.*s\n", stamp, level, (int)msg->len, msg->data);
    funlockfile(stderr);
}

// ---------------------------------------------------------------- random

static uint64_t lt_random_u64(void) {
    uint64_t v;
#if defined(__APPLE__)
    arc4random_buf(&v, sizeof v);
#else
    static __thread uint64_t s;
    if (!s) {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) {
            fread(&s, sizeof s, 1, f);
            fclose(f);
        }
        s |= 1;
    }
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    v = s;
#endif
    return v;
}

static int64_t lt_random_between(int64_t low, int64_t high, int line) {
    if (high < low) lt_panic_at("random.between: `high` is below `low`", line);
    uint64_t span = (uint64_t)(high - low) + 1;
    if (span == 0) return (int64_t)lt_random_u64();
    uint64_t limit = UINT64_MAX - UINT64_MAX % span;
    uint64_t r;
    do r = lt_random_u64();
    while (r >= limit);
    return low + (int64_t)(r % span);
}

static double lt_random_fraction(void) { return (double)(lt_random_u64() >> 11) / 9007199254740992.0; }

static lt_text *lt_random_token(int64_t n) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (n < 0) n = 0;
    lt_text *t = lt_text_new(n);
    for (int64_t i = 0; i < n; i++) t->data[i] = chars[lt_random_between(0, 61, 0)];
    return t;
}

// ---------------------------------------------------------------- crypto

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} lt_sha256;

static const uint32_t lt_k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define LT_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void lt_sha256_block(lt_sha256 *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = LT_ROR(w[i - 15], 7) ^ LT_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = LT_ROR(w[i - 2], 17) ^ LT_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (LT_ROR(e, 6) ^ LT_ROR(e, 11) ^ LT_ROR(e, 25)) + ((e & f) ^ (~e & g)) + lt_k256[i] + w[i];
        uint32_t t2 = (LT_ROR(a, 2) ^ LT_ROR(a, 13) ^ LT_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void lt_sha256_init(lt_sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->n = 0;
}
static void lt_sha256_update(lt_sha256 *s, const unsigned char *p, size_t n) {
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n == 64) {
            lt_sha256_block(s, s->buf);
            s->n = 0;
        }
    }
}
static void lt_sha256_final(lt_sha256 *s, unsigned char out[32]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    lt_sha256_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->n != 56) lt_sha256_update(s, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    lt_sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(s->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(s->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(s->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)s->h[i];
    }
}

static lt_bytes *lt_crypto_sha256(lt_bytes *d) {
    lt_sha256 s;
    lt_sha256_init(&s);
    lt_sha256_update(&s, d->data, (size_t)d->len);
    lt_bytes *r = lt_bytes_new(32);
    lt_sha256_final(&s, r->data);
    r->len = 32;
    return r;
}

static void lt_hmac_raw(const unsigned char *key, size_t kl, const unsigned char *msg, size_t ml, unsigned char out[32]) {
    unsigned char k[64] = { 0 };
    if (kl > 64) {
        lt_sha256 s;
        lt_sha256_init(&s);
        lt_sha256_update(&s, key, kl);
        lt_sha256_final(&s, k);
    } else {
        memcpy(k, key, kl);
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    unsigned char inner[32];
    lt_sha256 s;
    lt_sha256_init(&s);
    lt_sha256_update(&s, ipad, 64);
    lt_sha256_update(&s, msg, ml);
    lt_sha256_final(&s, inner);
    lt_sha256_init(&s);
    lt_sha256_update(&s, opad, 64);
    lt_sha256_update(&s, inner, 32);
    lt_sha256_final(&s, out);
}

static lt_bytes *lt_crypto_hmac(lt_bytes *key, lt_bytes *msg) {
    lt_bytes *r = lt_bytes_new(32);
    lt_hmac_raw(key->data, (size_t)key->len, msg->data, (size_t)msg->len, r->data);
    r->len = 32;
    return r;
}

static lt_bytes *lt_crypto_pbkdf2(lt_bytes *pw, lt_bytes *salt, int64_t iterations, int64_t length, int line) {
    if (iterations < 1 || length < 1 || length > 1024) lt_panic_at("pbkdf2: iterations must be at least 1 and length 1 to 1024", line);
    lt_bytes *out = lt_bytes_new(length);
    unsigned char *msg = (unsigned char *)malloc((size_t)salt->len + 4);
    memcpy(msg, salt->data, (size_t)salt->len);
    for (uint32_t block = 1; out->len < length; block++) {
        msg[salt->len] = (unsigned char)(block >> 24);
        msg[salt->len + 1] = (unsigned char)(block >> 16);
        msg[salt->len + 2] = (unsigned char)(block >> 8);
        msg[salt->len + 3] = (unsigned char)block;
        unsigned char u[32], t[32];
        lt_hmac_raw(pw->data, (size_t)pw->len, msg, (size_t)salt->len + 4, u);
        memcpy(t, u, 32);
        for (int64_t i = 1; i < iterations; i++) {
            lt_hmac_raw(pw->data, (size_t)pw->len, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        int64_t take = length - out->len < 32 ? length - out->len : 32;
        memcpy(out->data + out->len, t, (size_t)take);
        out->len += take;
    }
    free(msg);
    return out;
}

static bool lt_crypto_equal(lt_bytes *a, lt_bytes *b) {
    if (a->len != b->len) return false;
    unsigned char d = 0;
    for (int64_t i = 0; i < a->len; i++) d |= a->data[i] ^ b->data[i];
    return d == 0;
}

static lt_bytes *lt_crypto_random_bytes(int64_t n, int line) {
    if (n < 0) lt_panic_at("random_bytes: the count is negative", line);
    lt_bytes *b = lt_bytes_new(n);
    for (int64_t i = 0; i < n; i += 8) {
        uint64_t r = lt_random_u64();
        int64_t take = n - i < 8 ? n - i : 8;
        memcpy(b->data + i, &r, (size_t)take);
    }
    b->len = n;
    return b;
}

static lt_text *lt_random_uuid(void) {
    unsigned char u[16];
    uint64_t a = lt_random_u64(), b = lt_random_u64();
    memcpy(u, &a, 8);
    memcpy(u + 8, &b, 8);
    u[6] = (unsigned char)((u[6] & 0x0f) | 0x40);
    u[8] = (unsigned char)((u[8] & 0x3f) | 0x80);
    char s[37];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return lt_text_from(s, 36);
}

static lt_err lt_files_read_bytes(lt_text *path, lt_bytes **out) {
    lt_text *t = NULL;
    lt_err e = lt_files_read(path, &t);
    if (e.obj) return e;
    *out = lt_bytes_from(t->data, t->len);
    lt_text_drop(t);
    return (lt_err){ 0 };
}
static lt_err lt_files_write_bytes(lt_text *path, lt_bytes *b) {
    FILE *f = fopen(path->data, "wb");
    if (!f) return lt_os_error("can't write", path);
    size_t n = fwrite(b->data, 1, (size_t)b->len, f);
    if (fclose(f) != 0 || n != (size_t)b->len) return lt_os_error("can't write", path);
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- regular expressions

#include <regex.h>

typedef struct {
    lt_handle h;
    regex_t re;
    int ngroups;
} lt_regex;

static void lt_regex_free(lt_handle *h) {
    lt_regex *r = (lt_regex *)h;
    regfree(&r->re);
    free(r);
}

// translates \d \w \s (and capitals) to POSIX classes; rejects what POSIX
// regular expressions can't do instead of silently doing something else
static lt_err lt_regex_translate(lt_text *p, char **out, int *icase) {
    size_t cap = (size_t)p->len * 12 + 16;
    char *o = (char *)malloc(cap), *w = o;
    const char *s = p->data, *e = p->data + p->len;
    *icase = 0;
    if (e - s >= 4 && memcmp(s, "(?i)", 4) == 0) {
        *icase = 1;
        s += 4;
    }
    bool in_class = false;
    for (; s < e; s++) {
        char c = *s;
        if (c == '\\' && s + 1 < e) {
            char n = *++s;
            const char *rep = NULL;
            switch (n) {
            case 'd': rep = in_class ? "0-9" : "[0-9]"; break;
            case 'D': rep = in_class ? NULL : "[^0-9]"; break;
            case 'w': rep = in_class ? "[:alnum:]_" : "[[:alnum:]_]"; break;
            case 'W': rep = in_class ? NULL : "[^[:alnum:]_]"; break;
            case 's': rep = in_class ? "[:space:]" : "[[:space:]]"; break;
            case 'S': rep = in_class ? NULL : "[^[:space:]]"; break;
            case 'n': rep = "\n"; break;
            case 't': rep = "\t"; break;
            case 'b': case 'B': case 'A': case 'z': case 'Z':
                free(o);
                return lt_make_failure(lt_text_cstr("regex: \\b and other anchors except ^ and $ aren't supported"));
            default:
                if (in_class) {
                    *w++ = n;
                } else {
                    *w++ = '\\';
                    *w++ = n;
                }
                continue;
            }
            if (!rep) {
                free(o);
                return lt_make_failure(lt_text_cstr("regex: \\D, \\W and \\S can't be used inside [...]"));
            }
            size_t l = strlen(rep);
            memcpy(w, rep, l);
            w += l;
            continue;
        }
        if (!in_class && (c == '*' || c == '+' || c == '?' || c == '}') && s + 1 < e && s[1] == '?') {
            free(o);
            return lt_make_failure(lt_text_cstr("regex: lazy quantifiers like *? aren't supported"));
        }
        if (!in_class && c == '(' && s + 1 < e && s[1] == '?') {
            free(o);
            return lt_make_failure(lt_text_cstr("regex: (?...) groups aren't supported, except (?i) at the start"));
        }
        if (c == '[' && !in_class) {
            in_class = true;
            *w++ = c;
            if (s + 1 < e && s[1] == '^') *w++ = *++s;
            if (s + 1 < e && s[1] == ']') *w++ = *++s;
            continue;
        }
        if (c == '[' && in_class && s + 1 < e && s[1] == ':') {
            const char *end = strstr(s, ":]");
            if (end) {
                memcpy(w, s, (size_t)(end - s) + 2);
                w += end - s + 2;
                s = end + 1;
                continue;
            }
        }
        if (c == ']' && in_class) in_class = false;
        *w++ = c;
    }
    *w = 0;
    *out = o;
    return (lt_err){ 0 };
}

static lt_err lt_regex_compile(lt_text *pat, lt_handle **out) {
    char *tr;
    int icase;
    lt_err e = lt_regex_translate(pat, &tr, &icase);
    if (e.obj) return e;
    lt_regex *r = (lt_regex *)calloc(1, sizeof(lt_regex));
    r->h.rc = 1;
    r->h.free = lt_regex_free;
    int rc = regcomp(&r->re, tr, REG_EXTENDED | (icase ? REG_ICASE : 0));
    free(tr);
    if (rc != 0) {
        // our own words: the system's messages differ between macOS and Linux
        const char *msg;
        switch (rc) {
        case REG_EPAREN: msg = "parentheses not balanced"; break;
        case REG_EBRACK: msg = "brackets not balanced"; break;
        case REG_EBRACE: msg = "braces not balanced"; break;
        case REG_BADBR: msg = "invalid repetition count in braces"; break;
        case REG_ERANGE: msg = "invalid range in brackets"; break;
        case REG_ECTYPE: msg = "unknown character class"; break;
        case REG_EESCAPE: msg = "a trailing backslash"; break;
        case REG_ESUBREG: msg = "a back reference to a missing group"; break;
        case REG_BADRPT: msg = "a repetition with nothing to repeat"; break;
        default: msg = "invalid regular expression"; break;
        }
        char buf[512];
        snprintf(buf, sizeof buf, "regex: %s in \"%.*s\"", msg, (int)pat->len, pat->data);
        free(r);
        return lt_make_failure(lt_text_cstr(buf));
    }
    r->ngroups = (int)r->re.re_nsub;
    *out = &r->h;
    return (lt_err){ 0 };
}

#define LT_RE_GROUPS 10

// a match at or after byte `from`; pm offsets are absolute
static bool lt_regex_at(lt_handle *h, lt_text *t, int64_t from, regmatch_t *pm) {
    lt_regex *r = (lt_regex *)h;
#ifdef REG_STARTEND
    pm[0].rm_so = (regoff_t)from;
    pm[0].rm_eo = (regoff_t)t->len;
    int flags = REG_STARTEND | (from > 0 ? REG_NOTBOL : 0);
    return regexec(&r->re, t->data, LT_RE_GROUPS, pm, flags) == 0;
#else
    // musl: match from the offset (texts end with a zero byte) and shift back
    if (regexec(&r->re, t->data + from, LT_RE_GROUPS, pm, from > 0 ? REG_NOTBOL : 0) != 0) return false;
    for (int i = 0; i < LT_RE_GROUPS; i++) {
        if (pm[i].rm_so >= 0) {
            pm[i].rm_so += (regoff_t)from;
            pm[i].rm_eo += (regoff_t)from;
        }
    }
    return true;
#endif
}

static int64_t lt_char_index(lt_text *t, int64_t byte) {
    int64_t n = 0;
    for (int64_t i = 0; i < byte && i < t->len; i++)
        if (((unsigned char)t->data[i] & 0xC0) != 0x80) n++;
    return n;
}

static bool lt_regex_matches(lt_handle *h, lt_text *t) {
    regmatch_t pm[LT_RE_GROUPS];
    return lt_regex_at(h, t, 0, pm) && pm[0].rm_so == 0 && pm[0].rm_eo == t->len;
}

static lt_text *lt_regex_replace(lt_handle *h, lt_text *t, lt_text *with) {
    lt_regex *r = (lt_regex *)h;
    lt_texts *parts = lt_texts_new(8);
    regmatch_t pm[LT_RE_GROUPS];
    int64_t pos = 0;
    while (pos <= t->len && lt_regex_at(h, t, pos, pm)) {
        lt_texts_push(&parts, lt_text_from(t->data + pos, pm[0].rm_so - pos));
        for (int64_t i = 0; i < with->len; i++) {
            char c = with->data[i];
            if (c == '$' && i + 1 < with->len && with->data[i + 1] >= '0' && with->data[i + 1] <= '9') {
                int g = with->data[++i] - '0';
                if (g <= r->ngroups && pm[g].rm_so >= 0) lt_texts_push(&parts, lt_text_from(t->data + pm[g].rm_so, pm[g].rm_eo - pm[g].rm_so));
            } else {
                int64_t j = i;
                while (j < with->len && !(with->data[j] == '$' && j + 1 < with->len && with->data[j + 1] >= '0' && with->data[j + 1] <= '9')) j++;
                lt_texts_push(&parts, lt_text_from(with->data + i, j - i));
                i = j - 1;
            }
        }
        if (pm[0].rm_eo == pm[0].rm_so) {
            // an empty match: keep one character and move on
            if (pm[0].rm_eo < t->len) lt_texts_push(&parts, lt_text_from(t->data + pm[0].rm_eo, 1));
            pos = pm[0].rm_eo + 1;
        } else {
            pos = pm[0].rm_eo;
        }
    }
    if (pos < t->len) lt_texts_push(&parts, lt_text_from(t->data + pos, t->len - pos));
    lt_text *res = lt_text_concat_n((int)parts->len, parts->items);
    for (int64_t i = 0; i < parts->len; i++) lt_text_drop(parts->items[i]);
    lt_free(parts, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)parts->cap);
    return res;
}

static lt_texts *lt_regex_split(lt_handle *h, lt_text *t) {
    lt_texts *parts = lt_texts_new(8);
    regmatch_t pm[LT_RE_GROUPS];
    int64_t pos = 0, piece = 0;
    while (pos <= t->len && lt_regex_at(h, t, pos, pm)) {
        if (pm[0].rm_eo == pm[0].rm_so) {
            pos = pm[0].rm_eo + 1;
            continue;
        }
        lt_texts_push(&parts, lt_text_from(t->data + piece, pm[0].rm_so - piece));
        piece = pos = pm[0].rm_eo;
    }
    lt_texts_push(&parts, lt_text_from(t->data + piece, t->len - piece));
    return parts;
}

// ---------------------------------------------------------------- numbers as text

static lt_text *lt_float_format(double v, int64_t decimals, int line) {
    if (decimals < 0 || decimals > 20) lt_panic_at("format: decimals must be 0 to 20", line);
    char buf[400];
    int n = snprintf(buf, sizeof buf, "%.*f", (int)decimals, v);
    if (n < 0 || n >= (int)sizeof buf) return lt_float_to_text(v);
    return lt_text_from(buf, n);
}

// ---------------------------------------------------------------- url

static lt_text *lt_url_encode(lt_text *t) {
    static const char hx[] = "0123456789ABCDEF";
    lt_text *r = lt_text_new(t->len * 3);
    char *w = r->data;
    for (int64_t i = 0; i < t->len; i++) {
        unsigned char c = (unsigned char)t->data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *w++ = (char)c;
        } else {
            *w++ = '%';
            *w++ = hx[c >> 4];
            *w++ = hx[c & 15];
        }
    }
    r->len = w - r->data;
    *w = 0;
    return r;
}

static lt_err lt_url_decode_text(lt_text *t, lt_text **out) {
    lt_text *r = lt_text_new(t->len);
    int64_t w = 0;
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        if (c == '%') {
            int h = i + 2 < t->len ? lt_hexval(t->data[i + 1]) : -1, l = i + 2 < t->len ? lt_hexval(t->data[i + 2]) : -1;
            if (h < 0 || l < 0) {
                lt_text_drop(r);
                return lt_make_failure(lt_text_cstr("url: a bad %-escape"));
            }
            r->data[w++] = (char)(h * 16 + l);
            i += 2;
        } else {
            r->data[w++] = c == '+' ? ' ' : c;
        }
    }
    r->len = w;
    r->data[w] = 0;
    *out = r;
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- csv

// rows as lists of text; returns the number of rows in *nrows
static lt_err lt_csv_parse(lt_text *t, lt_texts ***rows_out, int64_t *nrows) {
    int64_t cap = 16, n = 0;
    lt_texts **rows = (lt_texts **)malloc(sizeof(lt_texts *) * (size_t)cap);
    const char *p = t->data, *e = t->data + t->len;
    int64_t line = 1;
    while (p < e) {
        lt_texts *row = lt_texts_new(4);
        for (;;) {
            size_t fcap = 64, flen = 0;
            char *f = (char *)malloc(fcap);
            if (p < e && *p == '"') {
                p++;
                for (;;) {
                    if (p >= e) {
                        free(f);
                        for (int64_t i = 0; i < row->len; i++) lt_text_drop(row->items[i]);
                        for (int64_t r = 0; r < n; r++) {
                            for (int64_t i = 0; i < rows[r]->len; i++) lt_text_drop(rows[r]->items[i]);
                            lt_free(rows[r], sizeof(lt_texts) + sizeof(lt_text *) * (size_t)rows[r]->cap);
                        }
                        lt_free(row, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)row->cap);
                        free(rows);
                        char buf[96];
                        snprintf(buf, sizeof buf, "csv: a quoted field that starts on line %lld is never closed", (long long)line);
                        return lt_make_failure(lt_text_cstr(buf));
                    }
                    if (*p == '"') {
                        if (p + 1 < e && p[1] == '"') {
                            p += 2;
                            if (flen + 1 >= fcap) f = (char *)realloc(f, fcap *= 2);
                            f[flen++] = '"';
                            continue;
                        }
                        p++;
                        break;
                    }
                    if (*p == '\n') line++;
                    if (flen + 1 >= fcap) f = (char *)realloc(f, fcap *= 2);
                    f[flen++] = *p++;
                }
                while (p < e && *p != ',' && *p != '\n' && *p != '\r') p++;
            } else {
                while (p < e && *p != ',' && *p != '\n' && *p != '\r') {
                    if (flen + 1 >= fcap) f = (char *)realloc(f, fcap *= 2);
                    f[flen++] = *p++;
                }
            }
            lt_texts_push(&row, lt_text_from(f, (int64_t)flen));
            free(f);
            if (p < e && *p == ',') {
                p++;
                continue;
            }
            if (p < e && *p == '\r') p++;
            if (p < e && *p == '\n') p++;
            line++;
            break;
        }
        if (n == cap) rows = (lt_texts **)realloc(rows, sizeof(lt_texts *) * (size_t)(cap *= 2));
        rows[n++] = row;
    }
    *rows_out = rows;
    *nrows = n;
    return (lt_err){ 0 };
}

static void lt_csv_field(lt_buf *b, lt_text *f) {
    bool quote = false;
    for (int64_t i = 0; i < f->len; i++) {
        char c = f->data[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') quote = true;
    }
    if (!quote) {
        lt_buf_put(b, f->data, f->len);
        return;
    }
    lt_buf_c(b, '"');
    for (int64_t i = 0; i < f->len; i++) {
        if (f->data[i] == '"') lt_buf_c(b, '"');
        lt_buf_c(b, f->data[i]);
    }
    lt_buf_c(b, '"');
}

// ---------------------------------------------------------------- xml
// The scanner turns a document into flat events for xml.lang to build a tree:
// "open" name, then "attr" key value per attribute, "text" content, "close".

#include <ctype.h>

typedef struct {
    const char *s, *e, *start;
    lt_texts *out;
    const char *msg;
    const char *at;
} lt_xml;

static bool lt_xml_fail(lt_xml *x, const char *msg) {
    if (!x->msg) {
        x->msg = msg;
        x->at = x->s;
    }
    return false;
}

static bool lt_xml_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.' || (unsigned char)c >= 0x80;
}

// decodes entities in [p, e) into b
static bool lt_xml_decode(lt_xml *x, const char *p, const char *e, lt_buf *b) {
    while (p < e) {
        const char *amp = memchr(p, '&', (size_t)(e - p));
        if (!amp) {
            lt_buf_put(b, p, e - p);
            return true;
        }
        lt_buf_put(b, p, amp - p);
        const char *semi = memchr(amp, ';', (size_t)(e - amp));
        if (!semi || semi - amp > 12) {
            x->s = amp;
            return lt_xml_fail(x, "an `&` that doesn't start an entity (write `&amp;`)");
        }
        const char *n = amp + 1;
        size_t len = (size_t)(semi - n);
        if (len == 2 && memcmp(n, "lt", 2) == 0) lt_buf_c(b, '<');
        else if (len == 2 && memcmp(n, "gt", 2) == 0) lt_buf_c(b, '>');
        else if (len == 3 && memcmp(n, "amp", 3) == 0) lt_buf_c(b, '&');
        else if (len == 4 && memcmp(n, "quot", 4) == 0) lt_buf_c(b, '"');
        else if (len == 4 && memcmp(n, "apos", 4) == 0) lt_buf_c(b, '\'');
        else if (len >= 2 && n[0] == '#') {
            uint32_t c = 0;
            bool hex = n[1] == 'x' || n[1] == 'X';
            const char *d = n + (hex ? 2 : 1);
            if (d == semi) goto bad;
            for (; d < semi; d++) {
                int v = hex ? lt_hex(*d) : (*d >= '0' && *d <= '9' ? *d - '0' : -1);
                if (v < 0 || c > 0x10FFFF) goto bad;
                c = c * (hex ? 16 : 10) + (uint32_t)v;
            }
            if (c == 0 || c > 0x10FFFF) goto bad;
            char u[4], *w = u;
            lt_utf8_put(&w, c);
            lt_buf_put(b, u, w - u);
        } else {
        bad:
            x->s = amp;
            return lt_xml_fail(x, "an unknown entity");
        }
        p = semi + 1;
    }
    return true;
}

static void lt_xml_emit(lt_xml *x, const char *s, int64_t n) {
    lt_texts_push(&x->out, lt_text_from(s, n));
}

static void lt_xml_skip_space(lt_xml *x) {
    while (x->s < x->e && isspace((unsigned char)*x->s)) x->s++;
}

static bool lt_xml_skip_past(lt_xml *x, const char *end, const char *what) {
    size_t n = strlen(end);
    for (const char *p = x->s; p + n <= x->e; p++) {
        if (memcmp(p, end, n) == 0) {
            x->s = p + n;
            return true;
        }
    }
    return lt_xml_fail(x, what);
}

static bool lt_xml_name(lt_xml *x, const char **n, int64_t *len) {
    const char *p = x->s;
    while (x->s < x->e && lt_xml_name_char(*x->s)) x->s++;
    if (x->s == p) return lt_xml_fail(x, "expected a name");
    *n = p;
    *len = x->s - p;
    return true;
}

static bool lt_xml_text(lt_xml *x, const char *p, const char *e, bool keep_space) {
    if (!keep_space) {
        const char *q = p;
        while (q < e && isspace((unsigned char)*q)) q++;
        if (q == e) return true; // whitespace between elements
    }
    lt_buf b = { 0 };
    if (!lt_xml_decode(x, p, e, &b)) {
        free(b.d);
        return false;
    }
    lt_xml_emit(x, "text", 4);
    lt_texts_push(&x->out, lt_buf_text(&b));
    return true;
}

static bool lt_xml_run(lt_xml *x) {
    // open element names, for matching the closing tags
    const char *names[256];
    int64_t lens[256];
    int depth = 0;
    bool had_root = false;
    while (x->s < x->e) {
        if (*x->s != '<') {
            const char *p = x->s;
            while (x->s < x->e && *x->s != '<') x->s++;
            if (depth == 0) {
                for (const char *q = p; q < x->s; q++) {
                    if (!isspace((unsigned char)*q)) {
                        x->s = q;
                        return lt_xml_fail(x, "text outside the root element");
                    }
                }
                continue;
            }
            if (!lt_xml_text(x, p, x->s, false)) return false;
            continue;
        }
        const char *lt = x->s;
        if (x->e - x->s >= 4 && memcmp(x->s, "<!--", 4) == 0) {
            if (!lt_xml_skip_past(x, "-->", "a comment without `-->`")) return false;
        } else if (x->e - x->s >= 9 && memcmp(x->s, "<![CDATA[", 9) == 0) {
            const char *p = x->s + 9;
            x->s = p;
            if (!lt_xml_skip_past(x, "]]>", "a CDATA section without `]]>`")) return false;
            if (depth == 0) {
                x->s = lt;
                return lt_xml_fail(x, "text outside the root element");
            }
            lt_xml_emit(x, "text", 4);
            lt_xml_emit(x, p, x->s - 3 - p);
        } else if (x->e - x->s >= 2 && x->s[1] == '?') {
            if (!lt_xml_skip_past(x, "?>", "a `<?` without `?>`")) return false;
        } else if (x->e - x->s >= 2 && x->s[1] == '!') {
            // <!DOCTYPE ...>, possibly with [ ... ]
            int nest = 0;
            for (x->s += 2; x->s < x->e; x->s++) {
                if (*x->s == '[') nest++;
                else if (*x->s == ']') nest--;
                else if (*x->s == '>' && nest <= 0) break;
            }
            if (x->s >= x->e) return lt_xml_fail(x, "a `<!` without `>`");
            x->s++;
        } else if (x->e - x->s >= 2 && x->s[1] == '/') {
            x->s += 2;
            const char *n;
            int64_t len;
            if (!lt_xml_name(x, &n, &len)) return false;
            if (depth == 0) {
                x->s = lt;
                return lt_xml_fail(x, "a closing tag without an opening one");
            }
            depth--;
            if (depth < 256 && (lens[depth] != len || memcmp(names[depth], n, (size_t)len) != 0)) {
                x->s = lt;
                return lt_xml_fail(x, "the closing tag doesn't match the open element");
            }
            lt_xml_skip_space(x);
            if (x->s >= x->e || *x->s != '>') return lt_xml_fail(x, "expected `>`");
            x->s++;
            lt_xml_emit(x, "close", 5);
        } else {
            if (depth == 0 && had_root) return lt_xml_fail(x, "a second root element");
            had_root = true;
            x->s++;
            const char *n;
            int64_t len;
            if (!lt_xml_name(x, &n, &len)) return false;
            lt_xml_emit(x, "open", 4);
            lt_xml_emit(x, n, len);
            for (;;) {
                const char *before = x->s;
                lt_xml_skip_space(x);
                if (x->s >= x->e) return lt_xml_fail(x, "a tag without `>`");
                if (*x->s == '>' || *x->s == '/') break;
                if (x->s == before) return lt_xml_fail(x, "expected a space before the attribute");
                const char *k;
                int64_t klen;
                if (!lt_xml_name(x, &k, &klen)) return false;
                lt_xml_skip_space(x);
                if (x->s >= x->e || *x->s != '=') return lt_xml_fail(x, "expected `=` after the attribute name");
                x->s++;
                lt_xml_skip_space(x);
                if (x->s >= x->e || (*x->s != '"' && *x->s != '\'')) return lt_xml_fail(x, "expected a quoted attribute value");
                char q = *x->s++;
                const char *v = x->s;
                while (x->s < x->e && *x->s != q) {
                    if (*x->s == '<') return lt_xml_fail(x, "a `<` inside an attribute value");
                    x->s++;
                }
                if (x->s >= x->e) return lt_xml_fail(x, "an attribute value without its closing quote");
                lt_buf b = { 0 };
                if (!lt_xml_decode(x, v, x->s, &b)) {
                    free(b.d);
                    return false;
                }
                x->s++;
                lt_xml_emit(x, "attr", 4);
                lt_xml_emit(x, k, klen);
                lt_texts_push(&x->out, lt_buf_text(&b));
            }
            if (*x->s == '/') {
                x->s++;
                if (x->s >= x->e || *x->s != '>') return lt_xml_fail(x, "expected `>` after `/`");
                x->s++;
                lt_xml_emit(x, "close", 5);
            } else {
                x->s++;
                if (depth >= 256) return lt_xml_fail(x, "elements nested deeper than 256 levels");
                names[depth] = n;
                lens[depth] = len;
                depth++;
            }
        }
    }
    if (depth > 0) return lt_xml_fail(x, "the document ends inside an element");
    if (!had_root) return lt_xml_fail(x, "no root element");
    return true;
}

static lt_err lt_xml_scan(lt_text *t, lt_texts **out) {
    lt_xml x = { t->data, t->data + t->len, t->data, lt_texts_new(16), NULL, NULL };
    if (lt_xml_run(&x)) {
        *out = x.out;
        return (lt_err){ 0 };
    }
    for (int64_t i = 0; i < x.out->len; i++) lt_text_drop(x.out->items[i]);
    lt_free(x.out, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)x.out->cap);
    int64_t line = 1;
    for (const char *p = x.start; p < x.at && p < x.e; p++) line += *p == '\n';
    char buf[256];
    snprintf(buf, sizeof buf, "xml: line %lld: %s", (long long)line, x.msg);
    return lt_make_failure(lt_text_cstr(buf));
}

// text for element content, or for attribute values in double quotes
static lt_text *lt_xml_escape(lt_text *t) {
    lt_buf b = { 0 };
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        switch (c) {
        case '<': lt_buf_put(&b, "&lt;", 4); break;
        case '>': lt_buf_put(&b, "&gt;", 4); break;
        case '&': lt_buf_put(&b, "&amp;", 5); break;
        case '"': lt_buf_put(&b, "&quot;", 6); break;
        default: lt_buf_c(&b, c);
        }
    }
    return lt_buf_text(&b);
}
