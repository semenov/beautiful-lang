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
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <sys/resource.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LT_ASAN 1
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#endif
#if __has_feature(thread_sanitizer)
#define LT_TSAN 1
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif
#endif

#define LT_INLINE static inline __attribute__((always_inline))
#define LT_NOINLINE static __attribute__((noinline))
#define LT_LIKELY(x) __builtin_expect(!!(x), 1)
#define LT_UNLIKELY(x) __builtin_expect(!!(x), 0)

static const char *lt_file = "?";
// the program's other files: a line number's bits above 20 name one
static const char **lt_files;
static int lt_nfiles;
// The Plumb name of a function of the program, from its address (the
// program defines it; NULL for the runtime's own)
static const char *lt_fn_name(void *start);

// The Plumb functions on the call stack, innermost first: the frame
// pointers link the frames, and dladdr gives each return address's
// function. After the panic's message, so a broken chain costs nothing.
static void lt_print_stack(void) {
    void **fp = (void **)__builtin_frame_address(0);
    const char *last = NULL;
    for (int i = 0; i < 64 && fp; i++) {
        void **next = (void **)fp[0];
        void *ret = fp[1];
        Dl_info info;
        if (ret && dladdr(ret, &info) && info.dli_saddr) {
            // an optimized build may have put `main`'s code into C's main
            const char *n = info.dli_sname && strcmp(info.dli_sname, "main") == 0 ? "main" : lt_fn_name(info.dli_saddr);
            if (n && n != last) fprintf(stderr, "  in %s\n", n);
            if (n) last = n;
        }
        if (next <= fp || (char *)next - (char *)fp > (1 << 22) || ((uintptr_t)next & 7)) break;
        fp = next;
    }
}

// "file.plumb:12" for a line number (0: none)
static void lt_where(int line, char *buf, size_t n) {
    int f = line >> 20, l = line & 0xFFFFF;
    const char *name = f == 0 ? lt_file : (f <= lt_nfiles ? lt_files[f - 1] : "?");
    snprintf(buf, n, "%s:%d", name, l);
}

// ---------------------------------------------------------------- reference counts
// In a program that uses tasks, counters change atomically; otherwise with
// plain increments. Negative counts are immortal static objects.
#ifdef LT_THREADS
#define LT_INC(p) do { if (__atomic_load_n(&(p)->rc, __ATOMIC_RELAXED) > 0) __atomic_fetch_add(&(p)->rc, 1, __ATOMIC_RELAXED); } while (0)
#define LT_DEC_ZERO(p) (__atomic_load_n(&(p)->rc, __ATOMIC_RELAXED) > 0 && __atomic_sub_fetch(&(p)->rc, 1, __ATOMIC_ACQ_REL) == 0)
#define LT_UNIQUE(p) (__atomic_load_n(&(p)->rc, __ATOMIC_ACQUIRE) == 1)
#else
#define LT_INC(p) do { if ((p)->rc > 0) (p)->rc++; } while (0)
#define LT_DEC_ZERO(p) ((p)->rc > 0 && --(p)->rc == 0)
#define LT_UNIQUE(p) ((p)->rc == 1)
#endif

// ---------------------------------------------------------------- CPUs

// The CPUs this program may use: PLUMB_WORKERS if set; else the online
// CPUs, fewer if the process is pinned to some (taskset, docker
// --cpuset-cpus) or its cgroup has a CPU quota (docker --cpus, a
// Kubernetes limit), rounded up.
static int lt_ncpu(void) {
    static int cached;
    if (cached) return cached;
    const char *env = getenv("PLUMB_WORKERS");
    long n = env ? atol(env) : 0;
    if (n < 1) {
        n = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __linux__
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0 && CPU_COUNT(&set) < n) n = CPU_COUNT(&set);
        FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
        if (f) {
            char quota[32];
            long period;
            if (fscanf(f, "%31s %ld", quota, &period) == 2 && strcmp(quota, "max") != 0 && period > 0) {
                long q = (atol(quota) + period - 1) / period;
                if (q >= 1 && q < n) n = q;
            }
            fclose(f);
        }
#endif
    }
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    cached = (int)n;
    return cached;
}

// ---------------------------------------------------------------- spin locks

typedef struct { int v; } lt_spin;
LT_INLINE void lt_spin_lock(lt_spin *s) {
    while (__atomic_exchange_n(&s->v, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s->v, __ATOMIC_RELAXED)) {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#else
            __asm__ volatile("pause");
#endif
        }
    }
}
LT_INLINE void lt_spin_unlock(lt_spin *s) { __atomic_store_n(&s->v, 0, __ATOMIC_RELEASE); }

// ---------------------------------------------------------------- memory

// Small objects come from per-size free lists: a freed block goes on the list
// for its size class and is reused by the next allocation of that class.
#define LT_CLASSES 32
#define LT_CLASS_BYTES 16
typedef struct lt_free_node { struct lt_free_node *next; } lt_free_node;
// Freed blocks and the unused rest of the current arena chunk. With tasks
// there is one per OS thread.
typedef struct lt_heap {
    lt_free_node *lists[LT_CLASSES];
    char *cur, *end;
#ifdef LT_THREADS
    int32_t counts[LT_CLASSES];
#endif
} lt_heap;
#ifdef LT_THREADS
// A task can move to another OS thread at any wait, and C compilers may keep
// the address of a thread-local variable across such a point (they assume
// the thread never changes inside a function). So the heap is found from
// the thread register, read with an `asm volatile` the compiler must repeat:
// on macOS through the thread's specific-data slots (as pthread_getspecific
// does), on Linux at the heap's fixed offset from the thread pointer.
// Elsewhere, through a call the compiler can't see into.
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__x86_64__))
static pthread_key_t lt_heap_key;
static void lt_oom(void);
__attribute__((constructor)) static void lt_heap_key_make(void) { pthread_key_create(&lt_heap_key, NULL); }
LT_NOINLINE lt_heap *lt_heap_new_here(void) {
    lt_heap *h = (lt_heap *)calloc(1, sizeof(lt_heap));
    if (!h) lt_oom();
    pthread_setspecific(lt_heap_key, h);
    return h;
}
LT_INLINE lt_heap *lt_heap_here(void) {
    lt_heap *h;
#if defined(__aarch64__)
    uintptr_t tsd;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tsd));
    h = ((lt_heap **)(tsd & ~(uintptr_t)7))[lt_heap_key];
#else
    __asm__ volatile("movq %%gs:(,%1,8), %0" : "=r"(h) : "r"((uintptr_t)lt_heap_key));
#endif
    return LT_LIKELY(h != NULL) ? h : lt_heap_new_here();
}
#elif defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
static __thread lt_heap lt_thread_heap __attribute__((tls_model("local-exec")));
static intptr_t lt_heap_off;
LT_INLINE char *lt_thread_pointer(void) {
    char *tp;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
#else
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
#endif
    return tp;
}
// the offset is the same in every thread (the program's own TLS block)
__attribute__((constructor)) static void lt_heap_off_find(void) { lt_heap_off = (char *)&lt_thread_heap - lt_thread_pointer(); }
LT_INLINE lt_heap *lt_heap_here(void) { return (lt_heap *)(lt_thread_pointer() + lt_heap_off); }
#else
static __thread lt_heap lt_thread_heap;
__attribute__((noinline)) static lt_heap *lt_heap_here(void) {
    lt_heap *h = &lt_thread_heap;
    __asm__ volatile("" : "+r"(h) : : "memory");
    return h;
}
#endif
#else
static lt_heap lt_the_heap;
#define lt_heap_here() (&lt_the_heap)
#endif

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

LT_NOINLINE void *lt_arena_refill(lt_heap *h, size_t n) {
    size_t chunk = 1 << 20;
    char *p = (char *)malloc(chunk);
    if (!p) lt_oom();
    h->cur = p + n;
    h->end = p + chunk;
    return p;
}

#ifdef LT_DEBUG_ALLOC
static int64_t lt_live_objects, lt_total_objects;
// set by process.exit: whatever was alive then was never meant to be freed
static int lt_exited_early;
static void lt_report_leaks(void) {
    if (lt_exited_early) {
        fprintf(stderr, "debug: %lld allocations, exited with process.exit (no leak count)\n", (long long)lt_total_objects);
        return;
    }
    fprintf(stderr, "debug: %lld allocations, %lld not freed\n", (long long)lt_total_objects, (long long)lt_live_objects);
}
// The release allocator frees by the size it's given: a small size class,
// malloc, or a mapped big block. Freeing with a size of another kind than
// the block was allocated with (a text made big, then shortened in place)
// crashes there, so --debug keeps each block's size and checks.
static int64_t lt_size_kind(size_t n) {
    size_t c = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (c < LT_CLASSES) return c ? (int64_t)c : 1;
    return n >= LT_BIG ? -2 : -1;
}
LT_INLINE void *lt_alloc(size_t n) {
    __atomic_fetch_add(&lt_live_objects, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&lt_total_objects, 1, __ATOMIC_RELAXED);
    size_t *p = (size_t *)malloc(n + 16);
    if (!p) lt_oom();
    p[0] = n;
    return p + 2;
}
LT_INLINE void lt_free(void *p, size_t n) {
    __atomic_fetch_sub(&lt_live_objects, 1, __ATOMIC_RELAXED);
    size_t *h = (size_t *)p - 2;
    if (lt_size_kind(h[0]) != lt_size_kind(n)) {
        fprintf(stderr, "runtime bug: a block of %zu bytes is freed as %zu bytes (another size class)\n", h[0], n);
        abort();
    }
    free(h);
}
LT_INLINE void *lt_realloc(void *p, size_t old, size_t n) {
    size_t *h = (size_t *)p - 2;
    if (lt_size_kind(h[0]) != lt_size_kind(old)) {
        fprintf(stderr, "runtime bug: a block of %zu bytes is resized as %zu bytes (another size class)\n", h[0], old);
        abort();
    }
    size_t *q = (size_t *)realloc(h, n + 16);
    if (!q) lt_oom();
    q[0] = n;
    return q + 2;
}
#else
#ifdef LT_THREADS
// With tasks, a block may be freed on another thread than the one that
// allocated it, so a thread's free lists could grow without end (one task
// produces, another consumes). A list that gets too long moves a batch of
// blocks to a shared pool, where a thread whose list is empty takes them.
static struct { lt_spin lock; lt_free_node *batches; } lt_pool[LT_CLASSES];
// Blocks per batch: about 32 KB worth, at least 16. A batch's first block
// links to the next batch in its second word (every class is >= 16 bytes).
LT_INLINE int32_t lt_batch(size_t c) {
    size_t b = 32768 / (c * LT_CLASS_BYTES);
    return b < 16 ? 16 : (int32_t)b;
}
LT_NOINLINE void lt_pool_put(lt_heap *h, size_t c) {
    int32_t b = lt_batch(c);
    lt_free_node *first = h->lists[c], *last = first;
    for (int32_t i = 1; i < b; i++) last = last->next;
    h->lists[c] = last->next;
    h->counts[c] -= b;
    last->next = NULL;
    lt_spin_lock(&lt_pool[c].lock);
    ((lt_free_node **)first)[1] = lt_pool[c].batches;
    lt_pool[c].batches = first;
    lt_spin_unlock(&lt_pool[c].lock);
}
LT_NOINLINE void *lt_pool_take(lt_heap *h, size_t c) {
    lt_free_node *got = NULL;
    if (__atomic_load_n(&lt_pool[c].batches, __ATOMIC_RELAXED)) {
        lt_spin_lock(&lt_pool[c].lock);
        got = lt_pool[c].batches;
        if (got) lt_pool[c].batches = ((lt_free_node **)got)[1];
        lt_spin_unlock(&lt_pool[c].lock);
    }
    if (got) {
        h->lists[c] = got->next;
        h->counts[c] = lt_batch(c) - 1;
        return got;
    }
    size_t sz = c * LT_CLASS_BYTES;
    if (LT_LIKELY(h->cur + sz <= h->end)) {
        void *p = h->cur;
        h->cur += sz;
        return p;
    }
    return lt_arena_refill(h, sz);
}
#endif

LT_INLINE void *lt_alloc(size_t n) {
    size_t c = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (LT_LIKELY(c < LT_CLASSES)) {
        if (LT_UNLIKELY(c == 0)) c = 1;
        lt_heap *h = lt_heap_here();
        lt_free_node *f = h->lists[c];
        if (f) {
            h->lists[c] = f->next;
#ifdef LT_THREADS
            h->counts[c]--;
#endif
            return f;
        }
#ifdef LT_THREADS
        return lt_pool_take(h, c);
#else
        size_t sz = c * LT_CLASS_BYTES;
        if (LT_LIKELY(h->cur + sz <= h->end)) {
            void *p = h->cur;
            h->cur += sz;
            return p;
        }
        return lt_arena_refill(h, sz);
#endif
    }
    if (n >= LT_BIG) return lt_big_alloc(n);
    void *p = malloc(n);
    if (!p) lt_oom();
    return p;
}

LT_INLINE void lt_free(void *p, size_t n) {
    size_t c = (n + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (LT_LIKELY(c < LT_CLASSES)) {
        if (LT_UNLIKELY(c == 0)) c = 1;
        lt_heap *h = lt_heap_here();
        lt_free_node *f = (lt_free_node *)p;
        f->next = h->lists[c];
        h->lists[c] = f;
#ifdef LT_THREADS
        if (LT_UNLIKELY(++h->counts[c] >= 2 * lt_batch(c))) lt_pool_put(h, c);
#endif
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
    if (oc == 0) oc = 1;
    if (nc == 0) nc = 1;
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

// Byte buffers of the runtime that can grow big (request bodies, stream
// buffers): the size is kept in front, and big ones are mapped from the
// system and given back when freed (malloc keeps freed big blocks resident).
typedef struct { size_t size; size_t pad; } lt_rhdr;
static void *lt_rmalloc(size_t n) {
    size_t t = n + sizeof(lt_rhdr);
    lt_rhdr *h = t >= LT_BIG ? (lt_rhdr *)lt_big_map(t) : (lt_rhdr *)malloc(t);
    if (!h) lt_oom();
    h->size = n;
    return h + 1;
}
static void lt_rfree(void *p) {
    if (!p) return;
    lt_rhdr *h = (lt_rhdr *)p - 1;
    if (h->size + sizeof(lt_rhdr) >= LT_BIG) lt_big_free(h, h->size + sizeof(lt_rhdr));
    else free(h);
}
static void *lt_rrealloc(void *p, size_t n) {
    if (!p) return lt_rmalloc(n);
    lt_rhdr *h = (lt_rhdr *)p - 1;
    bool was_big = h->size + sizeof(lt_rhdr) >= LT_BIG, big = n + sizeof(lt_rhdr) >= LT_BIG;
    if (!was_big && !big) {
        h = (lt_rhdr *)realloc(h, n + sizeof(lt_rhdr));
        if (!h) lt_oom();
        h->size = n;
        return h + 1;
    }
    if (was_big && big) {
        h = (lt_rhdr *)lt_big_realloc(h, h->size + sizeof(lt_rhdr), n + sizeof(lt_rhdr));
        h->size = n;
        return h + 1;
    }
    void *q = lt_rmalloc(n);
    memcpy(q, p, h->size < n ? h->size : n);
    lt_rfree(p);
    return q;
}

// ---------------------------------------------------------------- panics

static void (*lt_panic_hook)(const char *msg, int line);

LT_NOINLINE _Noreturn void lt_panic_at(const char *msg, int line) {
    if (lt_panic_hook) lt_panic_hook(msg, line); // returns if it can't handle it
    fflush(stdout);
    if (line > 0) {
        char where[512];
        lt_where(line, where, sizeof where);
        fprintf(stderr, "panic: %s\n  at %s\n", msg, where);
    } else {
        fprintf(stderr, "panic: %s\n", msg);
    }
    lt_print_stack();
    exit(101);
}

// ---------------------------------------------------------------- core types

typedef struct lt_obj { int64_t rc; } lt_obj;

typedef struct lt_text {
    int64_t rc;
    int64_t len; // bytes, UTF-8
    // characters; -1 until counted (then chars == len means ASCII, where a
    // character's index is its byte offset)
    int64_t chars;
    // a hint for walking by character: the last (character index, byte
    // offset) pair found, as (index << 32 | offset); 0 (the start) until
    // then. One word, read and written with relaxed atomics, so threads
    // sharing the text always see a pair that belongs together.
    uint64_t at;
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
static lt_err lt_make_file_error(int kind, lt_text *path, lt_text *msg);
// around C calls that may block for long (SQLite, DNS): with tasks, other
// workers can take over (see sched.h)
#ifdef LT_THREADS
static void lt_block_enter(void);
static void lt_block_exit(void);
#else
#define lt_block_enter() ((void)0)
#define lt_block_exit() ((void)0)
#endif

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
    t->chars = -1;
    t->at = 0;
    t->data[len] = 0;
    return t;
}

// A text made with room to spare (lt_text_new(most)), then filled with `n`
// bytes: sets its length, moving it to a block of the right size if the
// size class changes (the allocator frees by the length's size class).
static lt_text *lt_text_shorten(lt_text *t, int64_t n) {
    size_t old = LT_TEXT_SIZE(t->len), neu = LT_TEXT_SIZE(n);
    if (n < t->len) t = (lt_text *)lt_realloc(t, old, neu);
    t->len = n;
    t->data[n] = 0;
    t->chars = -1;
    t->at = 0;
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

static struct { int64_t rc; int64_t len; int64_t chars; uint64_t at; char data[1]; } lt_empty_text_obj = { -1, 0, 0, 0, "" };
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

// The shortest decimal digits that read back as `v` (finite), as JavaScript
// writes numbers: plain from 1e-6 up to 1e21 (12345678901234567000),
// otherwise with an exponent (1.5e+21, 1e-7). Returns the length.
static int lt_float_js(double v, char *out) {
    char buf[40];
    int p = 1;
    for (; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        if (strtod(buf, NULL) == v) break;
    }
    // "-d.ddde+XX" -> sign, digits, exponent
    const char *q = buf;
    bool neg = *q == '-';
    if (neg) q++;
    char digits[24];
    int k = 0;
    for (; *q && *q != 'e'; q++)
        if (*q != '.') digits[k++] = *q;
    int e = atoi(q + 1);
    while (k > 1 && digits[k - 1] == '0') k--;
    int n = e + 1; // where the decimal point goes
    int w = 0;
    if (neg) out[w++] = '-';
    if (k == 1 && digits[0] == '0') {
        out[w++] = '0';
    } else if (k <= n && n <= 21) {
        for (int i = 0; i < k; i++) out[w++] = digits[i];
        for (int i = k; i < n; i++) out[w++] = '0';
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < n; i++) out[w++] = digits[i];
        out[w++] = '.';
        for (int i = n; i < k; i++) out[w++] = digits[i];
    } else if (-6 < n && n <= 0) {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = 0; i < -n; i++) out[w++] = '0';
        for (int i = 0; i < k; i++) out[w++] = digits[i];
    } else {
        out[w++] = digits[0];
        if (k > 1) {
            out[w++] = '.';
            for (int i = 1; i < k; i++) out[w++] = digits[i];
        }
        w += snprintf(out + w, 8, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
    }
    out[w] = 0;
    return w;
}

// A Float as text: like JavaScript, but a whole number keeps ".0" so it
// reads as a Float (1.0, 12345678901234567000.0).
static lt_text *lt_float_to_text(double v) {
    char buf[48];
    if (isnan(v)) return lt_text_cstr("NaN");
    if (isinf(v)) return lt_text_cstr(v > 0 ? "Infinity" : "-Infinity");
    int n = lt_float_js(v, buf);
    bool has_dot = false;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '.' || buf[i] == 'e') has_dot = true;
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

#if defined(__APPLE__)
#include <malloc/malloc.h>
#define LT_USABLE(p) malloc_size(p)
#else
#include <malloc.h>
#define LT_USABLE(p) malloc_usable_size(p)
#endif

// the bytes the block of text `t` has room for (`size`: what its length needs)
static size_t lt_text_room(lt_text *t, size_t size) {
#ifdef LT_DEBUG_ALLOC
    (void)size;
    return ((size_t *)t)[-2]; // the size it was allocated with
#else
    size_t c = (size + LT_CLASS_BYTES - 1) / LT_CLASS_BYTES;
    if (c < LT_CLASSES) return (c ? c : 1) * LT_CLASS_BYTES;
    if (size >= LT_BIG) return ((lt_big_hdr *)((char *)t - sizeof(lt_big_hdr)))->mapped - sizeof(lt_big_hdr);
    return LT_USABLE(t);
#endif
}

// `x = "${x}..."`: appends the parts to the text in *tp. A text nothing else
// holds grows in place, with room to spare (as a list does), so building
// text in a loop is linear; a shared one is copied.
static void lt_text_append_n(lt_text **tp, int n, lt_text **parts) {
    lt_text *t = *tp;
    int64_t add = 0;
    bool alias = false;
    for (int i = 0; i < n; i++) {
        add += parts[i]->len;
        alias |= parts[i] == t;
    }
    if (!LT_UNIQUE(t) || alias) {
        lt_text *all[n + 1];
        all[0] = t;
        for (int i = 0; i < n; i++) all[i + 1] = parts[i];
        *tp = lt_text_concat_n(n + 1, all);
        lt_text_drop(t);
        return;
    }
    if (add == 0) return;
    size_t old = LT_TEXT_SIZE(t->len), need = LT_TEXT_SIZE(t->len + add);
    if (need > lt_text_room(t, old)) {
        size_t want = need;
        // past the small size classes, grow by half again, short of a big
        // block (those keep room themselves; see lt_big_realloc)
        if (need >= LT_CLASSES * LT_CLASS_BYTES && need < LT_BIG) {
            want = need + need / 2;
            if (want >= LT_BIG) want = LT_BIG - 1;
        }
        t = (lt_text *)lt_realloc(t, old, want);
    }
    char *w = t->data + t->len;
    for (int i = 0; i < n; i++) {
        memcpy(w, parts[i]->data, (size_t)parts[i]->len);
        w += parts[i]->len;
    }
    t->len += add;
    *w = 0;
    t->chars = -1; // (the position hint in `at` is still right)
    *tp = t;
}

static int64_t lt_text_length(lt_text *t) {
    int64_t c = __atomic_load_n(&t->chars, __ATOMIC_RELAXED);
    if (c >= 0) return c;
    int64_t n = 0;
    for (int64_t i = 0; i < t->len; i++) {
        if (((unsigned char)t->data[i] & 0xC0) != 0x80) n++;
    }
    // shared texts are only read: every thread would store the same count
    if (t->rc >= 0) __atomic_store_n(&t->chars, n, __ATOMIC_RELAXED);
    return n;
}

// whether characters are bytes (ASCII): then indexes are byte offsets
static inline bool lt_text_ascii(lt_text *t) { return lt_text_length(t) == t->len; }

#define LT_IS_LEAD(c) (((unsigned char)(c) & 0xC0) != 0x80)

LT_INLINE void lt_text_at_load(lt_text *t, int64_t *ci, int64_t *bi) {
    uint64_t at = __atomic_load_n(&t->at, __ATOMIC_RELAXED);
    *ci = (int64_t)(at >> 32);
    *bi = (int64_t)(at & 0xFFFFFFFFu);
}
LT_INLINE void lt_text_at_store(lt_text *t, int64_t ci, int64_t bi) {
    if (bi > 0xFFFFFFFFll || ci > 0xFFFFFFFFll) return; // texts over 4 GB: no hint
    __atomic_store_n(&t->at, ((uint64_t)ci << 32) | (uint64_t)bi, __ATOMIC_RELAXED);
}

// byte offset of the character with index `ci` (clamped to the end).
// Walks from the remembered position (forward or back) or from the start,
// whichever is nearer, so a loop over the characters is linear.
static int64_t lt_text_char_offset(lt_text *t, int64_t ci) {
    if (ci <= 0) return 0;
    if (lt_text_ascii(t)) return ci < t->len ? ci : t->len;
    if (ci >= lt_text_length(t)) return t->len;
    int64_t c, b;
    lt_text_at_load(t, &c, &b);
    if (ci < c && ci < c - ci) c = b = 0; // nearer the start than the hint
    const char *d = t->data;
    if (ci >= c) {
        while (c < ci) { // forward: step over one character
            b++;
            while (b < t->len && !LT_IS_LEAD(d[b])) b++;
            c++;
        }
    } else {
        while (c > ci) { // back
            b--;
            while (b > 0 && !LT_IS_LEAD(d[b])) b--;
            c--;
        }
    }
    lt_text_at_store(t, ci, b);
    return b;
}

// the character index of byte offset `byte` (a character's first byte, or
// the end); uses the same hint
static int64_t lt_text_char_index(lt_text *t, int64_t byte) {
    if (byte <= 0) return 0;
    if (byte >= t->len) return lt_text_length(t);
    if (lt_text_ascii(t)) return byte;
    int64_t c, b;
    lt_text_at_load(t, &c, &b);
    if (byte < b && byte < b - byte) c = b = 0;
    const char *d = t->data;
    if (byte >= b) {
        for (; b < byte; b++) c += LT_IS_LEAD(d[b]);
    } else {
        for (b--; b >= byte; b--) c -= LT_IS_LEAD(d[b]);
    }
    if (LT_IS_LEAD(d[byte])) lt_text_at_store(t, c, byte); // only a valid pair
    return c;
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
    return lt_text_shorten(r, w - r->data);
}

// `"<t>" is not <what>` and, if there is one, `: <why>` (a long text
// shortened to its first 100 bytes and "...")
static lt_err lt_number_error(lt_text *t, const char *what, const char *why) {
    int64_t cut = t->len;
    if (cut > 100) {
        cut = 100;
        while (cut > 0 && !LT_IS_LEAD(t->data[cut])) cut--;
    }
    lt_text *shown = lt_text_from(t->data, cut);
    lt_text *q = lt_text_quote(shown);
    lt_text_drop(shown);
    char buf[160];
    snprintf(buf, sizeof buf, "%s is not %s%s%s", cut < t->len ? "..." : "", what, why ? ": " : "", why ? why : "");
    lt_text *suffix = lt_text_cstr(buf);
    lt_text *parts[2] = { q, suffix };
    lt_text *msg = lt_text_concat_n(2, parts);
    lt_text_drop(q);
    lt_text_drop(suffix);
    return lt_make_failure(msg);
}

// what's wrong with number text beyond its digits: spaces around it, `_`
static const char *lt_number_text_problem(lt_text *t) {
    if (t->len == 0) return "it's empty";
    if (lt_is_space(t->data[0]) || lt_is_space(t->data[t->len - 1])) return "it has spaces around it (trim it first)";
    if (memchr(t->data, '_', (size_t)t->len)) return "`_` isn't allowed in number text";
    return NULL;
}

// strict: an optional sign and digits, nothing else
static lt_err lt_text_to_int(lt_text *t, int64_t *out) {
    const char *why = lt_number_text_problem(t);
    if (why) return lt_number_error(t, "a whole number", why);
    int64_t i = 0, b = t->len;
    bool neg = false;
    if (t->data[i] == '-' || t->data[i] == '+') {
        neg = t->data[i] == '-';
        i++;
    }
    if (i == b) return lt_number_error(t, "a whole number", NULL);
    uint64_t v = 0;
    bool big = false;
    for (; i < b; i++) {
        char c = t->data[i];
        if (c < '0' || c > '9') return lt_number_error(t, "a whole number", NULL);
        if (v > (UINT64_MAX - 9) / 10) big = true;
        else v = v * 10 + (uint64_t)(c - '0');
    }
    if (big || (neg ? v > (uint64_t)INT64_MAX + 1 : v > (uint64_t)INT64_MAX))
        return lt_number_error(t, "a whole number", "it's too big for an Int");
    *out = neg ? (int64_t)(0 - v) : (int64_t)v;
    return (lt_err){ 0 };
}

// strict: [sign] digits [. digits] [e [sign] digits], as in JSON (and ".5",
// "5."); no NaN, infinity or hexadecimal, and not so big it's infinite
static lt_err lt_text_to_float(lt_text *t, double *out) {
    const char *why = lt_number_text_problem(t);
    if (why) return lt_number_error(t, "a number", why);
    const char *d = t->data;
    int64_t i = 0, n = t->len, digits = 0;
    if (d[i] == '-' || d[i] == '+') i++;
    if (i < n && (d[i] == 'n' || d[i] == 'N' || d[i] == 'i' || d[i] == 'I'))
        return lt_number_error(t, "a number", "NaN and infinity aren't accepted");
    while (i < n && d[i] >= '0' && d[i] <= '9') i++, digits++;
    if (i < n && d[i] == '.') {
        i++;
        while (i < n && d[i] >= '0' && d[i] <= '9') i++, digits++;
    }
    bool ok = digits > 0;
    if (ok && i < n && (d[i] == 'e' || d[i] == 'E')) {
        i++;
        if (i < n && (d[i] == '-' || d[i] == '+')) i++;
        int64_t ed = 0;
        while (i < n && d[i] >= '0' && d[i] <= '9') i++, ed++;
        ok = ed > 0;
    }
    if (!ok || i != n) return lt_number_error(t, "a number", NULL);
    double v = strtod(d, NULL);
    if (isinf(v)) return lt_number_error(t, "a number", "it's too big for a Float");
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
    // a shared value is copied with room for its length, not its capacity
    int64_t cap = (LT_UNIQUE(b) ? b->cap : b->len) * 2;
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

// The length of the valid UTF-8 sequence at s[0] (1-4), or 0 if there
// isn't one; `*bad` is then how many bytes to replace with one U+FFFD (the
// longest start of a valid sequence, at least 1). Overlong forms,
// surrogates and code points past U+10FFFF aren't valid.
static int lt_utf8_seq(const unsigned char *s, int64_t n, int *bad) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    int k;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) k = 1;
    else if (c >= 0xE0 && c <= 0xEF) {
        k = 2;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        k = 3;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        *bad = 1;
        return 0;
    }
    for (int j = 1; j <= k; j++) {
        unsigned char b = j < n ? s[j] : 0;
        bool ok = j == 1 ? (b >= lo && b <= hi) : (b & 0xC0) == 0x80;
        if (j >= n || !ok) {
            *bad = j;
            return 0;
        }
    }
    return k + 1;
}

static bool lt_utf8_valid(const unsigned char *s, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        // ASCII 8 bytes at a time
        while (i + 8 <= n) {
            uint64_t w;
            memcpy(&w, s + i, 8);
            if (w & 0x8080808080808080ull) break;
            i += 8;
        }
        if (i >= n) break;
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        int bad;
        int k = lt_utf8_seq(s + i, n - i, &bad);
        if (!k) return false;
        i += k;
    }
    return true;
}

// A String is always valid UTF-8: text from outside (files, streams, the
// network, the environment) comes through here, and each invalid sequence
// becomes U+FFFD. Takes `t` and returns it, or a repaired copy.
static lt_text *lt_text_valid(lt_text *t) {
    if (!t || lt_utf8_valid((const unsigned char *)t->data, t->len)) return t;
    const unsigned char *s = (const unsigned char *)t->data;
    int64_t n = t->len;
    lt_text *r = lt_text_new(n * 3);
    int64_t w = 0, i = 0;
    while (i < n) {
        int bad = 0;
        int k = lt_utf8_seq(s + i, n - i, &bad);
        if (k) {
            memcpy(r->data + w, s + i, (size_t)k);
            w += k;
            i += k;
        } else {
            memcpy(r->data + w, "\xEF\xBF\xBD", 3);
            w += 3;
            i += bad;
        }
    }
    r = lt_text_shorten(r, w);
    lt_text_drop(t);
    return r;
}

static lt_text *lt_text_from_input(const char *s, int64_t n) { return lt_text_valid(lt_text_from(s, n)); }

// Bytes.text_lossy
static lt_text *lt_bytes_text_lossy(lt_bytes *b) { return lt_text_from_input((const char *)b->data, b->len); }

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
    return lt_text_shorten(t, w - t->data);
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

// to standard error: messages for people (`eprint`), not program output
static void lt_eprint(lt_text *t) {
    fflush(stdout);
    flockfile(stderr);
    fwrite(t->data, 1, (size_t)t->len, stderr);
    putc_unlocked('\n', stderr);
    funlockfile(stderr);
}

static void lt_print(lt_text *t) {
    flockfile(stdout);
    fwrite(t->data, 1, (size_t)t->len, stdout);
    putc_unlocked('\n', stdout);
    // the reader went away (`... | head`): stop quietly, as tools do
    if (ferror_unlocked(stdout) && errno == EPIPE) _exit(141);
    funlockfile(stdout);
}

// io.write: no line break; shown at once on a terminal (a prompt, progress)
static void lt_write_out(lt_text *t) {
    static int tty = -1;
    if (tty < 0) tty = isatty(1);
    flockfile(stdout);
    fwrite(t->data, 1, (size_t)t->len, stdout);
    if (tty) fflush(stdout); // (the lock is recursive)
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

// Ctrl-C (SIGINT) and SIGTERM. With tasks: the first cancels main's task
// (waits stop with Cancelled, `with` blocks close); without tasks, only a
// program that checks process.interrupted() gets to finish its loop. The
// second signal ends the program at once. Exit status 130 either way.
static volatile sig_atomic_t lt_interrupted;
static int lt_interrupt_pipe[2] = { -1, -1 };

static void lt_on_interrupt(int sig) {
    (void)sig;
    if (lt_interrupted) _exit(130);
    lt_interrupted = 1;
    if (lt_interrupt_pipe[1] >= 0) {
        char x = 1;
        ssize_t r = write(lt_interrupt_pipe[1], &x, 1);
        (void)r;
    }
}

// A signal the parent set to be ignored stays ignored (background jobs,
// nohup), as Go does.
static void lt_catch_interrupts(void) {
    int sigs[2] = { SIGINT, SIGTERM };
    for (int i = 0; i < 2; i++) {
        struct sigaction old;
        if (sigaction(sigs[i], NULL, &old) == 0 && old.sa_handler == SIG_IGN) continue;
        signal(sigs[i], lt_on_interrupt);
    }
}

// A server's own SIGINT/SIGTERM handler while it runs; the previous ones
// come back afterwards. Ignored signals stay ignored.
static void lt_signals_take(void (*handler)(int), struct sigaction old[2]) {
    int sigs[2] = { SIGINT, SIGTERM };
    for (int i = 0; i < 2; i++) {
        sigaction(sigs[i], NULL, &old[i]);
        if (old[i].sa_handler == SIG_IGN) continue;
        signal(sigs[i], handler);
    }
}
static void lt_signals_restore(struct sigaction old[2]) {
    sigaction(SIGINT, &old[0], NULL);
    sigaction(SIGTERM, &old[1], NULL);
}

// ---------------------------------------------------------------- stack overflow
// A stack overflow runs into a guard page and faults. The handler runs on
// its own small stack (the thread's stack is full) and reports it.

static char *lt_main_stack_lo; // the lowest address of the main thread's stack
static bool (*lt_overflow_hook)(char *addr); // with tasks: in a task's guard?
static bool (*lt_grow_hook)(char *addr); // with tasks: open more of the stack?

static void lt_write_err(const char *s) {
    ssize_t r = write(2, s, strlen(s));
    (void)r;
}

static void lt_on_fault(int sig, siginfo_t *info, void *uc) {
    (void)uc;
    char *addr = (char *)info->si_addr;
    if (lt_grow_hook && lt_grow_hook(addr)) return; // a task's stack got deeper
    static int reported;
    if (__atomic_exchange_n(&reported, 1, __ATOMIC_ACQ_REL)) {
        for (;;) pause(); // another thread is reporting its fault and exiting
    }
    bool overflow = lt_overflow_hook && lt_overflow_hook(addr);
    // the main thread's stack: the fault is just below its lowest address
    if (!overflow && lt_main_stack_lo && addr < lt_main_stack_lo + 65536 && addr >= lt_main_stack_lo - ((size_t)4 << 20))
        overflow = true;
    if (ftrylockfile(stdout) == 0) {
        fflush(stdout);
        funlockfile(stdout);
    }
    if (overflow) {
        lt_write_err("panic: stack overflow: too many nested calls (most likely a recursion that doesn't stop)\n");
        _exit(101);
    }
    lt_write_err(sig == SIGBUS ? "panic: crashed (bus error)\n" : "panic: crashed (segmentation fault)\n");
    signal(sig, SIG_DFL); // returning faults again, now with the default action
}

// Every thread that runs the program's code needs one.
static void lt_alt_stack(void) {
#if !defined(LT_ASAN) && !defined(LT_TSAN)
    stack_t ss;
    ss.ss_size = 65536;
    ss.ss_sp = mmap(NULL, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    ss.ss_flags = 0;
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, NULL);
#endif
}

static void lt_catch_overflow(void) {
#if !defined(LT_ASAN) && !defined(LT_TSAN)
    struct rlimit rl;
    char here;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < ((rlim_t)1 << 40))
        lt_main_stack_lo = &here - rl.rlim_cur;
    lt_alt_stack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = lt_on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif
}

static void lt_init(void) {
    lt_catch_overflow();
    // started by `plumb run`: remove the temporary executable (still running)
    const char *self = getenv("PLUMB_RUN_EXE");
    if (self) {
        unlink(self);
        unsetenv("PLUMB_RUN_EXE");
    }
    static char buf[1 << 16];
    setvbuf(stdout, buf, _IOFBF, sizeof buf);
#if defined(LT_WANTS_INTERRUPT) && !defined(LT_THREADS)
    lt_catch_interrupts();
#endif
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

// ---------------------------------------------------------------- Decimal
// value = c / 10^s: exact decimal numbers for money. Up to 18 digits;
// going past that is a bug (like Int overflow). The scale is kept (1.50
// prints as 1.50), and equality ignores it (1.50 == 1.5).

typedef struct {
    int64_t c;
    int32_t s;
} lt_decimal;

static const int64_t lt_p10[19] = { 1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL, 100000000LL, 1000000000LL, 10000000000LL, 100000000000LL, 1000000000000LL, 10000000000000LL, 100000000000000LL, 1000000000000000LL, 10000000000000000LL, 100000000000000000LL, 1000000000000000000LL };

static void lt_decimal_overflow(int line) { lt_panic_at("Decimal overflow: more than 18 digits", line); }

// scale up to `s` (more digits after the point)
static int64_t lt_decimal_scaled(lt_decimal a, int32_t s, int line) {
    int64_t r;
    if (s - a.s > 18 || __builtin_mul_overflow(a.c, lt_p10[s - a.s], &r)) lt_decimal_overflow(line);
    return r;
}

// divides by 10^k, rounding half away from zero
static __int128 lt_div_round(__int128 n, __int128 d) {
    __int128 q = n / d, r = n % d;
    if (r < 0) r = -r;
    if (d < 0 ? 2 * r >= -d : 2 * r >= d) q += ((n < 0) != (d < 0)) ? -1 : 1;
    return q;
}

static lt_decimal lt_decimal_from_i128(__int128 c, int32_t s, int line) {
    // keep it in 64 bits, giving up digits after the point if needed
    while ((c > INT64_MAX || c < INT64_MIN) && s > 0) {
        c = lt_div_round(c, 10);
        s--;
    }
    if (c > INT64_MAX || c < INT64_MIN) lt_decimal_overflow(line);
    while (s > 18) {
        c = lt_div_round(c, 10);
        s--;
    }
    return (lt_decimal){ (int64_t)c, s };
}

static lt_decimal lt_decimal_add(lt_decimal a, lt_decimal b, int line) {
    int32_t s = a.s > b.s ? a.s : b.s;
    int64_t r;
    if (__builtin_add_overflow(lt_decimal_scaled(a, s, line), lt_decimal_scaled(b, s, line), &r)) lt_decimal_overflow(line);
    return (lt_decimal){ r, s };
}

static lt_decimal lt_decimal_sub(lt_decimal a, lt_decimal b, int line) {
    int32_t s = a.s > b.s ? a.s : b.s;
    int64_t r;
    if (__builtin_sub_overflow(lt_decimal_scaled(a, s, line), lt_decimal_scaled(b, s, line), &r)) lt_decimal_overflow(line);
    return (lt_decimal){ r, s };
}

static lt_decimal lt_decimal_mul(lt_decimal a, lt_decimal b, int line) {
    return lt_decimal_from_i128((__int128)a.c * b.c, a.s + b.s, line);
}

static lt_decimal lt_decimal_neg(lt_decimal a, int line) {
    if (a.c == INT64_MIN) lt_decimal_overflow(line);
    return (lt_decimal){ -a.c, a.s };
}

static lt_decimal lt_decimal_abs(lt_decimal a, int line) { return a.c < 0 ? lt_decimal_neg(a, line) : a; }

// exactly `places` digits after the point, rounding half away from zero
static lt_decimal lt_decimal_round(lt_decimal a, int64_t places, int line) {
    if (places < 0 || places > 18) lt_panic_at("round: places must be 0 to 18", line);
    if (places >= a.s) return (lt_decimal){ lt_decimal_scaled(a, (int32_t)places, line), (int32_t)places };
    return (lt_decimal){ (int64_t)lt_div_round(a.c, lt_p10[a.s - places]), (int32_t)places };
}

// a / b with `places` digits after the point, rounded
static lt_decimal lt_decimal_div(lt_decimal a, lt_decimal b, int64_t places, int line) {
    if (b.c == 0) lt_panic_at("division by zero", line);
    if (places < 0 || places > 18) lt_panic_at("div: places must be 0 to 18", line);
    // a.c/10^a.s / (b.c/10^b.s) * 10^places = a.c * 10^(places + b.s - a.s) / b.c
    int64_t k = places + b.s - a.s;
    __int128 n = a.c;
    __int128 d = b.c;
    if (k >= 0) {
        for (int64_t i = 0; i < k; i++) n *= 10;
    } else {
        for (int64_t i = 0; i < -k; i++) d *= 10;
    }
    return lt_decimal_from_i128(lt_div_round(n, d), (int32_t)places, line);
}

static int lt_decimal_cmp(lt_decimal a, lt_decimal b) {
    __int128 x = a.c, y = b.c;
    for (int i = a.s; i < b.s; i++) x *= 10;
    for (int i = b.s; i < a.s; i++) y *= 10;
    return x < y ? -1 : x > y ? 1 : 0;
}

static bool lt_decimal_eq(lt_decimal a, lt_decimal b) { return lt_decimal_cmp(a, b) == 0; }

static uint64_t lt_decimal_hash(lt_decimal a) {
    // equal values hash alike: drop trailing zeros first
    while (a.s > 0 && a.c % 10 == 0) {
        a.c /= 10;
        a.s--;
    }
    return (uint64_t)a.c * 0x9E3779B97F4A7C15ULL ^ (uint64_t)a.s;
}

static lt_text *lt_decimal_text(lt_decimal a) {
    char digits[32], out[48];
    uint64_t m = a.c < 0 ? (uint64_t)(-(a.c + 1)) + 1 : (uint64_t)a.c;
    int n = snprintf(digits, sizeof digits, "%llu", (unsigned long long)m);
    int w = 0;
    if (a.c < 0) out[w++] = '-';
    if (a.s == 0) {
        memcpy(out + w, digits, (size_t)n);
        w += n;
    } else if (n <= a.s) {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = n; i < a.s; i++) out[w++] = '0';
        memcpy(out + w, digits, (size_t)n);
        w += n;
    } else {
        memcpy(out + w, digits, (size_t)(n - a.s));
        w += n - a.s;
        out[w++] = '.';
        memcpy(out + w, digits + n - a.s, (size_t)a.s);
        w += a.s;
    }
    return lt_text_from(out, w);
}

// "12", "-0.50", "1234.5678"; false if it isn't one
static bool lt_decimal_parse(const char *s, int64_t len, lt_decimal *out) {
    int64_t i = 0;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    __int128 c = 0;
    int32_t scale = 0, digits = 0;
    bool point = false, any = false;
    for (; i < len; i++) {
        char ch = s[i];
        if (ch == '.' && !point) {
            point = true;
            continue;
        }
        if (ch < '0' || ch > '9') return false;
        any = true;
        if (digits == 0 && ch == '0' && !point) continue;
        c = c * 10 + (ch - '0');
        digits++;
        if (point) scale++;
        if (digits > 18) return false;
    }
    if (!any) return false;
    out->c = (int64_t)(neg ? -c : c);
    out->s = scale;
    return true;
}

static lt_decimal lt_decimal_lit(const char *s) {
    lt_decimal d = { 0, 0 };
    lt_decimal_parse(s, (int64_t)strlen(s), &d);
    return d;
}

static lt_err lt_text_to_decimal(lt_text *t, lt_decimal *out) {
    const char *why = lt_number_text_problem(t);
    if (why) return lt_number_error(t, "a decimal number", why);
    if (!lt_decimal_parse(t->data, t->len, out)) {
        char buf[160];
        snprintf(buf, sizeof buf, "\"%.*s\" is not a decimal number (like 19.99)", (int)(t->len > 100 ? 100 : t->len), t->data);
        return lt_make_failure(lt_text_cstr(buf));
    }
    return (lt_err){ 0 };
}

static double lt_decimal_to_float(lt_decimal a) { return (double)a.c / (double)lt_p10[a.s]; }

static lt_decimal lt_decimal_from_float(double v, int line) {
    if (!isfinite(v)) lt_panic_at("a Decimal can't be NaN or infinite", line);
    lt_text *t = lt_float_to_text(v);
    lt_decimal d = { 0, 0 };
    if (!lt_decimal_parse(t->data, t->len, &d)) {
        lt_text_drop(t);
        lt_decimal_overflow(line);
    }
    lt_text_drop(t);
    return d;
}

// ---------------------------------------------------------------- more text

// the character position of `part` at or after character `from`; -1 if absent
static int64_t lt_text_find(lt_text *t, lt_text *part, int64_t from) {
    if (from < 0) from = 0;
    int64_t start = lt_text_char_offset(t, from);
    if (start > t->len) return -1;
    const char *hit = lt_find(t->data + start, t->len - start, part->data, part->len);
    if (!hit) return -1;
    return lt_text_char_index(t, hit - t->data);
}

// the character index of the last `part`, or -1
static int64_t lt_text_rfind(lt_text *t, lt_text *part) {
    if (part->len > t->len) return -1;
    for (int64_t i = t->len - part->len; i >= 0; i--) {
        if (memcmp(t->data + i, part->data, (size_t)part->len) == 0) {
            return lt_text_char_index(t, i);
        }
    }
    return -1;
}

static lt_text *lt_text_trim_side(lt_text *t, bool start, bool end) {
    int64_t a = 0, b = t->len;
    if (start)
        while (a < b && isspace((unsigned char)t->data[a])) a++;
    if (end)
        while (b > a && isspace((unsigned char)t->data[b - 1])) b--;
    return lt_text_from(t->data + a, b - a);
}

static uint32_t lt_utf8_next(const unsigned char **s, const unsigned char *e) {
    uint32_t c = **s;
    int n = 1;
    if (c >= 0xf0) c &= 0x07, n = 4;
    else if (c >= 0xe0) c &= 0x0f, n = 3;
    else if (c >= 0xc0) c &= 0x1f, n = 2;
    for (int i = 1; i < n && *s + i < e; i++) c = (c << 6) | ((*s)[i] & 0x3f);
    *s += n;
    return c;
}

static bool lt_uc_in(const uint32_t (*t)[2], size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (c < t[m][0]) hi = m;
        else if (c > t[m][1]) lo = m + 1;
        else return true;
    }
    return false;
}
#define LT_UC_IN(t, c) lt_uc_in(t, sizeof(t) / sizeof(t[0]), c)
// the Unicode letter categories (L, Lu, Ll)
static bool lt_cp_letter(uint32_t c) { return c < 0x80 ? isalpha((int)c) : LT_UC_IN(lt_uc_letter, c); }
static bool lt_cp_upper(uint32_t c) { return c < 0x80 ? (c >= 'A' && c <= 'Z') : LT_UC_IN(lt_uc_upper, c); }
static bool lt_cp_lower(uint32_t c) { return c < 0x80 ? (c >= 'a' && c <= 'z') : LT_UC_IN(lt_uc_lower, c); }

static uint32_t lt_uc_map(const uint32_t (*t)[2], size_t n, uint32_t c) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (c < t[m][0]) hi = m;
        else if (c > t[m][0]) lo = m + 1;
        else return t[m][1];
    }
    return c;
}
static int lt_utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }
static char *lt_utf8_write(char *w, uint32_t c) {
    if (c < 0x80) *w++ = (char)c;
    else if (c < 0x800) *w++ = (char)(0xC0 | (c >> 6)), *w++ = (char)(0x80 | (c & 0x3F));
    else if (c < 0x10000) *w++ = (char)(0xE0 | (c >> 12)), *w++ = (char)(0x80 | ((c >> 6) & 0x3F)), *w++ = (char)(0x80 | (c & 0x3F));
    else *w++ = (char)(0xF0 | (c >> 18)), *w++ = (char)(0x80 | ((c >> 12) & 0x3F)), *w++ = (char)(0x80 | ((c >> 6) & 0x3F)), *w++ = (char)(0x80 | (c & 0x3F));
    return w;
}

// lower / upper: ASCII directly; other characters through the Unicode
// one-to-one case mappings (the length in bytes may change)
static lt_text *lt_text_case(lt_text *t, bool up) {
    bool ascii = true;
    for (int64_t i = 0; i < t->len; i++)
        if ((unsigned char)t->data[i] >= 0x80) ascii = false;
    if (ascii) {
        lt_text *r = lt_text_new(t->len);
        for (int64_t i = 0; i < t->len; i++) {
            char c = t->data[i];
            if (up) r->data[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            else r->data[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        return r;
    }
    const uint32_t (*tab)[2] = up ? lt_uc_to_upper : lt_uc_to_lower;
    size_t n = up ? sizeof(lt_uc_to_upper) / sizeof(lt_uc_to_upper[0]) : sizeof(lt_uc_to_lower) / sizeof(lt_uc_to_lower[0]);
    const unsigned char *e = (const unsigned char *)t->data + t->len;
    int64_t len = 0;
    for (const unsigned char *s = (const unsigned char *)t->data; s < e;) {
        const unsigned char *s0 = s;
        uint32_t c = lt_utf8_next(&s, e);
        if (c < 0x80) len += 1;
        else if (s > e) len += e - s0; // cut short: kept as it is
        else len += lt_utf8_len(lt_uc_map(tab, n, c));
    }
    lt_text *r = lt_text_new(len);
    char *w = r->data;
    for (const unsigned char *s = (const unsigned char *)t->data; s < e;) {
        const unsigned char *s0 = s;
        uint32_t c = lt_utf8_next(&s, e);
        if (c < 0x80) *w++ = up ? ((c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c) : ((c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c);
        else if (s > e) {
            memcpy(w, s0, (size_t)(e - s0));
            w += e - s0;
        } else w = lt_utf8_write(w, lt_uc_map(tab, n, c));
    }
    return r;
}
static lt_text *lt_text_lower(lt_text *t) { return lt_text_case(t, false); }
static lt_text *lt_text_upper(lt_text *t) { return lt_text_case(t, true); }

// Int.character: the UTF-8 of a code point; U+FFFD if it isn't one
static lt_text *lt_code_point_text(int64_t c) {
    if (c < 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    char b[4];
    int n;
    if (c < 0x80) b[0] = (char)c, n = 1;
    else if (c < 0x800) b[0] = (char)(0xC0 | (c >> 6)), b[1] = (char)(0x80 | (c & 0x3F)), n = 2;
    else if (c < 0x10000) b[0] = (char)(0xE0 | (c >> 12)), b[1] = (char)(0x80 | ((c >> 6) & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), n = 3;
    else b[0] = (char)(0xF0 | (c >> 18)), b[1] = (char)(0x80 | ((c >> 12) & 0x3F)), b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), n = 4;
    return lt_text_from(b, n);
}

// every character passes (and there is at least one): 0 digit, 1 letter,
// 2 space, 3 upper, 4 lower
static bool lt_text_all(lt_text *t, int what) {
    if (t->len == 0) return false;
    const unsigned char *s = (const unsigned char *)t->data, *e = s + t->len;
    while (s < e) {
        uint32_t c = lt_utf8_next(&s, e);
        bool ok;
        switch (what) {
        case 0: ok = c >= '0' && c <= '9'; break;
        case 1: ok = lt_cp_letter(c); break;
        case 2: ok = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A); break;
        case 3: ok = lt_cp_upper(c); break;
        default: ok = lt_cp_lower(c); break;
        }
        if (!ok) return false;
    }
    return true;
}
