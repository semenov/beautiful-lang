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
// Dynamic values (from JSON text, environment variables, command-line
// arguments, database rows) and the JSON reader and writer. The compiler
// generates, for each type T, a decoder from lt_dyn and an encoder to text.

enum { LT_D_NULL, LT_D_BOOL, LT_D_NUM, LT_D_STR, LT_D_ARR, LT_D_OBJ };

typedef struct lt_dyn {
    int kind;
    bool b;
    bool is_int; // a number written without a fraction or exponent
    bool raw;    // strings: raw bytes (database blobs), not base64
    int64_t i;
    double num;
    const char *s; // strings: not zero-terminated
    int64_t slen;
    int64_t n;                // array / object size
    struct lt_dyn *items;     // array items / object values
    const char **keys;        // object keys
    int64_t *klens;
} lt_dyn;

// All nodes of one parse live in an arena freed at once.
typedef struct lt_arena {
    char **blocks;
    int nblocks, cap;
    char *cur, *end;
} lt_arena;

static void *lt_arena_alloc(lt_arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->cur || (size_t)(a->end - a->cur) < n) {
        size_t sz = n > 65536 ? n : 65536;
        char *b = (char *)malloc(sz);
        if (!b) lt_oom();
        if (a->nblocks == a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->blocks = (char **)realloc(a->blocks, sizeof(char *) * (size_t)a->cap);
        }
        a->blocks[a->nblocks++] = b;
        a->cur = b;
        a->end = b + sz;
    }
    void *p = a->cur;
    a->cur += n;
    return p;
}
static void lt_arena_free(lt_arena *a) {
    for (int i = 0; i < a->nblocks; i++) free(a->blocks[i]);
    free(a->blocks);
}

// ---------------------------------------------------------------- errors with a path

typedef struct lt_path {
    const struct lt_path *parent;
    const char *key; // field name, or NULL for an index
    int64_t klen;
    int64_t index;
} lt_path;

static void lt_path_write(const lt_path *p, char *buf, size_t cap, size_t *len) {
    if (!p) {
        *len += (size_t)snprintf(buf + *len, cap - *len, "$");
        return;
    }
    lt_path_write(p->parent, buf, cap, len);
    if (*len >= cap) return;
    if (p->key) *len += (size_t)snprintf(buf + *len, cap - *len, ".%.*s", (int)p->klen, p->key);
    else *len += (size_t)snprintf(buf + *len, cap - *len, "[%lld]", (long long)p->index);
}

static const char *lt_dyn_kind_name(const lt_dyn *d) {
    switch (d->kind) {
    case LT_D_NULL: return "null";
    case LT_D_BOOL: return d->b ? "true" : "false";
    case LT_D_NUM: return "a number";
    case LT_D_STR: return "text";
    case LT_D_ARR: return "a list";
    default: return "an object";
    }
}

// "<source>: at $.a.b: expected <what>, found <kind>"
static lt_err lt_dec_error(const char *source, const lt_path *p, const char *what, const lt_dyn *found) {
    char buf[1024];
    size_t len = (size_t)snprintf(buf, sizeof buf, "%s: at ", source);
    lt_path_write(p, buf, sizeof buf, &len);
    if (len < sizeof buf) {
        if (!found) snprintf(buf + len, sizeof buf - len, ": %s", what);
        else if (found->kind == LT_D_STR) snprintf(buf + len, sizeof buf - len, ": expected %s, found \"%.*s\"", what, (int)(found->slen > 60 ? 60 : found->slen), found->s);
        else if (found->kind == LT_D_NUM) snprintf(buf + len, sizeof buf - len, ": expected %s, found %g", what, found->num);
        else snprintf(buf + len, sizeof buf - len, ": expected %s, found %s", what, lt_dyn_kind_name(found));
    }
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_dec_missing(const char *src, const lt_path *p, const char *field) {
    char buf[512];
    if (strcmp(src, "env") == 0) {
        char up[128];
        size_t i = 0;
        for (; field[i] && i < sizeof up - 1; i++) up[i] = (char)((field[i] >= 'a' && field[i] <= 'z') ? field[i] - 32 : field[i]);
        up[i] = 0;
        snprintf(buf, sizeof buf, "the environment variable %s is not set", up);
    } else if (strcmp(src, "cli") == 0) {
        char fl[128];
        size_t i = 0;
        for (; field[i] && i < sizeof fl - 1; i++) fl[i] = field[i] == '_' ? '-' : field[i];
        fl[i] = 0;
        snprintf(buf, sizeof buf, "the option --%s is required (see --help)", fl);
    } else if (strcmp(src, "db") == 0) {
        snprintf(buf, sizeof buf, "db: the query result has no column `%s`", field);
    } else {
        size_t len = (size_t)snprintf(buf, sizeof buf, "%s: at ", src);
        lt_path_write(p, buf, sizeof buf, &len);
        if (len < sizeof buf) snprintf(buf + len, sizeof buf - len, ": the field `%s` is missing", field);
    }
    return lt_make_failure(lt_text_cstr(buf));
}

static const lt_dyn *lt_dyn_get(const lt_dyn *o, const char *key, int64_t klen) {
    for (int64_t i = 0; i < o->n; i++)
        if (o->klens[i] == klen && memcmp(o->keys[i], key, (size_t)klen) == 0) return &o->items[i];
    return NULL;
}

// ---- scalar decoders; `lenient`: text is accepted for numbers and booleans
// (environment variables and command-line arguments are always text)

static lt_err lt_dec_int(const char *src, const lt_dyn *d, const lt_path *p, int lenient, int64_t *out) {
    if (d->kind == LT_D_NUM && d->is_int) {
        *out = d->i;
        return (lt_err){ 0 };
    }
    if (d->kind == LT_D_NUM && d->num == (double)(int64_t)d->num && fabs(d->num) < 9e15) {
        *out = (int64_t)d->num;
        return (lt_err){ 0 };
    }
    if (lenient && d->kind == LT_D_STR) {
        lt_text *t = lt_text_from(d->s, d->slen);
        lt_err e = lt_text_to_int(t, out);
        lt_text_drop(t);
        if (!e.obj) return e;
        lt_iface_drop(e);
    }
    return lt_dec_error(src, p, "a whole number", d);
}
static lt_err lt_dec_float(const char *src, const lt_dyn *d, const lt_path *p, int lenient, double *out) {
    if (d->kind == LT_D_NUM) {
        *out = d->is_int ? (double)d->i : d->num;
        return (lt_err){ 0 };
    }
    if (lenient && d->kind == LT_D_STR) {
        lt_text *t = lt_text_from(d->s, d->slen);
        lt_err e = lt_text_to_float(t, out);
        lt_text_drop(t);
        if (!e.obj) return e;
        lt_iface_drop(e);
    }
    return lt_dec_error(src, p, "a number", d);
}
static lt_err lt_dec_bool(const char *src, const lt_dyn *d, const lt_path *p, int lenient, bool *out) {
    if (d->kind == LT_D_BOOL) {
        *out = d->b;
        return (lt_err){ 0 };
    }
    if (lenient && d->kind == LT_D_STR) {
        const char *yes[] = { "true", "1", "yes", "on" }, *no[] = { "false", "0", "no", "off", "" };
        for (int i = 0; i < 4; i++)
            if ((int64_t)strlen(yes[i]) == d->slen && strncasecmp(d->s, yes[i], (size_t)d->slen) == 0) return (*out = true), (lt_err){ 0 };
        for (int i = 0; i < 5; i++)
            if ((int64_t)strlen(no[i]) == d->slen && strncasecmp(d->s, no[i], (size_t)d->slen) == 0) return (*out = false), (lt_err){ 0 };
    }
    if (lenient && d->kind == LT_D_NUM && d->is_int && (d->i == 0 || d->i == 1)) {
        *out = d->i == 1;
        return (lt_err){ 0 };
    }
    return lt_dec_error(src, p, "true or false", d);
}
static lt_err lt_dec_text(const char *src, const lt_dyn *d, const lt_path *p, int lenient, lt_text **out) {
    if (d->kind == LT_D_STR) {
        *out = lt_text_from(d->s, d->slen);
        return (lt_err){ 0 };
    }
    (void)lenient;
    return lt_dec_error(src, p, "text", d);
}

// ---------------------------------------------------------------- JSON reader

typedef struct {
    const char *s, *end, *start;
    lt_arena *arena;
    const char *err;
    const char *err_at;
    int depth;
} lt_jp;

static void lt_jp_ws(lt_jp *p) {
    while (p->s < p->end && (*p->s == ' ' || *p->s == '\n' || *p->s == '\r' || *p->s == '\t')) p->s++;
}

static bool lt_jp_fail(lt_jp *p, const char *msg) {
    if (!p->err) {
        p->err = msg;
        p->err_at = p->s;
    }
    return false;
}

static void lt_utf8_put(char **w, uint32_t c) {
    char *o = *w;
    if (c < 0x80) *o++ = (char)c;
    else if (c < 0x800) { *o++ = (char)(0xC0 | (c >> 6)); *o++ = (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { *o++ = (char)(0xE0 | (c >> 12)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    else { *o++ = (char)(0xF0 | (c >> 18)); *o++ = (char)(0x80 | ((c >> 12) & 0x3F)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
    *w = o;
}

static int lt_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool lt_jp_string(lt_jp *p, const char **out, int64_t *len) {
    p->s++; // opening quote
    const char *q = p->s;
    bool plain = true;
    while (q < p->end && *q != '"') {
        if (*q == '\\') {
            plain = false;
            q++;
        } else if ((unsigned char)*q < 0x20) {
            p->s = q;
            return lt_jp_fail(p, "a control character inside a string");
        }
        q++;
    }
    if (q >= p->end) return lt_jp_fail(p, "the string is never closed");
    if (plain) {
        *out = p->s;
        *len = q - p->s;
        p->s = q + 1;
        return true;
    }
    char *buf = (char *)lt_arena_alloc(p->arena, (size_t)(q - p->s) + 4);
    char *w = buf;
    const char *r = p->s;
    while (r < q) {
        if (*r != '\\') {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '/': *w++ = '/'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            uint32_t c = 0;
            for (int k = 1; k <= 4; k++) {
                int h = r + k < q ? lt_hex(r[k]) : -1;
                if (h < 0) {
                    p->s = r;
                    return lt_jp_fail(p, "a bad \\u escape");
                }
                c = c * 16 + (uint32_t)h;
            }
            r += 4;
            if (c >= 0xD800 && c < 0xDC00 && r + 6 < q + 1 && r[1] == '\\' && r[2] == 'u') {
                uint32_t lo = 0;
                for (int k = 3; k <= 6; k++) lo = lo * 16 + (uint32_t)(lt_hex(r[k]) < 0 ? 0 : lt_hex(r[k]));
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                r += 6;
            }
            lt_utf8_put(&w, c);
            break;
        }
        default:
            p->s = r;
            return lt_jp_fail(p, "an unknown escape");
        }
        r++;
    }
    *out = buf;
    *len = w - buf;
    p->s = q + 1;
    return true;
}

static bool lt_jp_value(lt_jp *p, lt_dyn *d) {
    lt_jp_ws(p);
    memset(d, 0, sizeof *d);
    if (p->s >= p->end) return lt_jp_fail(p, "the text ends too early");
    if (++p->depth > 500) return lt_jp_fail(p, "too deeply nested");
    char c = *p->s;
    bool ok = true;
    if (c == '{') {
        p->s++;
        d->kind = LT_D_OBJ;
        int64_t cap = 0;
        lt_jp_ws(p);
        if (p->s < p->end && *p->s == '}') {
            p->s++;
        } else {
            for (;;) {
                lt_jp_ws(p);
                if (p->s >= p->end || *p->s != '"') { ok = lt_jp_fail(p, "expected a key in quotes"); break; }
                const char *k;
                int64_t kl;
                if (!lt_jp_string(p, &k, &kl)) { ok = false; break; }
                lt_jp_ws(p);
                if (p->s >= p->end || *p->s != ':') { ok = lt_jp_fail(p, "expected `:` after the key"); break; }
                p->s++;
                if (d->n == cap) {
                    int64_t nc = cap ? cap * 2 : 8;
                    lt_dyn *ni = (lt_dyn *)lt_arena_alloc(p->arena, sizeof(lt_dyn) * (size_t)nc);
                    const char **nk = (const char **)lt_arena_alloc(p->arena, sizeof(char *) * (size_t)nc);
                    int64_t *nl = (int64_t *)lt_arena_alloc(p->arena, sizeof(int64_t) * (size_t)nc);
                    if (d->n) {
                        memcpy(ni, d->items, sizeof(lt_dyn) * (size_t)d->n);
                        memcpy(nk, d->keys, sizeof(char *) * (size_t)d->n);
                        memcpy(nl, d->klens, sizeof(int64_t) * (size_t)d->n);
                    }
                    d->items = ni;
                    d->keys = nk;
                    d->klens = nl;
                    cap = nc;
                }
                d->keys[d->n] = k;
                d->klens[d->n] = kl;
                if (!lt_jp_value(p, &d->items[d->n])) { ok = false; break; }
                d->n++;
                lt_jp_ws(p);
                if (p->s < p->end && *p->s == ',') { p->s++; continue; }
                if (p->s < p->end && *p->s == '}') { p->s++; break; }
                ok = lt_jp_fail(p, "expected `,` or `}`");
                break;
            }
        }
    } else if (c == '[') {
        p->s++;
        d->kind = LT_D_ARR;
        int64_t cap = 0;
        lt_jp_ws(p);
        if (p->s < p->end && *p->s == ']') {
            p->s++;
        } else {
            for (;;) {
                if (d->n == cap) {
                    int64_t nc = cap ? cap * 2 : 8;
                    lt_dyn *ni = (lt_dyn *)lt_arena_alloc(p->arena, sizeof(lt_dyn) * (size_t)nc);
                    if (d->n) memcpy(ni, d->items, sizeof(lt_dyn) * (size_t)d->n);
                    d->items = ni;
                    cap = nc;
                }
                if (!lt_jp_value(p, &d->items[d->n])) { ok = false; break; }
                d->n++;
                lt_jp_ws(p);
                if (p->s < p->end && *p->s == ',') { p->s++; continue; }
                if (p->s < p->end && *p->s == ']') { p->s++; break; }
                ok = lt_jp_fail(p, "expected `,` or `]`");
                break;
            }
        }
    } else if (c == '"') {
        d->kind = LT_D_STR;
        ok = lt_jp_string(p, &d->s, &d->slen);
    } else if (c == 't' && p->end - p->s >= 4 && memcmp(p->s, "true", 4) == 0) {
        d->kind = LT_D_BOOL;
        d->b = true;
        p->s += 4;
    } else if (c == 'f' && p->end - p->s >= 5 && memcmp(p->s, "false", 5) == 0) {
        d->kind = LT_D_BOOL;
        p->s += 5;
    } else if (c == 'n' && p->end - p->s >= 4 && memcmp(p->s, "null", 4) == 0) {
        d->kind = LT_D_NULL;
        p->s += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *st = p->s;
        bool frac = false;
        if (*p->s == '-') p->s++;
        while (p->s < p->end && ((*p->s >= '0' && *p->s <= '9') || *p->s == '.' || *p->s == 'e' || *p->s == 'E' || *p->s == '+' || *p->s == '-')) {
            if (*p->s == '.' || *p->s == 'e' || *p->s == 'E') frac = true;
            p->s++;
        }
        char tmp[64];
        int64_t n = p->s - st;
        if (n >= 63) return lt_jp_fail(p, "the number is too long");
        memcpy(tmp, st, (size_t)n);
        tmp[n] = 0;
        char *e;
        d->kind = LT_D_NUM;
        d->num = strtod(tmp, &e);
        if (*e) {
            p->s = st;
            return lt_jp_fail(p, "a bad number");
        }
        if (!frac) {
            errno = 0;
            long long v = strtoll(tmp, &e, 10);
            if (!*e && errno == 0) {
                d->is_int = true;
                d->i = v;
            }
        }
    } else {
        ok = lt_jp_fail(p, "unexpected character");
    }
    p->depth--;
    return ok;
}

// Parses JSON text into *d (nodes in `arena`).
static lt_err lt_json_parse(lt_text *text, lt_arena *arena, lt_dyn *d) {
    lt_jp p = { text->data, text->data + text->len, text->data, arena, NULL, NULL, 0 };
    bool ok = lt_jp_value(&p, d);
    if (ok) {
        lt_jp_ws(&p);
        if (p.s < p.end) ok = lt_jp_fail(&p, "unexpected text after the value");
    }
    if (!ok) {
        int line = 1, col = 1;
        for (const char *c = p.start; c < p.err_at && c < p.end; c++) {
            if (*c == '\n') { line++; col = 1; } else col++;
        }
        char buf[256];
        snprintf(buf, sizeof buf, "json: %s at line %d, column %d", p.err, line, col);
        return lt_make_failure(lt_text_cstr(buf));
    }
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- JSON writer

typedef struct {
    char *d;
    int64_t len, cap;
    int indent; // < 0: compact
    int level;
} lt_buf;

static void lt_buf_grow(lt_buf *b, int64_t n) {
    if (b->len + n <= b->cap) return;
    int64_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n) nc *= 2;
    b->d = (char *)realloc(b->d, (size_t)nc);
    b->cap = nc;
}
static void lt_buf_put(lt_buf *b, const char *s, int64_t n) {
    lt_buf_grow(b, n);
    memcpy(b->d + b->len, s, (size_t)n);
    b->len += n;
}
static void lt_buf_c(lt_buf *b, char c) {
    lt_buf_grow(b, 1);
    b->d[b->len++] = c;
}
static void lt_buf_newline(lt_buf *b) {
    if (b->indent < 0) return;
    lt_buf_c(b, '\n');
    for (int i = 0; i < b->level * b->indent; i++) lt_buf_c(b, ' ');
}
static void lt_json_str(lt_buf *b, const char *s, int64_t n) {
    lt_buf_c(b, '"');
    for (int64_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': lt_buf_put(b, "\\\"", 2); break;
        case '\\': lt_buf_put(b, "\\\\", 2); break;
        case '\n': lt_buf_put(b, "\\n", 2); break;
        case '\r': lt_buf_put(b, "\\r", 2); break;
        case '\t': lt_buf_put(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char u[8];
                snprintf(u, sizeof u, "\\u%04x", c);
                lt_buf_put(b, u, 6);
            } else {
                lt_buf_c(b, (char)c);
            }
        }
    }
    lt_buf_c(b, '"');
}
static void lt_json_int(lt_buf *b, int64_t v) {
    char t[24];
    int n = snprintf(t, sizeof t, "%lld", (long long)v);
    lt_buf_put(b, t, n);
}
static void lt_json_float(lt_buf *b, double v) {
    if (!isfinite(v)) {
        lt_buf_put(b, "null", 4);
        return;
    }
    lt_text *t = lt_float_to_text(v);
    lt_buf_put(b, t->data, t->len);
    lt_text_drop(t);
}
// a number of unknown kind: whole numbers without a fraction
static void lt_json_num(lt_buf *b, double v) {
    if (isfinite(v) && v == floor(v) && fabs(v) < 9e15) lt_json_int(b, (int64_t)v);
    else lt_json_float(b, v);
}
static void lt_json_key(lt_buf *b, const char *k, bool first) {
    if (!first) lt_buf_c(b, ',');
    lt_buf_newline(b);
    lt_json_str(b, k, (int64_t)strlen(k));
    lt_buf_c(b, ':');
    if (b->indent >= 0) lt_buf_c(b, ' ');
}
static lt_text *lt_buf_text(lt_buf *b) {
    lt_text *t = lt_text_from(b->d ? b->d : "", b->len);
    free(b->d);
    return t;
}
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

// ---- walking directories and matching globs

// Every file under `dir` (recursively; symlinked directories aren't
// followed), as paths starting with `dir`, sorted.
static bool lt_walk_into(char *buf, size_t len, size_t cap, lt_texts **out, lt_text *root, lt_err *err) {
    DIR *d = opendir(buf);
    if (!d) {
        *err = lt_os_error("can't list", root);
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = strlen(e->d_name);
        if (len + n + 2 >= cap) continue;
        size_t at = len;
        if (len > 0 && buf[len - 1] != '/') buf[at++] = '/';
        memcpy(buf + at, e->d_name, n + 1);
        struct stat st;
        if (lstat(buf, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (!lt_walk_into(buf, at + n, cap, out, root, err)) {
                    closedir(d);
                    return false;
                }
            } else {
                // "./x" is shown as "x"
                const char *shown = (buf[0] == '.' && buf[1] == '/') ? buf + 2 : buf;
                lt_texts_push(out, lt_text_cstr(shown));
            }
        }
        buf[len] = 0;
    }
    closedir(d);
    return true;
}

static lt_err lt_files_walk(lt_text *dir, lt_texts **out) {
    char buf[4096];
    if (dir->len >= 4000) {
        errno = ENAMETOOLONG;
        return lt_os_error("can't list", dir);
    }
    memcpy(buf, dir->data, (size_t)dir->len + 1);
    lt_texts *l = lt_texts_new(16);
    lt_err e = { 0 };
    if (!lt_walk_into(buf, (size_t)dir->len, sizeof buf, &l, dir, &e)) {
        for (int64_t i = 0; i < l->len; i++) lt_text_drop(l->items[i]);
        if (l->cap) lt_free(l, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)l->cap);
        return e;
    }
    qsort(l->items, (size_t)l->len, sizeof(lt_text *), lt_cmp_texts);
    *out = l;
    return (lt_err){ 0 };
}

static bool lt_rm_tree(char *buf, size_t len, size_t cap) {
    struct stat st;
    if (lstat(buf, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(buf) == 0;
    DIR *d = opendir(buf);
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        size_t n = strlen(e->d_name);
        if (len + n + 2 >= cap) {
            errno = ENAMETOOLONG;
            ok = false;
            break;
        }
        buf[len] = '/';
        memcpy(buf + len + 1, e->d_name, n + 1);
        ok = lt_rm_tree(buf, len + 1 + n, cap);
        buf[len] = 0;
    }
    closedir(d);
    return ok && rmdir(buf) == 0;
}

// A file, or a directory with everything in it; nothing there is fine.
static lt_err lt_files_delete_all(lt_text *path) {
    char buf[4096];
    if (path->len >= 4000 || path->len == 0) {
        errno = path->len ? ENAMETOOLONG : EINVAL;
        return lt_os_error("can't delete", path);
    }
    memcpy(buf, path->data, (size_t)path->len + 1);
    size_t len = (size_t)path->len;
    while (len > 1 && buf[len - 1] == '/') buf[--len] = 0;
    if (!lt_rm_tree(buf, len, sizeof buf)) return lt_os_error("can't delete", path);
    return (lt_err){ 0 };
}

// Glob matching: `*` (within a path part), `?`, `**` (any number of
// parts), `[a-z]`, `[!a-z]`, `{a,b}`.
static bool lt_glob_simple(const char *p, const char *s, const char *start);

static bool lt_glob_class(const char **pp, char c) {
    const char *p = *pp + 1;
    bool neg = *p == '!' || *p == '^';
    if (neg) p++;
    bool hit = false;
    bool first = true;
    while (*p && (*p != ']' || first)) {
        first = false;
        char lo = *p, hi = *p;
        if (p[1] == '-' && p[2] && p[2] != ']') {
            hi = p[2];
            p += 2;
        }
        if (c >= lo && c <= hi) hit = true;
        p++;
    }
    if (*p == ']') p++;
    *pp = p;
    return hit != neg;
}

static bool lt_glob_simple(const char *p, const char *s, const char *start) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*' && (p == start || p[-1] == '/') && (p[2] == '/' || p[2] == 0)) {
            const char *rest = p[2] == '/' ? p + 3 : p + 2;
            if (!*rest) return true;
            for (const char *q = s;;) {
                if (lt_glob_simple(rest, q, start)) return true;
                q = strchr(q, '/');
                if (!q) return false;
                q++;
            }
        }
        if (*p == '*') {
            p++;
            for (const char *q = s;; q++) {
                if (lt_glob_simple(p, q, start)) return true;
                if (!*q || *q == '/') return false;
            }
        }
        if (!*s) return false;
        if (*p == '?') {
            if (*s == '/') return false;
        } else if (*p == '[') {
            if (*s == '/' || !lt_glob_class(&p, *s)) return false;
            s++;
            continue;
        } else if (*p != *s) {
            return false;
        }
        p++;
        s++;
    }
    return !*s;
}

// `{a,b}`: each alternative in turn
static bool lt_glob(const char *p, const char *s) {
    const char *open = strchr(p, '{');
    if (!open) return lt_glob_simple(p, s, p);
    int depth = 0;
    const char *close = NULL;
    for (const char *q = open; *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) {
            close = q;
            break;
        }
    }
    if (!close) return lt_glob_simple(p, s, p);
    size_t pre = (size_t)(open - p), post = strlen(close + 1);
    const char *alt = open + 1;
    for (;;) {
        const char *end = alt;
        int dd = 0;
        while (end < close && !(*end == ',' && dd == 0)) {
            if (*end == '{') dd++;
            if (*end == '}') dd--;
            end++;
        }
        size_t n = (size_t)(end - alt);
        char *buf = (char *)malloc(pre + n + post + 1);
        memcpy(buf, p, pre);
        memcpy(buf + pre, alt, n);
        memcpy(buf + pre + n, close + 1, post + 1);
        bool hit = lt_glob(buf, s);
        free(buf);
        if (hit) return true;
        if (end >= close) return false;
        alt = end + 1;
    }
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

// Runs a program to the end. `dir` ("" for here), `env` (name, value pairs
// added to this program's environment) and `input` (its standard input)
// are optional.
static lt_err lt_process_run_ex(lt_text *program, lt_texts *args, const char *dir, lt_texts *env, lt_text *input, int64_t *status, lt_text **out, lt_text **err) {
    int outp[2], errp[2], inp[2];
    if (pipe(outp) != 0) return lt_os_error("can't run", program);
    if (pipe(errp) != 0) {
        close(outp[0]);
        close(outp[1]);
        return lt_os_error("can't run", program);
    }
    if (pipe(inp) != 0) {
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        return lt_os_error("can't run", program);
    }
    fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    fcntl(errp[0], F_SETFD, FD_CLOEXEC);
    fcntl(inp[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errp[1], 2);
    if (dir && *dir) posix_spawn_file_actions_addchdir_np(&fa, dir);
    char **argv = (char **)calloc((size_t)args->len + 2, sizeof(char *));
    argv[0] = program->data;
    for (int64_t i = 0; i < args->len; i++) argv[i + 1] = args->items[i]->data;
    // the environment: ours, with `env` added or replaced
    char **envp = environ;
    int64_t extra = env ? env->len / 2 : 0;
    char **built = NULL;
    if (extra > 0) {
        int64_t n = 0;
        while (environ[n]) n++;
        built = (char **)calloc((size_t)(n + extra + 1), sizeof(char *));
        int64_t k = 0;
        for (int64_t i = 0; i < n; i++) {
            bool replaced = false;
            for (int64_t j = 0; j + 1 < env->len; j += 2) {
                lt_text *name = env->items[j];
                if (strncmp(environ[i], name->data, (size_t)name->len) == 0 && environ[i][name->len] == '=') replaced = true;
            }
            if (!replaced) built[k++] = strdup(environ[i]);
        }
        for (int64_t j = 0; j + 1 < env->len; j += 2) {
            size_t len = (size_t)(env->items[j]->len + env->items[j + 1]->len + 2);
            built[k] = (char *)malloc(len);
            snprintf(built[k], len, "%s=%s", env->items[j]->data, env->items[j + 1]->data);
            k++;
        }
        envp = built;
    }
    pid_t pid;
    fflush(stdout);
    int r = posix_spawnp(&pid, program->data, &fa, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    free(argv);
    if (built) {
        for (char **q = built; *q; q++) free(*q);
        free(built);
    }
    close(outp[1]);
    close(errp[1]);
    close(inp[0]);
    if (r != 0) {
        close(outp[0]);
        close(errp[0]);
        close(inp[1]);
        errno = r;
        return lt_os_error(dir && *dir && r == ENOENT ? "can't run (or no such directory)" : "can't run", program);
    }
    signal(SIGPIPE, SIG_IGN);
    // feed the input and read both outputs until they're closed
    size_t cap[2] = { 4096, 1024 }, len[2] = { 0, 0 };
    char *buf[2] = { (char *)malloc(cap[0]), (char *)malloc(cap[1]) };
    int64_t fed = 0, to_feed = input ? input->len : 0;
    if (to_feed == 0) {
        close(inp[1]);
        inp[1] = -1;
    } else {
        fcntl(inp[1], F_SETFL, fcntl(inp[1], F_GETFL, 0) | O_NONBLOCK);
    }
    struct pollfd fds[3] = { { outp[0], POLLIN, 0 }, { errp[0], POLLIN, 0 }, { inp[1], POLLOUT, 0 } };
    int open_fds = 2;
    while (open_fds > 0 || inp[1] >= 0) {
        if (poll(fds, 3, -1) < 0) {
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
        if (inp[1] >= 0 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t n = write(inp[1], input->data + fed, (size_t)(to_feed - fed));
            if (n > 0) fed += n;
            if (fed >= to_feed || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                close(inp[1]);
                inp[1] = -1;
                fds[2].fd = -1;
            }
        }
        if (open_fds == 0 && inp[1] >= 0) {
            // it stopped reading: the rest of the input can't go anywhere
            close(inp[1]);
            inp[1] = -1;
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

static lt_err lt_process_run(lt_text *program, lt_texts *args, int64_t *status, lt_text **out, lt_text **err) {
    return lt_process_run_ex(program, args, NULL, NULL, NULL, status, out, err);
}

// The full path of a program found on PATH, like `which`.
static lt_text *lt_process_find(lt_text *name) {
    if (strchr(name->data, '/')) return access(name->data, X_OK) == 0 ? (lt_text_dup(name), name) : NULL;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[4096];
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n == 0) n = 0;
        int w = snprintf(buf, sizeof buf, "%.*s/%s", (int)n, n ? path : ".", name->data);
        struct stat st;
        if (w > 0 && (size_t)w < sizeof buf && stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0) return lt_text_cstr(buf);
        if (!end) break;
        path = end + 1;
    }
    return NULL;
}

// ---------------------------------------------------------------- environment

static void lt_env_set(lt_text *name, lt_text *value) { setenv(name->data, value->data, 1); }

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

// LOG_LEVEL (debug, info, warn, error; default info) hides the levels
// below it; LOG_FORMAT=json writes one JSON object per line (for services
// whose logs are collected).
static int lt_log_rank(const char *level) {
    if (strcasecmp(level, "DEBUG") == 0) return 0;
    if (strcasecmp(level, "WARN") == 0) return 2;
    if (strcasecmp(level, "ERROR") == 0) return 3;
    return 1;
}

static void lt_log(const char *level, lt_text *msg) {
    static int min_rank = -1, json = -1;
    if (min_rank < 0) {
        const char *l = getenv("LOG_LEVEL");
        min_rank = l ? lt_log_rank(l) : 1;
        const char *f = getenv("LOG_FORMAT");
        json = f && strcasecmp(f, "json") == 0;
    }
    if (lt_log_rank(level) < min_rank) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t now = ts.tv_sec;
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[40];
    size_t sl = strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
    fflush(stdout);
    flockfile(stderr);
    if (json) {
        snprintf(stamp + sl, sizeof stamp - sl, ".%03ldZ", (long)(ts.tv_nsec / 1000000));
        lt_buf b = { 0 };
        lt_buf_put(&b, "{\"time\":\"", 9);
        lt_buf_put(&b, stamp, (int64_t)strlen(stamp));
        lt_buf_put(&b, "\",\"level\":\"", 11);
        for (const char *c = level; *c; c++) lt_buf_c(&b, (char)tolower((unsigned char)*c));
        lt_buf_put(&b, "\",\"message\":", 12);
        lt_json_str(&b, msg->data, msg->len);
        lt_buf_put(&b, "}\n", 2);
        fwrite(b.d, 1, (size_t)b.len, stderr);
        free(b.d);
    } else {
        fprintf(stderr, "%sZ %s %.*s\n", stamp, level, (int)msg->len, msg->data);
    }
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

// Time-ordered: the first 48 bits are the milliseconds since 1970, so
// ids made later sort later (good database keys).
static lt_text *lt_random_uuid_v7(void) {
    unsigned char u[16];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    for (int i = 0; i < 6; i++) u[i] = (unsigned char)(ms >> (40 - 8 * i));
    uint64_t a = lt_random_u64(), b = lt_random_u64();
    memcpy(u + 6, &a, 2);
    memcpy(u + 8, &b, 8);
    u[6] = (unsigned char)((u[6] & 0x0f) | 0x70);
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
#include <sys/uio.h>
#if defined(__APPLE__)
#include <sys/uio.h>
#else
#include <sys/sendfile.h>
#endif

#ifndef LT_THREADS
// without tasks, waiting on a descriptor simply blocks the program
static bool lt_io_wait(int fd, bool write) {
    struct pollfd p = { fd, (short)(write ? POLLOUT : POLLIN), 0 };
    while (poll(&p, 1, -1) < 0 && errno == EINTR) {
    }
    return true;
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
            if (!lt_io_wait(fd, true)) {
                errno = ECANCELED;
                return false;
            }
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
    bool chunked;   // an HTTP response body: each write is a chunk
    bool gone;      // a write found the other side gone (not worth logging)
    // http.open: checks at the end that the whole body arrived
    lt_err (*on_eof)(struct lt_conn *);
    void *owner;
    char peer[64];
    lt_text *path; // files and programs: for messages
    const char *label; // standard streams: for messages
    // a transport between the program and the descriptor: TLS
    // (net.connect_tls) or compression (zlib.open_gzip); `tls` is its state
    void *tls;
    ssize_t (*tls_recv)(struct lt_conn *, void *, size_t, lt_err *);
    bool (*tls_send)(struct lt_conn *, const void *, size_t, lt_err *);
    void (*tls_free)(struct lt_conn *);
    // writes what the transport still holds (the end of a gzip file)
    lt_err (*finish)(struct lt_conn *);
} lt_conn;

#define LT_WBUF (64 * 1024)

static lt_err lt_net_error(const char *what, const char *detail) {
    char buf[512];
    snprintf(buf, sizeof buf, "net: %s: %s", what, detail);
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_conn_error(lt_conn *c, const char *what, const char *detail) {
    if (errno == EPIPE || errno == ECONNRESET) c->gone = true;
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
    if (c->fd >= 0 && c->finish) {
        lt_err f = c->finish(c);
        if (f.obj) lt_iface_drop(f);
    }
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
            if (c->on_eof) {
                lt_err e = c->on_eof(c);
                c->on_eof = NULL;
                return e;
            }
            return (lt_err){ 0 };
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!lt_io_wait(c->fd, false)) return lt_make_cancelled();
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
    if (c->chunked) {
        if (n == 0) return (lt_err){ 0 }; // an empty chunk would end the body
        char head[24];
        int hl = snprintf(head, sizeof head, "%llx\r\n", (unsigned long long)n);
        struct iovec v[3] = { { head, (size_t)hl }, { (void *)d, (size_t)n }, { "\r\n", 2 } };
        size_t total = (size_t)hl + (size_t)n + 2;
        ssize_t w = writev(c->fd, v, 3);
        if (w == (ssize_t)total) return (lt_err){ 0 };
        // partly written (a full socket buffer): the rest in order
        size_t done = w > 0 ? (size_t)w : 0;
        for (int i = 0; i < 3; i++) {
            if (done >= v[i].iov_len) {
                done -= v[i].iov_len;
                continue;
            }
            if (!lt_sock_write_all(c->fd, (const char *)v[i].iov_base + done, v[i].iov_len - done)) return lt_conn_error(c, "write", strerror(errno));
            done = 0;
        }
        return (lt_err){ 0 };
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
    if (c->finish) {
        lt_err f = c->finish(c);
        c->finish = NULL;
        if (!e.obj) e = f;
        else if (f.obj) lt_iface_drop(f);
    }
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
            if (!lt_io_wait(sock, true)) {
                errno = ECANCELED;
                break;
            }
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

// ---- trusted certificates (Linux): where each distribution keeps them.
// A static program can't rely on its build machine's paths.

#if !defined(__APPLE__)
static const char *lt_ca_file(void) {
    const char *env = getenv("SSL_CERT_FILE");
    if (env && *env) return env;
    static const char *files[] = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Alpine, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                            // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7
        "/etc/ssl/cert.pem",                                 // Alpine, others
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        if (access(files[i], R_OK) == 0) return files[i];
    return NULL;
}
#define LT_NO_CA "no trusted certificates on this system (install the ca-certificates package, or set SSL_CERT_FILE)"
#endif


// ---- the terminal

#include <termios.h>

static int lt_color_on = -1;

// Colors only for a terminal, and never with NO_COLOR; FORCE_COLOR forces them.
static bool lt_term_colors(void) {
    if (lt_color_on < 0) {
        const char *force = getenv("FORCE_COLOR");
        const char *no = getenv("NO_COLOR");
        const char *term = getenv("TERM");
        if (force && *force && strcmp(force, "0") != 0) lt_color_on = 1;
        else if (no && *no) lt_color_on = 0;
        else lt_color_on = isatty(1) && !(term && strcmp(term, "dumb") == 0);
    }
    return lt_color_on;
}

static lt_text *lt_term_style(lt_text *t, const char *on, const char *off) {
    if (!lt_term_colors()) {
        lt_text_dup(t);
        return t;
    }
    size_t a = strlen(on), b = strlen(off);
    lt_text *r = lt_text_new(t->len + (int64_t)(a + b));
    memcpy(r->data, on, a);
    memcpy(r->data + a, t->data, (size_t)t->len);
    memcpy(r->data + a + (size_t)t->len, off, b);
    r->data[r->len] = 0;
    return r;
}

// Without ANSI escape codes.
static lt_text *lt_term_strip(lt_text *t) {
    lt_text *r = lt_text_new(t->len);
    int64_t w = 0;
    for (int64_t i = 0; i < t->len; i++) {
        if (t->data[i] == 0x1b && i + 1 < t->len && t->data[i + 1] == '[') {
            i += 2;
            while (i < t->len && !(t->data[i] >= 0x40 && t->data[i] <= 0x7e)) i++;
            continue;
        }
        r->data[w++] = t->data[i];
    }
    r->len = w;
    r->data[w] = 0;
    return r;
}

// Columns a code point takes: 0 for combining marks, 2 for wide (CJK,
// emoji), 1 otherwise.
static int lt_cp_width(uint32_t c) {
    if (c == 0 || (c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) || c == 0xfe0f || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x20d0 && c <= 0x20ff)) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
        (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) || (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x1f680 && c <= 0x1f6ff) ||
        (c >= 0x2600 && c <= 0x27bf && c >= 0x2614) || (c >= 0x20000 && c <= 0x3fffd))
        return 2;
    return 1;
}

static int64_t lt_term_width(lt_text *t) {
    int64_t w = 0;
    const unsigned char *s = (const unsigned char *)t->data, *e = s + t->len;
    while (s < e) {
        if (*s == 0x1b && s + 1 < e && s[1] == '[') {
            s += 2;
            while (s < e && !(*s >= 0x40 && *s <= 0x7e)) s++;
            if (s < e) s++;
            continue;
        }
        uint32_t c = *s;
        int n = 1;
        if (c >= 0xf0) c &= 0x07, n = 4;
        else if (c >= 0xe0) c &= 0x0f, n = 3;
        else if (c >= 0xc0) c &= 0x1f, n = 2;
        for (int i = 1; i < n && s + i < e; i++) c = (c << 6) | (s[i] & 0x3f);
        s += n;
        w += lt_cp_width(c);
    }
    return w;
}

static void lt_term_prompt(lt_text *q) {
    fflush(stdout);
    fwrite(q->data, 1, (size_t)q->len, stdout);
    if (q->len && q->data[q->len - 1] != ' ') fputc(' ', stdout);
    fflush(stdout);
}

// Asks and returns the answer (without the line break); an error at the end
// of the input.
static lt_err lt_term_ask(lt_text *q, lt_text **out) {
    lt_term_prompt(q);
    lt_text *line = NULL;
    lt_err e = lt_io_read_line(&line);
    if (e.obj) return e;
    if (!line) return lt_make_failure(lt_text_cstr("term: no answer (the input ended)"));
    *out = line;
    return (lt_err){ 0 };
}

// The same, without showing what's typed (passwords).
static lt_err lt_term_secret(lt_text *q, lt_text **out) {
    struct termios old, quiet;
    bool tty = isatty(0) && tcgetattr(0, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSAFLUSH, &quiet);
    }
    lt_err e = lt_term_ask(q, out);
    if (tty) {
        tcsetattr(0, TCSAFLUSH, &old);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return e;
}

// A progress line on standard error, redrawn in place (only on a terminal).
static void lt_term_progress(int64_t done, int64_t total, lt_text *label) {
    if (!isatty(2)) return;
    int width = 30;
    double f = total > 0 ? (double)done / (double)total : 0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int full = (int)(f * width + 0.5);
    char bar[64];
    for (int i = 0; i < width; i++) bar[i] = i < full ? '#' : '.';
    bar[width] = 0;
    fflush(stdout);
    fprintf(stderr, "\r[%s] %3d%% %.*s\x1b[K", bar, (int)(f * 100 + 0.5), (int)label->len, label->data);
    if (done >= total) fputc('\n', stderr);
    fflush(stderr);
}

// ---- generated ----
static void lt_index_panic(int64_t i, int64_t n, int line) { char b[128]; snprintf(b, sizeof b, "index %lld is out of range for a list of length %lld", (long long)i, (long long)n); lt_panic_at(b, line); }
static void lt_panic_error(lt_err e, int line);
typedef lt_texts *T8;
typedef struct { lt_text* f0; lt_iface f1; } T3; // Failure
typedef struct { char _unused; } T6; // Cancelled
typedef struct { char _unused; } T7; // ChannelClosed
#define T8_SIZE(cap) (sizeof(lt_texts) + sizeof(lt_text*) * (size_t)(cap))
static struct { int64_t rc; int64_t len; char data[2]; } lit0 = { -1, 1, "x" };
static struct { int64_t rc; int64_t len; char data[3]; } lit1 = { -1, 2, ": " };
static struct { int64_t rc; int64_t len; char data[23]; } lit2 = { -1, 22, "the task was cancelled" };
static struct { int64_t rc; int64_t len; char data[22]; } lit3 = { -1, 21, "the channel is closed" };
static struct { int64_t rc; int64_t len; char data[15]; } lit4 = { -1, 14, "ChannelClosed(" };
static struct { int64_t rc; int64_t len; char data[2]; } lit5 = { -1, 1, ")" };
static struct { int64_t rc; int64_t len; char data[11]; } lit6 = { -1, 10, "Cancelled(" };
static struct { int64_t rc; int64_t len; char data[9]; } lit7 = { -1, 8, "Failure(" };
static struct { int64_t rc; int64_t len; char data[10]; } lit8 = { -1, 9, "message: " };
static struct { int64_t rc; int64_t len; char data[10]; } lit9 = { -1, 9, ", cause: " };
static struct { int64_t rc; int64_t len; char data[5]; } lit10 = { -1, 4, "none" };
static void f0_main(void);
static lt_text* f1_Failure_message(T3 l0);
static lt_text* f2_Cancelled_message(T6 l0);
static lt_text* f3_ChannelClosed_message(T7 l0);
static lt_iface to_iface_vt0(T3 v);
static lt_iface to_iface_vt1(T6 v);
static lt_iface to_iface_vt2(T7 v);
static T8 T8_new(int64_t cap);
static void T8_free(T8 l);
static inline void T8_push(T8 *p, lt_text* v);
static T8 T8_lit(int64_t n, lt_text* *items);
static inline void dup_8(T8 x);
static inline void drop_8(T8 x);
static lt_text *totext_7(T7 a, bool debug);
static uint64_t hash_7(T7 a);
static bool eq_7(T7 a, T7 b);
static lt_text *totext_6(T6 a, bool debug);
static uint64_t hash_6(T6 a);
static bool eq_6(T6 a, T6 b);
static lt_text *totext_3(T3 a, bool debug);
static lt_text *totext_4(lt_iface a, bool debug);
static lt_text *totext_0(lt_iface a, bool debug);
static uint64_t hash_3(T3 a);
static uint64_t hash_4(lt_iface a);
static uint64_t hash_0(lt_iface a);
static bool eq_3(T3 a, T3 b);
static bool eq_4(lt_iface a, lt_iface b);
static bool eq_0(lt_iface a, lt_iface b);
static inline void dup_3(T3 x);
static inline void dup_0(lt_iface x);
static inline void drop_4(lt_iface x);
static inline void drop_3(T3 x);
static inline void dup_2(lt_text* x);
static inline void dup_4(lt_iface x);
static inline void drop_2(lt_text* x);
static inline void drop_0(lt_iface x);
static T8 T8_new(int64_t cap);
static void T8_free(T8 l);
static T8 T8_clone(T8 l, int64_t cap);
static void T8_grow(T8 *p, int64_t need);
static inline void T8_unique(T8 *p);
static inline void T8_push(T8 *p, lt_text* v);
static T8 T8_lit(int64_t n, lt_text* *items);
static inline lt_text* *T8_at(T8 l, int64_t i, int line);
static inline lt_text* T8_get(T8 l, int64_t i, int line);
static inline lt_text* T8_get_unchecked(T8 l, int64_t i);
static T8 T8_slice(T8 l, int64_t from, int64_t to);
static void T8_append_all(T8 *p, T8 o);
static void T8_insert(T8 *p, lt_text* v, int64_t at, int line);
static void T8_remove_at(T8 *p, int64_t at, int line);
static lt_text* T8_pop(T8 *p);
static void T8_reverse(T8 *p);
static void T8_clear(T8 *p);
static inline int64_t T8_cmp(lt_text* a, lt_text* b);
static void T8_msort(lt_text* *a, int64_t n, lt_text* *tmp);
static void T8_sort(T8 *p);
static lt_text* T8_minmax(T8 l, int sign);
static inline void dup_8(T8 x);
static inline void drop_8(T8 x);
static lt_text *totext_7(T7 a, bool debug);
static uint64_t hash_7(T7 a);
static bool eq_7(T7 a, T7 b);
static lt_text *totext_6(T6 a, bool debug);
static uint64_t hash_6(T6 a);
static bool eq_6(T6 a, T6 b);
static lt_text *totext_3(T3 a, bool debug);
static lt_text *totext_4(lt_iface a, bool debug);
static lt_text *totext_0(lt_iface a, bool debug);
static uint64_t hash_3(T3 a);
static uint64_t hash_4(lt_iface a);
static uint64_t hash_0(lt_iface a);
static bool eq_3(T3 a, T3 b);
static bool eq_4(lt_iface a, lt_iface b);
static bool eq_0(lt_iface a, lt_iface b);
static inline void dup_3(T3 x);
static inline void dup_0(lt_iface x);
static inline void drop_4(lt_iface x);
static inline void drop_3(T3 x);
static inline void dup_2(lt_text* x);
static inline void dup_4(lt_iface x);
static inline void drop_2(lt_text* x);
static inline void drop_0(lt_iface x);
static void B3_drop(lt_obj *o);
static bool B3_eq(lt_obj *a, lt_obj *b);
static uint64_t B3_hash(lt_obj *a);
static lt_text *B3_totext(lt_obj *a, bool debug);
static lt_obj *B3_clone(lt_obj *o);
static lt_text* vt0_m0(lt_obj *o);
static lt_iface to_iface_vt0(T3 v);
static void B6_drop(lt_obj *o);
static bool B6_eq(lt_obj *a, lt_obj *b);
static uint64_t B6_hash(lt_obj *a);
static lt_text *B6_totext(lt_obj *a, bool debug);
static lt_obj *B6_clone(lt_obj *o);
static lt_text* vt1_m0(lt_obj *o);
static lt_iface to_iface_vt1(T6 v);
static void B7_drop(lt_obj *o);
static bool B7_eq(lt_obj *a, lt_obj *b);
static uint64_t B7_hash(lt_obj *a);
static lt_text *B7_totext(lt_obj *a, bool debug);
static lt_obj *B7_clone(lt_obj *o);
static lt_text* vt2_m0(lt_obj *o);
static lt_iface to_iface_vt2(T7 v);
typedef struct { int64_t rc; T3 v; } B3;
static void B3_drop(lt_obj *o) { B3 *b = (B3 *)o; drop_3(b->v); lt_free(b, sizeof(B3)); }
static bool B3_eq(lt_obj *a, lt_obj *b) { return eq_3(((B3*)a)->v, ((B3*)b)->v); }
static uint64_t B3_hash(lt_obj *a) { return hash_3(((B3*)a)->v); }
static lt_text *B3_totext(lt_obj *a, bool debug) { return debug ? totext_3(((B3*)a)->v, true) : totext_3(((B3*)a)->v, false); }
static lt_obj *B3_clone(lt_obj *o) { B3 *n = (B3 *)lt_alloc(sizeof(B3)); n->rc = 1; n->v = ((B3 *)o)->v; dup_3(n->v); return (lt_obj *)n; }
static lt_text* vt0_m0(lt_obj *o) { T3 self_ = ((B3 *)o)->v; dup_3(self_); return f1_Failure_message(self_); }
static struct { lt_vt h; void *m[1]; } vt0 = { { B3_drop, B3_eq, B3_hash, B3_totext, B3_clone, 3 }, { (void *)vt0_m0 } };
static lt_iface to_iface_vt0(T3 v) { B3 *b = (B3 *)lt_alloc(sizeof(B3)); b->rc = 1; b->v = v; return (lt_iface){ (lt_obj *)b, &vt0.h }; }
typedef struct { int64_t rc; T6 v; } B6;
static void B6_drop(lt_obj *o) { B6 *b = (B6 *)o;  lt_free(b, sizeof(B6)); }
static bool B6_eq(lt_obj *a, lt_obj *b) { return eq_6(((B6*)a)->v, ((B6*)b)->v); }
static uint64_t B6_hash(lt_obj *a) { return hash_6(((B6*)a)->v); }
static lt_text *B6_totext(lt_obj *a, bool debug) { return debug ? totext_6(((B6*)a)->v, true) : totext_6(((B6*)a)->v, false); }
static lt_obj *B6_clone(lt_obj *o) { B6 *n = (B6 *)lt_alloc(sizeof(B6)); n->rc = 1; n->v = ((B6 *)o)->v;  return (lt_obj *)n; }
static lt_text* vt1_m0(lt_obj *o) { T6 self_ = ((B6 *)o)->v;  return f2_Cancelled_message(self_); }
static struct { lt_vt h; void *m[1]; } vt1 = { { B6_drop, B6_eq, B6_hash, B6_totext, B6_clone, 6 }, { (void *)vt1_m0 } };
static lt_iface to_iface_vt1(T6 v) { B6 *b = (B6 *)lt_alloc(sizeof(B6)); b->rc = 1; b->v = v; return (lt_iface){ (lt_obj *)b, &vt1.h }; }
typedef struct { int64_t rc; T7 v; } B7;
static void B7_drop(lt_obj *o) { B7 *b = (B7 *)o;  lt_free(b, sizeof(B7)); }
static bool B7_eq(lt_obj *a, lt_obj *b) { return eq_7(((B7*)a)->v, ((B7*)b)->v); }
static uint64_t B7_hash(lt_obj *a) { return hash_7(((B7*)a)->v); }
static lt_text *B7_totext(lt_obj *a, bool debug) { return debug ? totext_7(((B7*)a)->v, true) : totext_7(((B7*)a)->v, false); }
static lt_obj *B7_clone(lt_obj *o) { B7 *n = (B7 *)lt_alloc(sizeof(B7)); n->rc = 1; n->v = ((B7 *)o)->v;  return (lt_obj *)n; }
static lt_text* vt2_m0(lt_obj *o) { T7 self_ = ((B7 *)o)->v;  return f3_ChannelClosed_message(self_); }
static struct { lt_vt h; void *m[1]; } vt2 = { { B7_drop, B7_eq, B7_hash, B7_totext, B7_clone, 7 }, { (void *)vt2_m0 } };
static lt_iface to_iface_vt2(T7 v) { B7 *b = (B7 *)lt_alloc(sizeof(B7)); b->rc = 1; b->v = v; return (lt_iface){ (lt_obj *)b, &vt2.h }; }
static lt_err lt_make_failure(lt_text *msg) { return to_iface_vt0((T3){ msg, (lt_iface){0} }); }
static lt_err lt_make_cancelled(void) { return to_iface_vt1((T6){0}); }
static lt_err lt_make_channel_closed(void) { return to_iface_vt2((T7){0}); }
static lt_text *lt_error_message(lt_err e) { return ((lt_text *(*)(lt_obj *))e.vt->m[0])(e.obj); }
static void lt_panic_error(lt_err e, int line) { lt_text *m = lt_error_message(e); lt_panic_text(m, line); }

static T8 T8_new(int64_t cap) { if (cap <= 0) return (T8)&lt_empty_list; T8 l = (T8)lt_alloc(T8_SIZE(cap)); l->rc = 1; l->len = 0; l->cap = cap; return l; }
static void T8_free(T8 l) { for (int64_t i = 0; i < l->len; i++) { drop_2(l->items[i]); } lt_free(l, T8_SIZE(l->cap)); }
static T8 T8_clone(T8 l, int64_t cap) { T8 n = T8_new(cap); memcpy(n->items, l->items, sizeof(lt_text*) * (size_t)l->len); n->len = l->len; for (int64_t i = 0; i < n->len; i++) { dup_2(n->items[i]); } return n; }
static void T8_grow(T8 *p, int64_t need) {
  T8 l = *p; int64_t nc = l->cap * 2; if (nc < need) nc = need; if (nc < 4) nc = 4;
  if (LT_UNIQUE(l)) { l = (T8)lt_realloc(l, T8_SIZE(l->cap), T8_SIZE(nc)); l->cap = nc; *p = l; }
  else { T8 n = T8_clone(l, nc); drop_8(l); *p = n; }
}
static inline void T8_unique(T8 *p) { T8 l = *p; if (!LT_UNIQUE(l) && l->len > 0) { T8 n = T8_clone(l, l->len); drop_8(l); *p = n; } }
static inline void T8_push(T8 *p, lt_text* v) { T8 l = *p; if (LT_UNLIKELY(!LT_UNIQUE(l) || l->len == l->cap)) { T8_grow(p, l->len + 1); l = *p; } l->items[l->len++] = v; }
static T8 T8_lit(int64_t n, lt_text* *items) { T8 l = T8_new(n); memcpy(l->items, items, sizeof(lt_text*) * (size_t)n); l->len = n; return l; }
static inline lt_text* *T8_at(T8 l, int64_t i, int line) { if (LT_UNLIKELY((uint64_t)i >= (uint64_t)l->len)) lt_index_panic(i, l->len, line); return &l->items[i]; }
static inline lt_text* T8_get(T8 l, int64_t i, int line) { lt_text* v = *T8_at(l, i, line); dup_2(v); return v; }
static inline lt_text* T8_get_unchecked(T8 l, int64_t i) { lt_text* v = l->items[i]; dup_2(v); return v; }
static T8 T8_slice(T8 l, int64_t from, int64_t to) { if (from < 0) from = 0; if (to > l->len) to = l->len; if (to <= from) return (T8)&lt_empty_list; T8 n = T8_new(to - from); memcpy(n->items, l->items + from, sizeof(lt_text*) * (size_t)(to - from)); n->len = to - from; for (int64_t i = 0; i < n->len; i++) { dup_2(n->items[i]); } return n; }
static void T8_append_all(T8 *p, T8 o) {
  if (o->len == 0) { drop_8(o); return; }
  T8 l = *p; if (!LT_UNIQUE(l) || l->len + o->len > l->cap) { T8_grow(p, l->len + o->len); l = *p; }
  memcpy(l->items + l->len, o->items, sizeof(lt_text*) * (size_t)o->len);
  if (LT_UNIQUE(o)) { l->len += o->len; lt_free(o, T8_SIZE(o->cap)); }
  else { int64_t start = l->len; l->len += o->len; for (int64_t k = start; k < l->len; k++) { lt_text* v = l->items[k]; dup_2(v); } drop_8(o); }
}
static void T8_insert(T8 *p, lt_text* v, int64_t at, int line) {
  T8 l = *p; if (at < 0 || at > l->len) lt_index_panic(at, l->len, line);
  if (!LT_UNIQUE(l) || l->len == l->cap) { T8_grow(p, l->len + 1); l = *p; }
  memmove(l->items + at + 1, l->items + at, sizeof(lt_text*) * (size_t)(l->len - at)); l->items[at] = v; l->len++;
}
static void T8_remove_at(T8 *p, int64_t at, int line) {
  T8_at(*p, at, line); T8_unique(p); T8 l = *p; int64_t i = at; drop_2(l->items[i]);
  memmove(l->items + at, l->items + at + 1, sizeof(lt_text*) * (size_t)(l->len - at - 1)); l->len--;
}
static lt_text* T8_pop(T8 *p) { if ((*p)->len == 0) return NULL; T8_unique(p); T8 l = *p; lt_text* v = l->items[--l->len]; return v; }
static void T8_reverse(T8 *p) { T8_unique(p); T8 l = *p; for (int64_t i = 0, j = l->len - 1; i < j; i++, j--) { lt_text* t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; } }
static void T8_clear(T8 *p) { T8 l = *p; if (LT_UNIQUE(l)) { for (int64_t i = 0; i < l->len; i++) { drop_2(l->items[i]); } l->len = 0; } else { drop_8(l); *p = (T8)&lt_empty_list; } }
static inline int64_t T8_cmp(lt_text* a, lt_text* b) { return lt_text_cmp(a, b); }
static void T8_msort(lt_text* *a, int64_t n, lt_text* *tmp) {
  if (n <= 16) { for (int64_t i = 1; i < n; i++) { lt_text* x = a[i]; int64_t j = i - 1; while (j >= 0 && T8_cmp(a[j], x) > 0) { a[j + 1] = a[j]; j--; } a[j + 1] = x; } return; }
  int64_t h = n / 2; T8_msort(a, h, tmp); T8_msort(a + h, n - h, tmp);
  if (T8_cmp(a[h - 1], a[h]) <= 0) return;
  memcpy(tmp, a, sizeof(lt_text*) * (size_t)h);
  int64_t i = 0, j = h, k = 0;
  while (i < h && j < n) { if (T8_cmp(a[j], tmp[i]) < 0) a[k++] = a[j++]; else a[k++] = tmp[i++]; }
  while (i < h) a[k++] = tmp[i++];
}
static void T8_sort(T8 *p) { T8_unique(p); T8 l = *p; if (l->len < 2) return; lt_text* *tmp = (lt_text* *)malloc(sizeof(lt_text*) * (size_t)(l->len / 2 + 1)); T8_msort(l->items, l->len, tmp); free(tmp); }
static lt_text* T8_minmax(T8 l, int sign) { if (l->len == 0) return NULL; lt_text* best = l->items[0]; for (int64_t i = 1; i < l->len; i++) if (lt_text_cmp(l->items[i], best) * sign < 0) best = l->items[i]; lt_text* v = best; dup_2(v); return v; }
static inline void dup_8(T8 x) { if (x) LT_INC(x); }
static inline void drop_8(T8 x) { if (x && LT_DEC_ZERO(x)) T8_free(x); }
static lt_text *totext_7(T7 a, bool debug) { lt_texts *p = lt_texts_new(8); (void)debug; lt_texts_push(&p, ((lt_text*)&lit4)); lt_texts_push(&p, ((lt_text*)&lit5)); lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_8(p); return r; }
static uint64_t hash_7(T7 a) { uint64_t h = 7; return h; }
static bool eq_7(T7 a, T7 b) { return true; }
static lt_text *totext_6(T6 a, bool debug) { lt_texts *p = lt_texts_new(8); (void)debug; lt_texts_push(&p, ((lt_text*)&lit6)); lt_texts_push(&p, ((lt_text*)&lit5)); lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_8(p); return r; }
static uint64_t hash_6(T6 a) { uint64_t h = 7; return h; }
static bool eq_6(T6 a, T6 b) { return true; }
static lt_text *totext_3(T3 a, bool debug) { lt_texts *p = lt_texts_new(8); (void)debug; lt_texts_push(&p, ((lt_text*)&lit7)); lt_texts_push(&p, ((lt_text*)&lit8)); lt_texts_push(&p, lt_text_quote(a.f0)); lt_texts_push(&p, ((lt_text*)&lit9)); lt_texts_push(&p, totext_4(a.f1, true)); lt_texts_push(&p, ((lt_text*)&lit5)); lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_8(p); return r; }
static lt_text *totext_4(lt_iface a, bool debug) { lt_texts *p = lt_texts_new(8); (void)debug; if (((a).obj != NULL)) lt_texts_push(&p, totext_0(a, debug)); else lt_texts_push(&p, ((lt_text*)&lit10)); lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_8(p); return r; }
static lt_text *totext_0(lt_iface a, bool debug) { lt_texts *p = lt_texts_new(8); (void)debug; lt_texts_push(&p, a.vt->to_text(a.obj, debug)); lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_8(p); return r; }
static uint64_t hash_3(T3 a) { uint64_t h = 7; h = lt_hash_combine(h, lt_text_hash(a.f0)); h = lt_hash_combine(h, hash_4(a.f1)); return h; }
static uint64_t hash_4(lt_iface a) { return ((a).obj != NULL) ? lt_hash_combine(1, hash_0(a)) : 0; }
static uint64_t hash_0(lt_iface a) { return a.vt->hash(a.obj); }
static bool eq_3(T3 a, T3 b) { if (!lt_text_eq(a.f0, b.f0)) return false; if (!eq_4(a.f1, b.f1)) return false; return true; }
static bool eq_4(lt_iface a, lt_iface b) { if (((a).obj != NULL) != ((b).obj != NULL)) return false; if (!((a).obj != NULL)) return true; return eq_0(a, b); }
static bool eq_0(lt_iface a, lt_iface b) { if (a.obj == b.obj) return true; if (a.vt->type_id != b.vt->type_id) return false; return a.vt->eq(a.obj, b.obj); }
static inline void dup_3(T3 x) { dup_2(x.f0);dup_4(x.f1); }
static inline void dup_0(lt_iface x) { lt_iface_dup(x); }
static inline void drop_4(lt_iface x) { drop_0(x); }
static inline void drop_3(T3 x) { drop_2(x.f0);drop_4(x.f1); }
static inline void dup_2(lt_text* x) { lt_text_dup(x); }
static inline void dup_4(lt_iface x) { dup_0(x); }
static inline void drop_2(lt_text* x) { lt_text_drop(x); }
static inline void drop_0(lt_iface x) { lt_iface_drop(x); }
static void f0_main(void) { // main
  lt_iface l0 = ((lt_iface){0});
  int64_t l1 = 0;
  lt_iface l2 = ((lt_iface){0});
  int64_t l3 = 0;
  int64_t l4 = 0;
  int64_t l5 = 0;
  lt_text* l6 = NULL;
  lt_text* l7 = NULL;
b0:;
  l1 = 0;
  l2 = lt_text_to_int(((lt_text*)&lit0), &l1);
  if (l2.obj) goto b2; else goto b3;
b1:;
  l4 = lt_process_exit(INT64_C(2));
  l3 = l4;
  goto b4;
b2:;
  l0 = l2;
  drop_0(l0);
  goto b1;
b3:;
  drop_0(l2);
  l3 = l1;
  goto b4;
b4:;
  l5 = l3;
  l6 = lt_int_to_text(l5);
  l7 = lt_text_ret(l6);
  drop_2(l6);
  lt_print(l7);
  drop_2(l7);
  return;
}
static lt_text* f1_Failure_message(T3 l0) { // Failure.message
  lt_iface l1 = ((lt_iface){0});
  bool l2 = false;
  lt_iface l3 = ((lt_iface){0});
  lt_iface l4 = ((lt_iface){0});
  lt_text* l5 = NULL;
  lt_text* l6 = NULL;
  lt_text* l7 = NULL;
  lt_text* l8 = NULL;
b0:;
  l1 = l0.f1;
  dup_4(l1);
  l2 = ((l1).obj != NULL);
  if (l2) goto b3; else goto b2;
b1:;
  l8 = l0.f0;
  dup_2(l8);
  drop_3(l0);
  return l8;
b2:;
  drop_4(l1);
  goto b1;
b3:;
  l3 = l1;
  dup_0(l3);
  drop_4(l1);
  l4 = l3;
  dup_0(l4);
  drop_0(l3);
  l5 = l0.f0;
  dup_2(l5);
  drop_3(l0);
  l6 = ((lt_text* (*)(lt_obj *))(l4.vt->m[0]))(l4.obj);
  drop_0(l4);
  l7 = lt_text_concat_n(3, (lt_text*[]){ l5, ((lt_text*)&lit1), l6 });
  drop_2(l5);
  drop_2(l6);
  return l7;
b4:;
  lt_panic_at("unreachable code was reached (a compiler bug)", 0);
}
static lt_text* f2_Cancelled_message(T6 l0) { // Cancelled.message
b0:;
  return ((lt_text*)&lit2);
}
static lt_text* f3_ChannelClosed_message(T7 l0) { // ChannelClosed.message
b0:;
  return ((lt_text*)&lit3);
}
static const char *lt_file_init = "bug1.lang";
int main(int argc, char **argv) {
  lt_argc = argc;
  lt_argv = argv;
  lt_init(); lt_file = lt_file_init;
  f0_main(); lt_err e = {0};
  if (e.obj) { fflush(stdout); lt_text *m = lt_error_message(e); fprintf(stderr, "error: %.*s\n", (int)m->len, m->data); lt_text_drop(m); lt_iface_drop(e); return 1; }
  fflush(stdout);
  return 0;
}
