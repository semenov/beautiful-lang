// Runtime for compiled programs. The compiler pastes this file at the top of
// every generated C file, so everything is one translation unit and the C
// compiler can inline across the runtime and the program.
//
// Heap objects start with an `int64_t rc`. rc > 0: a live object with that
// many references. rc < 0: an immortal static object (literals, empties).
// Every dup/drop accepts NULL (a zero-initialized or missing value).

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <sys/mman.h>

#define LT_INLINE static inline __attribute__((always_inline))
#define LT_NOINLINE static __attribute__((noinline))
#define LT_LIKELY(x) __builtin_expect(!!(x), 1)
#define LT_UNLIKELY(x) __builtin_expect(!!(x), 0)

static const char *lt_file = "?";

// ---------------------------------------------------------------- reference counts
// In a program that uses tasks, counters change atomically; otherwise with
// plain increments. Negative counts are immortal static objects.
#ifdef LT_THREADS
#define LT_INC(p) do { if (__atomic_load_n(&(p)->rc, __ATOMIC_RELAXED) > 0) __atomic_fetch_add(&(p)->rc, 1, __ATOMIC_RELAXED); } while (0)
#define LT_DEC_ZERO(p) (__atomic_load_n(&(p)->rc, __ATOMIC_RELAXED) > 0 && __atomic_sub_fetch(&(p)->rc, 1, __ATOMIC_ACQ_REL) == 0)
#define LT_UNIQUE(p) (__atomic_load_n(&(p)->rc, __ATOMIC_ACQUIRE) == 1)
#define LT_TLS __thread
#else
#define LT_INC(p) do { if ((p)->rc > 0) (p)->rc++; } while (0)
#define LT_DEC_ZERO(p) ((p)->rc > 0 && --(p)->rc == 0)
#define LT_UNIQUE(p) ((p)->rc == 1)
#define LT_TLS
#endif

// ---------------------------------------------------------------- memory

// Small objects come from per-size free lists: a freed block goes on the list
// for its size class and is reused by the next allocation of that class.
#define LT_CLASSES 32
#define LT_CLASS_BYTES 16
typedef struct lt_free_node { struct lt_free_node *next; } lt_free_node;
static LT_TLS lt_free_node *lt_free_lists[LT_CLASSES];
static LT_TLS char *lt_arena_cur, *lt_arena_end;

LT_NOINLINE void lt_oom(void) {
    fflush(stdout);
    fprintf(stderr, "panic: out of memory\n");
    exit(101);
}

// Large blocks go straight to the OS, so freeing them returns the memory
// (the system allocator keeps freed large blocks around).
#define LT_BIG ((size_t)1 << 20)
#define LT_PAGE ((size_t)16384)
// A large block has a 16-byte header with the size of its mapping, which may
// be larger than requested: untouched pages cost no memory, and a growing
// list can then extend without copying.
typedef struct { size_t mapped; size_t pad; } lt_big_hdr;
LT_INLINE size_t lt_round_page(size_t n) { return (n + LT_PAGE - 1) & ~(LT_PAGE - 1); }
LT_NOINLINE void *lt_big_map(size_t reserve) {
    size_t m = lt_round_page(reserve + sizeof(lt_big_hdr));
    void *p = mmap(NULL, m, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) lt_oom();
    ((lt_big_hdr *)p)->mapped = m;
    return (char *)p + sizeof(lt_big_hdr);
}
LT_NOINLINE void *lt_big_alloc(size_t n) { return lt_big_map(n); }
LT_NOINLINE void lt_big_free(void *p, size_t n) {
    (void)n;
    lt_big_hdr *h = (lt_big_hdr *)((char *)p - sizeof(lt_big_hdr));
    munmap(h, h->mapped);
}
LT_NOINLINE void *lt_big_realloc(void *p, size_t old, size_t n) {
    lt_big_hdr *h = (lt_big_hdr *)((char *)p - sizeof(lt_big_hdr));
    if (n + sizeof(lt_big_hdr) <= h->mapped) return p;
    size_t want_total = lt_round_page(2 * n + sizeof(lt_big_hdr));
    char *want = (char *)h + h->mapped;
    void *q = mmap(want, want_total - h->mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q == want) {
        h->mapped = want_total;
        return p;
    }
    if (q != MAP_FAILED) munmap(q, want_total - h->mapped);
    void *r = lt_big_map(2 * n);
    memcpy(r, p, old);
    munmap(h, h->mapped);
    return r;
}

LT_NOINLINE void *lt_arena_refill(size_t n) {
    size_t chunk = 1 << 20;
    char *p = (char *)malloc(chunk);
    if (!p) lt_oom();
    lt_arena_cur = p + n;
    lt_arena_end = p + chunk;
    return p;
}

#ifdef LT_DEBUG_ALLOC
static int64_t lt_live_objects, lt_total_objects;
static void lt_report_leaks(void) {
    fprintf(stderr, "debug: %lld allocations, %lld not freed\n", (long long)lt_total_objects, (long long)lt_live_objects);
}
LT_INLINE void *lt_alloc(size_t n) {
    __atomic_fetch_add(&lt_live_objects, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&lt_total_objects, 1, __ATOMIC_RELAXED);
    void *p = malloc(n);
    if (!p) lt_oom();
    return p;
}
LT_INLINE void lt_free(void *p, size_t n) {
    (void)n;
    __atomic_fetch_sub(&lt_live_objects, 1, __ATOMIC_RELAXED);
    free(p);
}
LT_INLINE void *lt_realloc(void *p, size_t old, size_t n) {
    (void)old;
    void *q = realloc(p, n);
    if (!q) lt_oom();
    return q;
}
#elif defined(LT_THREADS)
// With tasks, a task can move to another OS thread at any wait, and C
// compilers may keep the address of a thread-local variable across such a
// point; so the small-object free lists (which are per thread) are not used:
// the system allocator is thread-safe by itself.
LT_INLINE void *lt_alloc(size_t n) {
    if (n >= LT_BIG) return lt_big_alloc(n);
    void *p = malloc(n);
    if (!p) lt_oom();
    return p;
}
LT_INLINE void lt_free(void *p, size_t n) {
    if (n >= LT_BIG) {
        lt_big_free(p, n);
        return;
    }
    free(p);
}
LT_INLINE void *lt_realloc(void *p, size_t old, size_t n) {
    if (old >= LT_BIG && n >= LT_BIG) return lt_big_realloc(p, old, n);
    if (old < LT_BIG && n >= LT_BIG) {
        void *q = lt_big_map(2 * n);
        memcpy(q, p, old);
        free(p);
        return q;
    }
    if (old >= LT_BIG) {
        void *q = malloc(n);
        memcpy(q, p, n);
        lt_big_free(p, old);
        return q;
    }
    void *q = realloc(p, n);
    if (!q) lt_oom();
    return q;
}
#else
LT_INLINE void *lt_alloc(size_t n) {
    size_t c = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (LT_LIKELY(c < LT_CLASSES)) {
        lt_free_node *f = lt_free_lists[c];
        if (f) {
            lt_free_lists[c] = f->next;
            return f;
        }
        size_t sz = c * LT_CLASS_BYTES;
        if (LT_LIKELY(lt_arena_cur + sz <= lt_arena_end)) {
            void *p = lt_arena_cur;
            lt_arena_cur += sz;
            return p;
        }
        return lt_arena_refill(sz);
    }
    if (n >= LT_BIG) return lt_big_alloc(n);
    void *p = malloc(n);
    if (!p) lt_oom();
    return p;
}

LT_INLINE void lt_free(void *p, size_t n) {
    size_t c = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (LT_LIKELY(c < LT_CLASSES)) {
        lt_free_node *f = (lt_free_node *)p;
        f->next = lt_free_lists[c];
        lt_free_lists[c] = f;
        return;
    }
    if (n >= LT_BIG) {
        lt_big_free(p, n);
        return;
    }
    free(p);
}

LT_INLINE void *lt_realloc(void *p, size_t old, size_t n) {
    size_t oc = (old + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    size_t nc = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (old >= LT_BIG && n >= LT_BIG) return lt_big_realloc(p, old, n);
    if (old < LT_BIG && n >= LT_BIG && oc >= LT_CLASSES) {
        void *q = lt_big_map(2 * n);
        memcpy(q, p, old);
        free(p);
        return q;
    }
    if (oc >= LT_CLASSES && nc >= LT_CLASSES && old < LT_BIG && n < LT_BIG) {
        void *q = realloc(p, n);
        if (!q) lt_oom();
        return q;
    }
    if (oc == nc) return p;
    void *q = lt_alloc(n);
    memcpy(q, p, old < n ? old : n);
    lt_free(p, old);
    return q;
}

#endif

// ---------------------------------------------------------------- panics

static void (*lt_panic_hook)(const char *msg, int line);

LT_NOINLINE _Noreturn void lt_panic_at(const char *msg, int line) {
    if (lt_panic_hook) lt_panic_hook(msg, line); // returns if it can't handle it
    fflush(stdout);
    if (line > 0) {
        fprintf(stderr, "panic: %s\n  at %s:%d\n", msg, lt_file, line);
    } else {
        fprintf(stderr, "panic: %s\n", msg);
    }
    exit(101);
}

// ---------------------------------------------------------------- core types

typedef struct lt_obj { int64_t rc; } lt_obj;

typedef struct lt_text {
    int64_t rc;
    int64_t len; // bytes, UTF-8
    char data[];
} lt_text;

struct lt_vt;
typedef struct lt_iface {
    lt_obj *obj;
    const struct lt_vt *vt;
} lt_iface;
typedef lt_iface lt_err;

typedef struct lt_vt {
    void (*drop)(lt_obj *);
    bool (*eq)(lt_obj *, lt_obj *);
    uint64_t (*hash)(lt_obj *);
    lt_text *(*to_text)(lt_obj *, bool);
    lt_obj *(*clone)(lt_obj *);
    int64_t type_id;
    void *m[];
} lt_vt;

typedef struct lt_env {
    int64_t rc;
    void (*drop)(struct lt_env *);
} lt_env;

typedef struct lt_fn {
    void *fn;
    lt_env *env;
} lt_fn;

LT_INLINE void lt_fn_dup(lt_fn f) {
    if (f.env) LT_INC(f.env);
}
LT_INLINE void lt_fn_drop(lt_fn f) {
    if (f.env && LT_DEC_ZERO(f.env)) f.env->drop(f.env);
}

LT_INLINE void lt_iface_dup(lt_iface x) {
    if (x.obj) LT_INC(x.obj);
}
LT_INLINE void lt_iface_drop(lt_iface x) {
    if (x.obj && LT_DEC_ZERO(x.obj)) x.vt->drop(x.obj);
}
// before calling a mutating method through an interface
LT_INLINE void lt_iface_unique(lt_iface *x) {
    if (!LT_UNIQUE(x->obj)) {
        lt_obj *c = x->vt->clone(x->obj);
        lt_iface_drop(*x);
        x->obj = c;
    }
}

static lt_err lt_make_failure(lt_text *msg);

// ---------------------------------------------------------------- text

#define LT_TEXT_SIZE(n) (sizeof(lt_text) + (size_t)(n) + 1)

LT_INLINE void lt_text_dup(lt_text *t) {
    if (t) LT_INC(t);
}
LT_INLINE void lt_text_drop(lt_text *t) {
    if (t && LT_DEC_ZERO(t)) lt_free(t, LT_TEXT_SIZE(t->len));
}
LT_INLINE lt_text *lt_text_ret(lt_text *t) {
    lt_text_dup(t);
    return t;
}

static lt_text *lt_text_new(int64_t len) {
    lt_text *t = (lt_text *)lt_alloc(LT_TEXT_SIZE(len));
    t->rc = 1;
    t->len = len;
    t->data[len] = 0;
    return t;
}

static lt_text *lt_text_from(const char *s, int64_t len) {
    lt_text *t = lt_text_new(len);
    memcpy(t->data, s, (size_t)len);
    return t;
}

static lt_text *lt_text_cstr(const char *s) {
    return lt_text_from(s, (int64_t)strlen(s));
}

static struct { int64_t rc; int64_t len; char data[1]; } lt_empty_text_obj = { -1, 0, "" };
#define LT_EMPTY_TEXT ((lt_text *)&lt_empty_text_obj)

LT_INLINE bool lt_text_eq(lt_text *a, lt_text *b) {
    if (a == b) return true;
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

LT_INLINE int64_t lt_text_cmp(lt_text *a, lt_text *b) {
    int64_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)n);
    if (c != 0) return c < 0 ? -1 : 1;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

// ---------------------------------------------------------------- hashing

LT_INLINE uint64_t lt_mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

LT_INLINE uint64_t lt_hash_combine(uint64_t a, uint64_t b) {
    return lt_mix(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

static inline uint64_t lt_hash_bytes(const char *p, int64_t n) {
    uint64_t h = 0x243f6a8885a308d3ULL ^ (uint64_t)n;
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ w) * 0x100000001b3ULL;
        h ^= h >> 29;
        p += 8;
        n -= 8;
    }
    uint64_t w = 0;
    memcpy(&w, p, (size_t)n);
    h = (h ^ w) * 0x100000001b3ULL;
    return lt_mix(h);
}

LT_INLINE uint64_t lt_text_hash(lt_text *t) { return lt_hash_bytes(t->data, t->len); }
LT_INLINE uint64_t lt_int_hash(int64_t v) { return lt_mix((uint64_t)v + 0x9e3779b97f4a7c15ULL); }
LT_INLINE uint64_t lt_float_hash(double v) {
    if (v == 0) v = 0; // -0.0 == 0.0
    uint64_t b;
    memcpy(&b, &v, 8);
    return lt_mix(b);
}

// ---------------------------------------------------------------- lists of text
// List<Text> has a fixed C type so the runtime can build and read it.

typedef struct lt_texts {
    int64_t rc;
    int64_t len;
    int64_t cap;
    lt_text *items[];
} lt_texts;

typedef struct { int64_t rc; int64_t len; int64_t cap; } lt_list_hdr;
static lt_list_hdr lt_empty_list = { -1, 0, 0 };

static lt_texts *lt_texts_new(int64_t cap) {
    if (cap == 0) return (lt_texts *)&lt_empty_list;
    lt_texts *l = (lt_texts *)lt_alloc(sizeof(lt_texts) + sizeof(lt_text *) * (size_t)cap);
    l->rc = 1;
    l->len = 0;
    l->cap = cap;
    return l;
}

static void lt_texts_push(lt_texts **lp, lt_text *t) {
    lt_texts *l = *lp;
    if (l->len == l->cap) {
        int64_t nc = l->cap < 4 ? 4 : l->cap * 2;
        if (l->rc < 0) {
            lt_texts *n = lt_texts_new(nc);
            *lp = l = n;
        } else {
            l = (lt_texts *)lt_realloc(l, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)l->cap, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)nc);
            l->cap = nc;
            *lp = l;
        }
    }
    l->items[l->len++] = t;
}

// ---------------------------------------------------------------- numbers

LT_NOINLINE _Noreturn void lt_overflow(int line) { lt_panic_at("whole number overflow", line); }

LT_INLINE int64_t lt_add(int64_t a, int64_t b, int line) {
    int64_t r;
    if (LT_UNLIKELY(__builtin_add_overflow(a, b, &r))) lt_overflow(line);
    return r;
}
LT_INLINE int64_t lt_sub(int64_t a, int64_t b, int line) {
    int64_t r;
    if (LT_UNLIKELY(__builtin_sub_overflow(a, b, &r))) lt_overflow(line);
    return r;
}
LT_INLINE int64_t lt_mul(int64_t a, int64_t b, int line) {
    int64_t r;
    if (LT_UNLIKELY(__builtin_mul_overflow(a, b, &r))) lt_overflow(line);
    return r;
}
LT_INLINE int64_t lt_neg(int64_t a, int line) {
    if (LT_UNLIKELY(a == INT64_MIN)) lt_overflow(line);
    return -a;
}
LT_INLINE int64_t lt_div(int64_t a, int64_t b, int line) {
    if (LT_UNLIKELY(b == 0)) lt_panic_at("division by zero", line);
    if (LT_UNLIKELY(a == INT64_MIN && b == -1)) lt_overflow(line);
    // floor division, like Python: -7.div(2) == -4
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
LT_INLINE int64_t lt_rem(int64_t a, int64_t b, int line) {
    if (LT_UNLIKELY(b == 0)) lt_panic_at("remainder of division by zero", line);
    if (LT_UNLIKELY(b == -1)) return 0;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
LT_INLINE int64_t lt_abs(int64_t a, int line) { return a < 0 ? lt_neg(a, line) : a; }
static int64_t lt_ipow(int64_t a, int64_t b, int line) {
    if (b < 0) lt_panic_at("a whole number can't be raised to a negative power; use Float", line);
    int64_t r = 1;
    while (b > 0) {
        if (b & 1) r = lt_mul(r, a, line);
        b >>= 1;
        if (b) a = lt_mul(a, a, line);
    }
    return r;
}
LT_INLINE int64_t lt_f2i(double x, int line) {
    if (LT_UNLIKELY(!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))) {
        lt_panic_at(isnan(x) ? "can't turn NaN into a whole number" : "the number is too large for Int", line);
    }
    return (int64_t)x;
}

static lt_text *lt_int_to_text(int64_t v) {
    char buf[24];
    char *p = buf + sizeof buf;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    return lt_text_from(p, (int64_t)(buf + sizeof buf - p));
}

static lt_text *lt_float_to_text(double v) {
    char buf[40];
    if (isnan(v)) return lt_text_cstr("NaN");
    if (isinf(v)) return lt_text_cstr(v > 0 ? "Infinity" : "-Infinity");
    int n = 0;
    double a = fabs(v);
    if (a == 0 || (a >= 1e-5 && a < 1e16)) {
        // plain notation with the fewest digits that read back exactly
        for (int d = 0; d <= 20; d++) {
            n = snprintf(buf, sizeof buf, "%.*f", d, v);
            if (strtod(buf, NULL) == v) break;
        }
    } else {
        for (int prec = 1; prec <= 17; prec++) {
            n = snprintf(buf, sizeof buf, "%.*g", prec, v);
            if (strtod(buf, NULL) == v) break;
        }
    }
    bool has_dot = false;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'n' || buf[i] == 'i') has_dot = true;
    }
    if (!has_dot) {
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = 0;
    }
    return lt_text_from(buf, n);
}

static lt_text *lt_bool_to_text(bool b) { return lt_text_cstr(b ? "true" : "false"); }

// ---------------------------------------------------------------- text operations

static lt_text *lt_text_concat_n(int n, lt_text **parts) {
    int64_t len = 0;
    for (int i = 0; i < n; i++) len += parts[i]->len;
    lt_text *t = lt_text_new(len);
    char *p = t->data;
    for (int i = 0; i < n; i++) {
        memcpy(p, parts[i]->data, (size_t)parts[i]->len);
        p += parts[i]->len;
    }
    return t;
}

static int64_t lt_text_length(lt_text *t) {
    int64_t n = 0;
    for (int64_t i = 0; i < t->len; i++) {
        if (((unsigned char)t->data[i] & 0xC0) != 0x80) n++;
    }
    return n;
}

// byte offset of the character with index `ci` (clamped to the end)
static int64_t lt_text_char_offset(lt_text *t, int64_t ci) {
    if (ci <= 0) return 0;
    int64_t n = 0;
    for (int64_t i = 0; i < t->len; i++) {
        if (((unsigned char)t->data[i] & 0xC0) != 0x80) {
            if (n == ci) return i;
            n++;
        }
    }
    return t->len;
}

static lt_text *lt_text_slice(lt_text *t, int64_t from, int64_t to) {
    if (to < from) return LT_EMPTY_TEXT;
    int64_t a = lt_text_char_offset(t, from);
    int64_t b = lt_text_char_offset(t, to);
    return lt_text_from(t->data + a, b - a);
}

static const char *lt_find(const char *h, int64_t hn, const char *n, int64_t nn) {
    if (nn == 0) return h;
    if (nn > hn) return NULL;
    const char *end = h + hn - nn;
    for (const char *p = h; p <= end; p++) {
        p = (const char *)memchr(p, n[0], (size_t)(end - p + 1));
        if (!p) return NULL;
        if (memcmp(p, n, (size_t)nn) == 0) return p;
    }
    return NULL;
}

static bool lt_text_contains(lt_text *t, lt_text *part) { return lt_find(t->data, t->len, part->data, part->len) != NULL; }
static bool lt_text_starts_with(lt_text *t, lt_text *p) { return p->len <= t->len && memcmp(t->data, p->data, (size_t)p->len) == 0; }
static bool lt_text_ends_with(lt_text *t, lt_text *p) { return p->len <= t->len && memcmp(t->data + t->len - p->len, p->data, (size_t)p->len) == 0; }

static lt_text *lt_text_lower(lt_text *t) {
    lt_text *r = lt_text_new(t->len);
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    return r;
}
static lt_text *lt_text_upper(lt_text *t) {
    lt_text *r = lt_text_new(t->len);
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    return r;
}

LT_INLINE bool lt_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static lt_text *lt_text_trim(lt_text *t) {
    int64_t a = 0, b = t->len;
    while (a < b && lt_is_space(t->data[a])) a++;
    while (b > a && lt_is_space(t->data[b - 1])) b--;
    if (a == 0 && b == t->len) return lt_text_ret(t);
    return lt_text_from(t->data + a, b - a);
}

static lt_texts *lt_text_chars(lt_text *t) {
    lt_texts *l = lt_texts_new(t->len);
    int64_t i = 0;
    while (i < t->len) {
        int64_t j = i + 1;
        while (j < t->len && ((unsigned char)t->data[j] & 0xC0) == 0x80) j++;
        lt_texts_push(&l, lt_text_from(t->data + i, j - i));
        i = j;
    }
    return l;
}

static lt_texts *lt_text_split(lt_text *t, lt_text *sep) {
    if (sep->len == 0) return lt_text_chars(t);
    lt_texts *l = lt_texts_new(4);
    const char *p = t->data, *end = t->data + t->len;
    for (;;) {
        const char *q = lt_find(p, end - p, sep->data, sep->len);
        if (!q) {
            lt_texts_push(&l, lt_text_from(p, end - p));
            break;
        }
        lt_texts_push(&l, lt_text_from(p, q - p));
        p = q + sep->len;
    }
    return l;
}

static lt_texts *lt_text_lines(lt_text *t) {
    lt_texts *l = lt_texts_new(4);
    const char *p = t->data, *end = t->data + t->len;
    while (p < end) {
        const char *q = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *e = q ? q : end;
        const char *e2 = (e > p && e[-1] == '\r') ? e - 1 : e;
        lt_texts_push(&l, lt_text_from(p, e2 - p));
        if (!q) break;
        p = q + 1;
    }
    return l;
}

static lt_texts *lt_text_words(lt_text *t) {
    lt_texts *l = lt_texts_new(4);
    int64_t i = 0, n = t->len;
    const char *d = t->data;
    while (i < n) {
        while (i < n && lt_is_space(d[i])) i++;
        if (i >= n) break;
        int64_t j = i;
        while (j < n && !lt_is_space(d[j])) j++;
        lt_texts_push(&l, lt_text_from(d + i, j - i));
        i = j;
    }
    return l;
}

static lt_text *lt_text_replace(lt_text *t, lt_text *old, lt_text *new_) {
    if (old->len == 0) return lt_text_ret(t);
    int64_t count = 0;
    const char *p = t->data, *end = t->data + t->len;
    for (;;) {
        const char *q = lt_find(p, end - p, old->data, old->len);
        if (!q) break;
        count++;
        p = q + old->len;
    }
    if (count == 0) return lt_text_ret(t);
    lt_text *r = lt_text_new(t->len + count * (new_->len - old->len));
    char *w = r->data;
    p = t->data;
    for (;;) {
        const char *q = lt_find(p, end - p, old->data, old->len);
        if (!q) {
            memcpy(w, p, (size_t)(end - p));
            break;
        }
        memcpy(w, p, (size_t)(q - p));
        w += q - p;
        memcpy(w, new_->data, (size_t)new_->len);
        w += new_->len;
        p = q + old->len;
    }
    return r;
}

static lt_text *lt_text_repeat(lt_text *t, int64_t n, int line) {
    if (n < 0) lt_panic_at("repeat count is negative", line);
    lt_text *r = lt_text_new(t->len * n);
    for (int64_t i = 0; i < n; i++) memcpy(r->data + i * t->len, t->data, (size_t)t->len);
    return r;
}

static lt_text *lt_text_pad(lt_text *t, int64_t width, lt_text *fill, bool start) {
    int64_t have = lt_text_length(t);
    if (have >= width || fill->len == 0) return lt_text_ret(t);
    int64_t need = width - have;
    int64_t fill_chars = lt_text_length(fill);
    // build `need` characters from repeated `fill`
    int64_t reps = (need + fill_chars - 1) / fill_chars;
    lt_text *pad = lt_text_new(fill->len * reps);
    for (int64_t i = 0; i < reps; i++) memcpy(pad->data + i * fill->len, fill->data, (size_t)fill->len);
    int64_t cut = lt_text_char_offset(pad, need);
    lt_text *r = lt_text_new(t->len + cut);
    if (start) {
        memcpy(r->data, pad->data, (size_t)cut);
        memcpy(r->data + cut, t->data, (size_t)t->len);
    } else {
        memcpy(r->data, t->data, (size_t)t->len);
        memcpy(r->data + t->len, pad->data, (size_t)cut);
    }
    lt_text_drop(pad);
    return r;
}

static lt_text *lt_text_join(lt_texts *l, lt_text *sep) {
    if (l->len == 0) return LT_EMPTY_TEXT;
    int64_t len = sep->len * (l->len - 1);
    for (int64_t i = 0; i < l->len; i++) len += l->items[i]->len;
    lt_text *r = lt_text_new(len);
    char *w = r->data;
    for (int64_t i = 0; i < l->len; i++) {
        if (i > 0) {
            memcpy(w, sep->data, (size_t)sep->len);
            w += sep->len;
        }
        memcpy(w, l->items[i]->data, (size_t)l->items[i]->len);
        w += l->items[i]->len;
    }
    return r;
}

static lt_text *lt_text_quote(lt_text *t) {
    lt_text *r = lt_text_new(t->len * 2 + 2);
    char *w = r->data;
    *w++ = '"';
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        switch (c) {
        case '"': *w++ = '\\'; *w++ = '"'; break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\n': *w++ = '\\'; *w++ = 'n'; break;
        case '\t': *w++ = '\\'; *w++ = 't'; break;
        case '\r': *w++ = '\\'; *w++ = 'r'; break;
        default: *w++ = c;
        }
    }
    *w++ = '"';
    r->len = w - r->data;
    *w = 0;
    return r;
}

static lt_err lt_text_to_int(lt_text *t, int64_t *out) {
    int64_t a = 0, b = t->len;
    while (a < b && lt_is_space(t->data[a])) a++;
    while (b > a && lt_is_space(t->data[b - 1])) b--;
    bool neg = false;
    int64_t i = a;
    if (i < b && (t->data[i] == '-' || t->data[i] == '+')) {
        neg = t->data[i] == '-';
        i++;
    }
    bool ok = i < b;
    uint64_t v = 0;
    for (; i < b && ok; i++) {
        char c = t->data[i];
        if (c == '_' && i > a) continue;
        if (c < '0' || c > '9') {
            ok = false;
            break;
        }
        if (v > (UINT64_MAX - 9) / 10) {
            ok = false;
            break;
        }
        v = v * 10 + (uint64_t)(c - '0');
    }
    if (ok && (neg ? v > (uint64_t)INT64_MAX + 1 : v > (uint64_t)INT64_MAX)) ok = false;
    if (!ok) {
        lt_text *q = lt_text_quote(t);
        lt_text *suffix = lt_text_cstr(" is not a whole number");
        lt_text *parts[2] = { q, suffix };
        lt_text *msg = lt_text_concat_n(2, parts);
        lt_text_drop(q);
        lt_text_drop(suffix);
        return lt_make_failure(msg);
    }
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return (lt_err){ 0 };
}

static lt_err lt_text_to_float(lt_text *t, double *out) {
    char *end = NULL;
    lt_text *tr = lt_text_trim(t);
    errno = 0;
    double v = tr->len ? strtod(tr->data, &end) : 0;
    bool ok = tr->len > 0 && end == tr->data + tr->len;
    lt_text_drop(tr);
    if (!ok) {
        lt_text *q = lt_text_quote(t);
        lt_text *suffix = lt_text_cstr(" is not a number");
        lt_text *parts[2] = { q, suffix };
        lt_text *msg = lt_text_concat_n(2, parts);
        lt_text_drop(q);
        lt_text_drop(suffix);
        return lt_make_failure(msg);
    }
    *out = v;
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- time

#include <time.h>
static int64_t lt_monotonic_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// ---------------------------------------------------------------- bytes

typedef struct lt_bytes {
    int64_t rc;
    int64_t len;
    int64_t cap;
    unsigned char data[];
} lt_bytes;

#define LT_BYTES_SIZE(c) (sizeof(lt_bytes) + (size_t)(c))
static struct { int64_t rc, len, cap; } lt_empty_bytes_obj = { -1, 0, 0 };
#define LT_EMPTY_BYTES ((lt_bytes *)&lt_empty_bytes_obj)

LT_INLINE void lt_bytes_dup(lt_bytes *b) {
    if (b) LT_INC(b);
}
LT_INLINE void lt_bytes_drop(lt_bytes *b) {
    if (b && LT_DEC_ZERO(b)) lt_free(b, LT_BYTES_SIZE(b->cap));
}

static lt_bytes *lt_bytes_new(int64_t cap) {
    if (cap <= 0) return LT_EMPTY_BYTES;
    lt_bytes *b = (lt_bytes *)lt_alloc(LT_BYTES_SIZE(cap));
    b->rc = 1;
    b->len = 0;
    b->cap = cap;
    return b;
}

static lt_bytes *lt_bytes_from(const void *p, int64_t n) {
    lt_bytes *b = lt_bytes_new(n);
    if (n > 0) memcpy(b->data, p, (size_t)n);
    b->len = n;
    return b;
}

// room for `more` bytes in a value only this variable holds
static void lt_bytes_reserve(lt_bytes **p, int64_t more) {
    lt_bytes *b = *p;
    if (LT_UNIQUE(b) && b->len + more <= b->cap) return;
    int64_t cap = b->cap * 2;
    if (cap < b->len + more) cap = b->len + more;
    if (cap < 16) cap = 16;
    if (LT_UNIQUE(b)) {
        b = (lt_bytes *)lt_realloc(b, LT_BYTES_SIZE(b->cap), LT_BYTES_SIZE(cap));
        b->cap = cap;
        *p = b;
    } else {
        lt_bytes *n = lt_bytes_new(cap);
        memcpy(n->data, b->data, (size_t)b->len);
        n->len = b->len;
        lt_bytes_drop(b);
        *p = n;
    }
}

static void lt_bytes_append(lt_bytes **p, int64_t v, int line) {
    if (v < 0 || v > 255) lt_panic_at("a byte is a number from 0 to 255", line);
    lt_bytes_reserve(p, 1);
    (*p)->data[(*p)->len++] = (unsigned char)v;
}
static void lt_bytes_append_raw(lt_bytes **p, const void *d, int64_t n) {
    if (n <= 0) return;
    lt_bytes_reserve(p, n);
    memcpy((*p)->data + (*p)->len, d, (size_t)n);
    (*p)->len += n;
}
static void lt_bytes_append_int(lt_bytes **p, int64_t v, int64_t size, int line) {
    if (size < 1 || size > 8) lt_panic_at("the size of a number in bytes is 1 to 8", line);
    lt_bytes_reserve(p, size);
    for (int64_t i = size - 1; i >= 0; i--) (*p)->data[(*p)->len++] = (unsigned char)((uint64_t)v >> (8 * i));
}
static int64_t lt_bytes_get(lt_bytes *b, int64_t i, int line) {
    if ((uint64_t)i >= (uint64_t)b->len) {
        char m[96];
        snprintf(m, sizeof m, "index %lld is out of range for %lld bytes", (long long)i, (long long)b->len);
        lt_panic_at(m, line);
    }
    return b->data[i];
}
static int64_t lt_bytes_int_at(lt_bytes *b, int64_t off, int64_t size, int line) {
    if (size < 1 || size > 8 || off < 0 || off + size > b->len) lt_panic_at("the number is outside the bytes", line);
    uint64_t v = 0;
    for (int64_t i = 0; i < size; i++) v = (v << 8) | b->data[off + i];
    return (int64_t)v;
}
static lt_bytes *lt_bytes_slice(lt_bytes *b, int64_t from, int64_t to) {
    if (from < 0) from = 0;
    if (to > b->len) to = b->len;
    if (to <= from) return LT_EMPTY_BYTES;
    return lt_bytes_from(b->data + from, to - from);
}
static lt_bytes *lt_bytes_concat(lt_bytes *a, lt_bytes *b) {
    lt_bytes *r = lt_bytes_new(a->len + b->len);
    memcpy(r->data, a->data, (size_t)a->len);
    memcpy(r->data + a->len, b->data, (size_t)b->len);
    r->len = a->len + b->len;
    return r;
}
static int64_t lt_bytes_index_of(lt_bytes *b, lt_bytes *part) {
    const char *p = lt_find((const char *)b->data, b->len, (const char *)part->data, part->len);
    return p ? (int64_t)(p - (const char *)b->data) : -1;
}
static bool lt_bytes_eq(lt_bytes *a, lt_bytes *b) { return a == b || (a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0); }

static bool lt_utf8_valid(const unsigned char *s, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int k; // continuation bytes that follow
        if (c < 0x80) k = 0;
        else if ((c >> 5) == 6) k = 1;
        else if ((c >> 4) == 14) k = 2;
        else if ((c >> 3) == 30) k = 3;
        else return false;
        if (i + k >= n && k > 0) return false;
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return false;
        i += k + 1;
    }
    return true;
}

static lt_err lt_bytes_text(lt_bytes *b, lt_text **out) {
    if (!lt_utf8_valid(b->data, b->len)) return lt_make_failure(lt_text_cstr("the bytes are not valid UTF-8 text"));
    *out = lt_text_from((const char *)b->data, b->len);
    return (lt_err){ 0 };
}

static lt_text *lt_bytes_hex(lt_bytes *b) {
    static const char hx[] = "0123456789abcdef";
    lt_text *t = lt_text_new(b->len * 2);
    for (int64_t i = 0; i < b->len; i++) {
        t->data[2 * i] = hx[b->data[i] >> 4];
        t->data[2 * i + 1] = hx[b->data[i] & 15];
    }
    return t;
}

static const char lt_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static lt_text *lt_base64_encode(const unsigned char *s, int64_t n, bool url) {
    lt_text *t = lt_text_new(((n + 2) / 3) * 4);
    char *w = t->data;
    int64_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
        *w++ = lt_b64[v >> 18];
        *w++ = lt_b64[(v >> 12) & 63];
        *w++ = lt_b64[(v >> 6) & 63];
        *w++ = lt_b64[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0);
        *w++ = lt_b64[v >> 18];
        *w++ = lt_b64[(v >> 12) & 63];
        if (i + 1 < n) *w++ = lt_b64[(v >> 6) & 63];
        else if (!url) *w++ = '=';
        if (!url) *w++ = '=';
    }
    if (url) {
        for (char *c = t->data; c < w; c++) {
            if (*c == '+') *c = '-';
            else if (*c == '/') *c = '_';
        }
    }
    t->len = w - t->data;
    *w = 0;
    return t;
}
static lt_text *lt_bytes_base64(lt_bytes *b) { return lt_base64_encode(b->data, b->len, false); }

static int lt_b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// accepts standard and URL-safe alphabets, with or without padding
static lt_err lt_base64_decode(lt_text *t, lt_bytes **out) {
    lt_bytes *b = lt_bytes_new(t->len * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (int64_t i = 0; i < t->len; i++) {
        char c = t->data[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = lt_b64_val(c);
        if (v < 0) {
            lt_bytes_drop(b);
            return lt_make_failure(lt_text_cstr("the text is not valid base64"));
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            b->data[b->len++] = (unsigned char)(acc >> bits);
        }
    }
    *out = b;
    return (lt_err){ 0 };
}

static int lt_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static lt_err lt_hex_decode(lt_text *t, lt_bytes **out) {
    if (t->len % 2) return lt_make_failure(lt_text_cstr("hex text has an odd number of digits"));
    lt_bytes *b = lt_bytes_new(t->len / 2);
    for (int64_t i = 0; i < t->len; i += 2) {
        int h = lt_hexval(t->data[i]), l = lt_hexval(t->data[i + 1]);
        if (h < 0 || l < 0) {
            lt_bytes_drop(b);
            return lt_make_failure(lt_text_cstr("the text is not valid hex"));
        }
        b->data[b->len++] = (unsigned char)(h * 16 + l);
    }
    *out = b;
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- output

static void lt_print(lt_text *t) {
    flockfile(stdout);
    fwrite(t->data, 1, (size_t)t->len, stdout);
    putc_unlocked('\n', stdout);
    // the reader went away (`... | head`): stop quietly, as tools do
    if (ferror_unlocked(stdout) && errno == EPIPE) _exit(141);
    funlockfile(stdout);
}

LT_NOINLINE _Noreturn void lt_panic_text(lt_text *msg, int line) {
    char buf[1024];
    int64_t n = msg->len < (int64_t)sizeof buf - 1 ? msg->len : (int64_t)sizeof buf - 1;
    memcpy(buf, msg->data, (size_t)n);
    buf[n] = 0;
    lt_panic_at(buf, line);
}

LT_INLINE void lt_assert(bool c, int line) {
    if (LT_UNLIKELY(!c)) lt_panic_at("assertion failed", line);
}

static void lt_init(void) {
    // started by `lang run`: remove the temporary executable (still running)
    const char *self = getenv("LANG_RUN_EXE");
    if (self) {
        unlink(self);
        unsetenv("LANG_RUN_EXE");
    }
    static char buf[1 << 16];
    setvbuf(stdout, buf, _IOFBF, sizeof buf);
#ifdef LT_DEBUG_ALLOC
    atexit(lt_report_leaks);
#endif
}

// ---------------------------------------------------------------- tests

static lt_err lt_expect_failed(const char *text, int64_t line, lt_text *left, lt_text *right) {
    char head[512];
    snprintf(head, sizeof head, "expect failed at line %lld: %s", (long long)line, text);
    lt_text *h = lt_text_cstr(head);
    if (!left) return lt_make_failure(h);
    lt_text *l1 = lt_text_cstr("\n      left: ");
    lt_text *l2 = lt_text_cstr("\n     right: ");
    lt_text *parts[5] = { h, l1, left, l2, right };
    lt_text *msg = lt_text_concat_n(5, parts);
    lt_text_drop(h);
    lt_text_drop(l1);
    lt_text_drop(l2);
    return lt_make_failure(msg);
}

static lt_err lt_expect_throws_failed(const char *text, int64_t line) {
    char head[512];
    snprintf(head, sizeof head, "expect throws failed at line %lld: %s didn't fail", (long long)line, text);
    return lt_make_failure(lt_text_cstr(head));
}
