// Tasks: lightweight threads on a pool of OS threads (one per core).
//
// A task has its own stack; switching between tasks saves and restores the
// callee-saved registers (a few dozen instructions, see lt_ctx_switch).
// Every blocking operation (waiting for a task, a channel, a lock, a timer)
// parks the task on a wait queue and lets the OS thread run other tasks.
//
// Parking protocol: the task adds itself to a wait queue while holding the
// queue's lock, then switches to its worker; the worker releases the lock
// only after the switch, so nobody can wake the task before its registers
// are saved.

#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

// ---------------------------------------------------------------- context switch

typedef struct lt_ctx { void *sp; } lt_ctx;
void lt_ctx_switch(lt_ctx *from, lt_ctx *to);
void lt_task_trampoline(void);

#if defined(__APPLE__)
#define LT_SYM(x) "_" #x
#else
#define LT_SYM(x) #x
#endif

#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl " LT_SYM(lt_ctx_switch) "\n"
    ".p2align 2\n"
    LT_SYM(lt_ctx_switch) ":\n"
    "  sub sp, sp, #160\n"
    "  stp x19, x20, [sp, #0]\n"
    "  stp x21, x22, [sp, #16]\n"
    "  stp x23, x24, [sp, #32]\n"
    "  stp x25, x26, [sp, #48]\n"
    "  stp x27, x28, [sp, #64]\n"
    "  stp x29, x30, [sp, #80]\n"
    "  stp d8, d9, [sp, #96]\n"
    "  stp d10, d11, [sp, #112]\n"
    "  stp d12, d13, [sp, #128]\n"
    "  stp d14, d15, [sp, #144]\n"
    "  mov x9, sp\n"
    "  str x9, [x0]\n"
    "  ldr x9, [x1]\n"
    "  mov sp, x9\n"
    "  ldp x19, x20, [sp, #0]\n"
    "  ldp x21, x22, [sp, #16]\n"
    "  ldp x23, x24, [sp, #32]\n"
    "  ldp x25, x26, [sp, #48]\n"
    "  ldp x27, x28, [sp, #64]\n"
    "  ldp x29, x30, [sp, #80]\n"
    "  ldp d8, d9, [sp, #96]\n"
    "  ldp d10, d11, [sp, #112]\n"
    "  ldp d12, d13, [sp, #128]\n"
    "  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #160\n"
    "  ret\n"
    ".globl " LT_SYM(lt_task_trampoline) "\n"
    ".p2align 2\n"
    LT_SYM(lt_task_trampoline) ":\n"
    "  mov x0, x19\n"
    "  bl " LT_SYM(lt_task_main) "\n"
    "  brk #0\n");
#define LT_CTX_FRAME 160
#elif defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl " LT_SYM(lt_ctx_switch) "\n"
    LT_SYM(lt_ctx_switch) ":\n"
    "  pushq %rbp\n"
    "  pushq %rbx\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  movq %rsp, (%rdi)\n"
    "  movq (%rsi), %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %rbx\n"
    "  popq %rbp\n"
    "  ret\n"
    ".globl " LT_SYM(lt_task_trampoline) "\n"
    LT_SYM(lt_task_trampoline) ":\n"
    "  movq %rbx, %rdi\n"
    "  call " LT_SYM(lt_task_main) "\n"
    "  ud2\n");
#else
#error "tasks are not supported on this architecture yet"
#endif

// ---------------------------------------------------------------- tasks

struct lt_task;
typedef struct lt_waitq {
    lt_spin *lock;
    struct lt_task *head, *tail;
} lt_waitq;

enum { LT_READY = 0, LT_RUNNING = 1, LT_PARKED = 2, LT_DONE = 3 };

typedef struct lt_scope {
    struct lt_task *tasks; // started in this function, not yet finished with
    struct lt_scope *parent_link;
} lt_scope;

typedef struct lt_task {
    int64_t rc;
    lt_ctx ctx;
    char *stack;
    lt_fn fn;                             // the call to run
    void (*run)(struct lt_task *);        // calls fn, stores the result
    void (*drop_result)(void *);
    int state;
    int cancelled;
    int observed;                          // someone waited and saw the outcome
    lt_err error;
    struct lt_task *next;                  // in a run queue or a wait queue
    struct lt_task *prev;
    lt_waitq *parked_on;                   // interruptible wait (for cancel)
    lt_spin lock;                          // protects `done_q`
    lt_waitq done_q;                       // tasks waiting for this one
    struct lt_task *scope_next;            // in the parent's scope list
    struct lt_task *scope_prev;
    lt_scope *scope;                       // that list, until waited for
    lt_scope *own_scope;                   // the scope of the function running in it
    int64_t wake_at;                       // sleeping: deadline (ns)
    int timed_out;
    int io_fd;                             // waiting for this descriptor (-1: not)
    void *fiber;                           // ThreadSanitizer's view of the task
    lt_spin io_spin;                       // held while registering for I/O
    void *panic_jmp;                       // a jmp_buf: a panic ends this request only
    struct lt_lock *held[8];               // locks held, released after such a panic
    int nheld;
    void *txs[4];                          // db connections it has a transaction on (db.h)
    int ntxs;
    int deep;                              // its stack was opened past the top part
    int worker;                            // the worker running it (an index)
    char result[] __attribute__((aligned(16)));
} lt_task;

// A task's stack: 8 MB, like a program's main thread. Only the pages a task
// touches take memory.
#define LT_STACK_SIZE ((size_t)8 << 20)
#define LT_GUARD ((size_t)16384)
// Only the top LT_STACK_KEEP of a stack is open at first; the rest can't be
// touched. A task that goes deeper faults once, and the fault handler opens
// the rest (lt_task_grow). When such a stack goes back to the pool, its deep
// pages go back to the system and are closed again. (Under the sanitizers,
// which own the fault handler, stacks are open from the start.)
#define LT_STACK_KEEP ((size_t)256 << 10)

// Run queues. Each worker (OS thread) has its own: a task made ready on a
// worker goes there, and the worker takes from it without contention. Other
// threads (timers, I/O) use the global queue. A worker with nothing to do
// takes from the global queue, then steals half of another worker's queue,
// spins a little, and then sleeps. Only one sleeper is woken per new task,
// and none while a worker is already looking for work.
#define LT_LOCALQ 256
typedef struct lt_worker {
    lt_spin lock;
    // the task made ready last on this worker runs next, here: a task that
    // wakes another (a channel, a lock) usually waits right after, and the
    // pair then stays on one core. Others may take it only once it has
    // waited a few microseconds (the owner is busy).
    lt_task *next;
    int64_t next_at;
    uint32_t head, tail; // take at head, add at tail
    lt_task *q[LT_LOCALQ];
    uint32_t tick;       // schedules, to look at the global queue now and then
    uint32_t seed;       // for picking whom to steal from
    // stacks kept by this worker (only it touches them), traded with the
    // global pool in batches
    char *stacks[16];
    int nstacks;
    int64_t blocked_since; // in a blocking C call since (0: not)
} lt_worker;
// Worker threads: one per core, plus spares started while workers are stuck
// in blocking calls (see lt_check_blocked).
#define LT_MAX_WORKERS 256
static lt_worker *lt_ws;
static __thread lt_worker *lt_self;

static lt_spin lt_gq_lock;
static lt_task *lt_q_head, *lt_q_tail;
static int64_t lt_gq_n;

static pthread_mutex_t lt_park_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lt_park_cv = PTHREAD_COND_INITIALIZER;
// lt_spinning: workers looking for work. lt_wakes: wake-ups given to
// sleeping workers and not yet taken (each woken worker starts out
// counted as looking).
static int lt_workers, lt_idle, lt_spinning, lt_wakes, lt_shutdown, lt_sleepers, lt_io_waiters;
static __thread lt_task *lt_cur;
static __thread lt_ctx lt_worker_ctx;
static __thread lt_spin *lt_release_after;
static __thread lt_task *lt_finished;
static __thread void *lt_worker_fiber;

LT_INLINE void lt_to_worker(lt_task *t) {
#ifdef LT_TSAN
    __tsan_switch_to_fiber(lt_worker_fiber, 0);
#endif
    lt_ctx_switch(&t->ctx, &lt_worker_ctx);
}
LT_INLINE void lt_to_task(lt_task *t) {
#ifdef LT_TSAN
    lt_worker_fiber = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(t->fiber, 0);
#endif
    lt_ctx_switch(&lt_worker_ctx, &t->ctx);
}

static lt_spin lt_stack_lock;
static char *lt_stack_pool[64];
static int lt_stack_count;

#define LT_STACK_BATCH 8
static char *lt_stack_get_shared(void);
static void lt_stack_put_shared(char *s);
static lt_worker *lt_self_worker(void);
static char *lt_stack_get(void) {
    lt_worker *w = lt_self_worker();
    if (w) {
        if (w->nstacks == 0) {
            // a batch from the pool, under one lock
            lt_spin_lock(&lt_stack_lock);
            while (w->nstacks < LT_STACK_BATCH && lt_stack_count > 0) w->stacks[w->nstacks++] = lt_stack_pool[--lt_stack_count];
            lt_spin_unlock(&lt_stack_lock);
        }
        if (w->nstacks > 0) {
            char *s = w->stacks[--w->nstacks];
#ifdef LT_ASAN
            __asan_unpoison_memory_region(s + LT_GUARD, LT_STACK_SIZE - LT_GUARD);
#endif
            return s;
        }
    }
    return lt_stack_get_shared();
}
static char *lt_stack_get_shared(void) {
    lt_spin_lock(&lt_stack_lock);
    if (lt_stack_count > 0) {
        char *s = lt_stack_pool[--lt_stack_count];
        lt_spin_unlock(&lt_stack_lock);
#ifdef LT_ASAN
        // frames of the task that used this stack before are still marked
        __asan_unpoison_memory_region(s + LT_GUARD, LT_STACK_SIZE - LT_GUARD);
#endif
        return s;
    }
    lt_spin_unlock(&lt_stack_lock);
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
#if defined(LT_ASAN) || defined(LT_TSAN)
    char *s = (char *)mmap(NULL, LT_STACK_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (s == MAP_FAILED) lt_oom();
    mprotect(s, LT_GUARD, PROT_NONE); // overflow hits the guard page
#else
    char *s = (char *)mmap(NULL, LT_STACK_SIZE, PROT_NONE, flags, -1, 0);
    if (s == MAP_FAILED) lt_oom();
    mprotect(s + LT_STACK_SIZE - LT_STACK_KEEP, LT_STACK_KEEP, PROT_READ | PROT_WRITE);
#endif
    return s;
}
static void lt_stack_put(char *s, bool deep) {
    if (deep) {
        madvise(s + LT_GUARD, LT_STACK_SIZE - LT_STACK_KEEP - LT_GUARD, MADV_DONTNEED);
#if !defined(LT_ASAN) && !defined(LT_TSAN)
        mprotect(s + LT_GUARD, LT_STACK_SIZE - LT_STACK_KEEP - LT_GUARD, PROT_NONE);
#endif
    }
    lt_worker *w = lt_self_worker();
    if (w) {
        if (w->nstacks == 16) {
            // half back to the pool, under one lock
            lt_spin_lock(&lt_stack_lock);
            while (w->nstacks > LT_STACK_BATCH && lt_stack_count < 64) lt_stack_pool[lt_stack_count++] = w->stacks[--w->nstacks];
            lt_spin_unlock(&lt_stack_lock);
            while (w->nstacks > LT_STACK_BATCH) munmap(w->stacks[--w->nstacks], LT_STACK_SIZE);
        }
        w->stacks[w->nstacks++] = s;
        return;
    }
    lt_stack_put_shared(s);
}
static void lt_stack_put_shared(char *s) {
    lt_spin_lock(&lt_stack_lock);
    if (lt_stack_count < 64) {
        lt_stack_pool[lt_stack_count++] = s;
        lt_spin_unlock(&lt_stack_lock);
        return;
    }
    lt_spin_unlock(&lt_stack_lock);
    munmap(s, LT_STACK_SIZE);
}

__attribute__((noinline)) static lt_task *lt_current(void) { return lt_cur; }

// For the fault handler: a fault below the open top of the running task's
// stack opens the rest (true: go on).
static bool lt_task_grow(char *addr) {
    lt_task *t = lt_cur;
    if (!t || !t->stack || t->deep) return false;
    char *lo = t->stack + LT_GUARD, *hi = t->stack + LT_STACK_SIZE - LT_STACK_KEEP;
    if (addr < lo || addr >= hi) return false;
    if (mprotect(lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE) != 0) return false;
    t->deep = 1;
    return true;
}

// For the fault handler: is `addr` in the guard page of the running task?
static bool lt_task_overflowed(char *addr) {
    lt_task *t = lt_cur;
    return t && t->stack && addr >= t->stack - LT_PAGE && addr < t->stack + LT_GUARD + LT_PAGE;
}

static void lt_task_free(lt_task *t) {
    if (!t->error.obj && t->state == LT_DONE && t->drop_result) t->drop_result(t->result);
    lt_iface_drop(t->error);
    free(t);
}
LT_INLINE void lt_task_dup(lt_task *t) {
    if (t) __atomic_fetch_add(&t->rc, 1, __ATOMIC_RELAXED);
}
LT_INLINE void lt_task_drop(lt_task *t) {
    if (t && __atomic_sub_fetch(&t->rc, 1, __ATOMIC_ACQ_REL) == 0) lt_task_free(t);
}

// (called from task code, which may have moved to another thread: the
// compiler must not reuse an earlier thread's `lt_self`)
__attribute__((noinline)) static lt_worker *lt_self_worker(void) {
    lt_worker *w = lt_self;
    __asm__ volatile("" : "+r"(w) : : "memory");
    return w;
}

static void lt_gq_push_list(lt_task *first, lt_task *last, int64_t n) {
    lt_spin_lock(&lt_gq_lock);
    last->next = NULL;
    if (lt_q_tail) lt_q_tail->next = first;
    else lt_q_head = first;
    lt_q_tail = last;
    __atomic_add_fetch(&lt_gq_n, n, __ATOMIC_SEQ_CST);
    lt_spin_unlock(&lt_gq_lock);
}

// Wakes a sleeping worker if there is one and nobody is looking for work.
// The waker claims "one is looking" first, so a burst of new tasks wakes
// one worker, not one each (it then wakes the next if work remains).
static void lt_wake_one(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&lt_idle, __ATOMIC_SEQ_CST) == 0) return;
    int zero = 0;
    if (!__atomic_compare_exchange_n(&lt_spinning, &zero, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&lt_park_mu);
    if (lt_idle > lt_wakes) {
        lt_wakes++;
        pthread_cond_signal(&lt_park_cv);
    } else {
        __atomic_sub_fetch(&lt_spinning, 1, __ATOMIC_SEQ_CST); // nobody asleep after all
    }
    pthread_mutex_unlock(&lt_park_mu);
}

// `next`: a task woken by a channel, which usually hands work back and
// forth with the one waking it (it runs next on this worker); other tasks
// go to the queue, where idle workers take them at once.
static void lt_ready_on(lt_task *t, bool next) {
    __atomic_store_n(&t->state, LT_READY, __ATOMIC_RELAXED);
    t->next = NULL;
    lt_worker *w = lt_self_worker();
    if (w) {
        lt_spin_lock(&w->lock);
        if (next) {
            // the new one runs next; the one it replaces joins the queue
            lt_task *old = w->next;
            w->next = t;
            w->next_at = lt_monotonic_nanos();
            t = old;
        }
        if (!t) {
            lt_spin_unlock(&w->lock);
        } else if (w->tail - w->head < LT_LOCALQ) {
            w->q[w->tail % LT_LOCALQ] = t;
            __atomic_store_n(&w->tail, w->tail + 1, __ATOMIC_SEQ_CST);
            lt_spin_unlock(&w->lock);
        } else {
            // full: half of it, and this one, to the global queue
            uint32_t n = (w->tail - w->head) / 2;
            lt_task *first = w->q[w->head % LT_LOCALQ], *last = first;
            for (uint32_t i = 1; i < n; i++) {
                lt_task *x = w->q[(w->head + i) % LT_LOCALQ];
                last->next = x;
                last = x;
            }
            last->next = t;
            w->head += n;
            lt_spin_unlock(&w->lock);
            lt_gq_push_list(first, t, (int64_t)n + 1);
        }
    } else {
        lt_gq_push_list(t, t, 1);
    }
    lt_wake_one();
}
static void lt_ready(lt_task *t) { lt_ready_on(t, false); }

static lt_task *lt_local_pop(lt_worker *w) {
    if (!__atomic_load_n(&w->next, __ATOMIC_SEQ_CST) && __atomic_load_n(&w->head, __ATOMIC_SEQ_CST) == __atomic_load_n(&w->tail, __ATOMIC_SEQ_CST)) return NULL;
    lt_spin_lock(&w->lock);
    lt_task *t = w->next;
    if (t) w->next = NULL;
    else if (w->head != w->tail) t = w->q[w->head++ % LT_LOCALQ];
    lt_spin_unlock(&w->lock);
    return t;
}

// Takes a batch from the global queue: one to run, some more for later.
static lt_task *lt_global_take(lt_worker *w) {
    if (__atomic_load_n(&lt_gq_n, __ATOMIC_SEQ_CST) == 0) return NULL;
    lt_spin_lock(&lt_gq_lock);
    lt_task *t = lt_q_head;
    if (!t) {
        lt_spin_unlock(&lt_gq_lock);
        return NULL;
    }
    int64_t n = lt_gq_n / __atomic_load_n(&lt_workers, __ATOMIC_ACQUIRE) + 1;
    if (n > LT_LOCALQ / 2) n = LT_LOCALQ / 2;
    // only this worker adds to its queue (others only take), so the room
    // seen now is there when the batch goes in
    int64_t room = LT_LOCALQ - (int64_t)(__atomic_load_n(&w->tail, __ATOMIC_SEQ_CST) - __atomic_load_n(&w->head, __ATOMIC_SEQ_CST));
    if (n > room + 1) n = room + 1;
    lt_q_head = t->next;
    int64_t took = 1;
    lt_task *more = NULL, *more_last = NULL; // (kept in order)
    while (took < n && lt_q_head) {
        lt_task *x = lt_q_head;
        lt_q_head = x->next;
        x->next = NULL;
        if (more_last) more_last->next = x;
        else more = x;
        more_last = x;
        took++;
    }
    if (!lt_q_head) lt_q_tail = NULL;
    __atomic_sub_fetch(&lt_gq_n, took, __ATOMIC_SEQ_CST);
    lt_spin_unlock(&lt_gq_lock);
    t->next = NULL;
    if (more) {
        lt_spin_lock(&w->lock);
        while (more) {
            lt_task *x = more;
            more = x->next;
            x->next = NULL;
            w->q[w->tail % LT_LOCALQ] = x;
            w->tail++;
        }
        lt_spin_unlock(&w->lock);
    }
    return t;
}

// Steals half of another worker's queue; returns one to run.
static lt_task *lt_steal(lt_worker *w) {
    int n = __atomic_load_n(&lt_workers, __ATOMIC_ACQUIRE);
    w->seed = w->seed * 1103515245u + 12345u;
    int start = (int)((w->seed >> 8) % (uint32_t)n);
    for (int k = 0; k < n; k++) {
        lt_worker *v = &lt_ws[(start + k) % n];
        if (v == w) continue;
        if (__atomic_load_n(&v->head, __ATOMIC_SEQ_CST) == __atomic_load_n(&v->tail, __ATOMIC_SEQ_CST)) {
            // only a next task: take it if its worker has kept it waiting
            if (!__atomic_load_n(&v->next, __ATOMIC_SEQ_CST)) continue;
            if (lt_monotonic_nanos() - __atomic_load_n(&v->next_at, __ATOMIC_SEQ_CST) < 5000) continue;
            lt_spin_lock(&v->lock);
            lt_task *t = v->head == v->tail ? v->next : NULL;
            if (t) v->next = NULL;
            lt_spin_unlock(&v->lock);
            if (t) return t;
            continue;
        }
        lt_task *got[LT_LOCALQ / 2];
        uint32_t m = 0;
        lt_spin_lock(&v->lock);
        uint32_t avail = v->tail - v->head;
        m = (avail + 1) / 2;
        for (uint32_t i = 0; i < m; i++) got[i] = v->q[(v->head + i) % LT_LOCALQ];
        v->head += m;
        lt_spin_unlock(&v->lock);
        if (m == 0) continue;
        if (m > 1) {
            lt_spin_lock(&w->lock);
            for (uint32_t i = 1; i < m; i++) {
                w->q[w->tail % LT_LOCALQ] = got[i];
                w->tail++;
            }
            lt_spin_unlock(&w->lock);
        }
        return got[0];
    }
    return NULL;
}

static bool lt_any_work(void) {
    if (__atomic_load_n(&lt_gq_n, __ATOMIC_SEQ_CST) > 0) return true;
    int nw = __atomic_load_n(&lt_workers, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nw; i++)
        if (__atomic_load_n(&lt_ws[i].next, __ATOMIC_SEQ_CST) || __atomic_load_n(&lt_ws[i].head, __ATOMIC_SEQ_CST) != __atomic_load_n(&lt_ws[i].tail, __ATOMIC_SEQ_CST)) return true;
    return false;
}

static lt_task *lt_find_work(lt_worker *w) {
    // now and then the global queue first, so it isn't starved
    if (++w->tick % 61 == 0) {
        lt_task *t = lt_global_take(w);
        if (t) return t;
    }
    lt_task *t = lt_local_pop(w);
    if (t) return t;
    t = lt_global_take(w);
    if (t) return t;
    return lt_steal(w);
}

// wait queues (caller holds q->lock)
static void lt_wq_push(lt_waitq *q, lt_task *t) {
    t->next = NULL;
    t->prev = q->tail;
    if (q->tail) q->tail->next = t;
    else q->head = t;
    q->tail = t;
}
static void lt_wq_remove(lt_waitq *q, lt_task *t) {
    if (t->prev) t->prev->next = t->next;
    else q->head = t->next;
    if (t->next) t->next->prev = t->prev;
    else q->tail = t->prev;
    t->next = t->prev = NULL;
}
static lt_task *lt_wq_pop(lt_waitq *q) {
    lt_task *t = q->head;
    if (t) {
        lt_wq_remove(q, t);
        __atomic_store_n(&t->parked_on, NULL, __ATOMIC_RELEASE);
    }
    return t;
}

// Park the current task on `q` (the caller holds q->lock). `interruptible`:
// cancelling the task wakes it.
__attribute__((noinline)) static void lt_park_on(lt_waitq *q, bool interruptible) {
    lt_task *t = lt_current();
    lt_wq_push(q, t);
    if (interruptible) __atomic_store_n(&t->parked_on, q, __ATOMIC_RELEASE);
    __atomic_store_n(&t->state, LT_PARKED, __ATOMIC_RELAXED);
    lt_release_after = q->lock;
    lt_to_worker(t);
}

// wake everyone on q (caller holds q->lock)
static void lt_wake_all(lt_waitq *q) {
    lt_task *t;
    while ((t = lt_wq_pop(q))) lt_ready(t);
}

static void lt_timer_cancel(lt_task *t);

void lt_task_cancel(lt_task *t);
typedef struct lt_fdesc lt_fdesc;
static lt_fdesc *lt_fdesc_of(int fd);
static lt_task *lt_fdesc_take(lt_fdesc *d, lt_task *only);
static void lt_io_wake(lt_task *t);
static void lt_cancel_scope(lt_scope *s) {
    for (lt_task *c = s->tasks; c; c = c->scope_next) lt_task_cancel(c);
}

void lt_task_cancel(lt_task *t) {
    if (__atomic_exchange_n(&t->cancelled, 1, __ATOMIC_ACQ_REL)) return;
    // wake it if it waits interruptibly
    for (;;) {
        lt_waitq *q = __atomic_load_n(&t->parked_on, __ATOMIC_ACQUIRE);
        if (!q) break;
        lt_spin_lock(q->lock);
        if (__atomic_load_n(&t->parked_on, __ATOMIC_ACQUIRE) == q) {
            lt_wq_remove(q, t);
            __atomic_store_n(&t->parked_on, NULL, __ATOMIC_RELEASE);
            lt_spin_unlock(q->lock);
            lt_ready(t);
            break;
        }
        lt_spin_unlock(q->lock);
    }
    lt_timer_cancel(t);
    // waiting for a socket: take it from the descriptor's record and wake it
    int fd = __atomic_load_n(&t->io_fd, __ATOMIC_ACQUIRE);
    if (fd >= 0) {
        lt_task *mine = lt_fdesc_take(lt_fdesc_of(fd), t);
        if (mine) lt_io_wake(mine);
    }
    // its own tasks are cancelled too
    lt_spin_lock(&t->lock);
    lt_scope *s = t->own_scope;
    if (s) lt_cancel_scope(s);
    lt_spin_unlock(&t->lock);
}

LT_INLINE bool lt_is_cancelled(void) {
    lt_task *t = lt_current();
    return t && __atomic_load_n(&t->cancelled, __ATOMIC_ACQUIRE);
}

static lt_err lt_make_cancelled(void);
static lt_err lt_make_channel_closed(void);

// The body of every task (called from the assembly trampoline).
static void lt_task_finish(lt_task *t);

__attribute__((used, noinline)) void lt_task_main(lt_task *t) {
    t->run(t);
    // the task may be on another OS thread now: finish in a fresh call, so
    // that no thread-local address from before `run` is reused
    lt_task_finish(t);
}

__attribute__((noinline)) static void lt_task_finish(lt_task *t) {
    lt_fn_drop(t->fn);
    t->fn = (lt_fn){ 0 };
    lt_spin_lock(&t->lock);
    __atomic_store_n(&t->state, LT_DONE, __ATOMIC_RELEASE);
    lt_wake_all(&t->done_q);
    lt_finished = t;
    lt_release_after = &t->lock;
    lt_to_worker(t);
    __builtin_unreachable();
}

static lt_task *lt_task_new(lt_fn fn, size_t result_size, void (*run)(lt_task *), void (*drop_result)(void *)) {
    lt_task *t = (lt_task *)calloc(1, sizeof(lt_task) + result_size + 16);
    t->io_fd = -1;
    if (!t) lt_oom();
    t->rc = 1;
    t->fn = fn;
    t->run = run;
    t->drop_result = drop_result;
    t->done_q.lock = &t->lock;
    t->stack = lt_stack_get();
#ifdef LT_TSAN
    t->fiber = __tsan_create_fiber(0);
#endif
    // an initial frame that "returns" into the trampoline with x19/rbx = t
    char *top = t->stack + LT_STACK_SIZE;
    top = (char *)((uintptr_t)top & ~(uintptr_t)15);
#if defined(__aarch64__)
    void **frame = (void **)(top - LT_CTX_FRAME);
    memset(frame, 0, LT_CTX_FRAME);
    frame[0] = t;                           // x19
    frame[10] = 0;                          // x29
    frame[11] = (void *)lt_task_trampoline; // x30
    t->ctx.sp = frame;
#else
    void **frame = (void **)(top - 8 * 8);
    frame[0] = 0;                           // r15
    frame[1] = 0;                           // r14
    frame[2] = 0;                           // r13
    frame[3] = 0;                           // r12
    frame[4] = t;                           // rbx
    frame[5] = 0;                           // rbp
    frame[6] = (void *)lt_task_trampoline;  // return address
    t->ctx.sp = frame;
#endif
    return t;
}

// ---------------------------------------------------------------- workers

static void lt_deadlock(void) {
    fflush(stdout);
    fprintf(stderr, "panic: deadlock: every task is waiting for another one\n");
    exit(101);
}

static lt_task *lt_next_task(void) {
    lt_worker *w = lt_self;
    bool spinning = false; // counted in lt_spinning
    for (;;) {
        lt_task *t = lt_find_work(w);
        if (!t) {
            // look a little longer before sleeping (work often comes right away)
            if (!spinning) {
                spinning = true;
                __atomic_add_fetch(&lt_spinning, 1, __ATOMIC_SEQ_CST);
            }
            // a few rounds with a pause between them (scanning every queue
            // in a tight loop only fights the owners for their locks)
            for (int i = 0; i < 8 && !t; i++) {
                for (int k = 0; k < 50; k++) {
#if defined(__aarch64__)
                    __asm__ volatile("yield");
#else
                    __asm__ volatile("pause");
#endif
                }
                t = lt_find_work(w);
            }
        }
        if (spinning) {
            spinning = false;
            __atomic_sub_fetch(&lt_spinning, 1, __ATOMIC_SEQ_CST);
            // wakers skipped waking while this one looked: look again, or
            // wake someone for what's left
            if (t) {
                if (lt_any_work()) lt_wake_one();
            } else if (lt_any_work()) {
                continue;
            }
        }
        if (t) return t;
        pthread_mutex_lock(&lt_park_mu);
        if (lt_shutdown) {
            pthread_mutex_unlock(&lt_park_mu);
            return NULL;
        }
        __atomic_add_fetch(&lt_idle, 1, __ATOMIC_SEQ_CST);
        if (lt_any_work()) {
            __atomic_sub_fetch(&lt_idle, 1, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&lt_park_mu);
            continue;
        }
        if (lt_idle == lt_workers && __atomic_load_n(&lt_sleepers, __ATOMIC_ACQUIRE) == 0 && __atomic_load_n(&lt_io_waiters, __ATOMIC_ACQUIRE) == 0) {
            pthread_mutex_unlock(&lt_park_mu);
            lt_deadlock();
        }
        while (lt_wakes == 0 && !lt_shutdown) pthread_cond_wait(&lt_park_cv, &lt_park_mu);
        __atomic_sub_fetch(&lt_idle, 1, __ATOMIC_SEQ_CST);
        if (lt_wakes > 0) {
            lt_wakes--;
            spinning = true; // the waker counted this worker as looking
        }
        pthread_mutex_unlock(&lt_park_mu);
    }
}

static lt_task *lt_main_task;

// Runs tasks until the main task is done (on whichever thread it finishes).
static void lt_worker_loop(void) {
    for (;;) {
        lt_task *t = lt_next_task();
        if (!t) return;
        lt_cur = t;
        t->worker = (int)(lt_self - lt_ws);
        __atomic_store_n(&t->state, LT_RUNNING, __ATOMIC_RELAXED);
        lt_to_task(t);
        lt_cur = NULL;
        if (lt_release_after) {
            lt_spin_unlock(lt_release_after);
            lt_release_after = NULL;
        }
        if (lt_finished) {
            lt_task *f = lt_finished;
            lt_finished = NULL;
            lt_stack_put(f->stack, f->deep);
            f->stack = NULL;
#ifdef LT_TSAN
            __tsan_destroy_fiber(f->fiber);
#endif
            bool is_main = f == lt_main_task;
            lt_task_drop(f); // the reference held while running
            if (is_main) {
                pthread_mutex_lock(&lt_park_mu);
                lt_shutdown = 1;
                pthread_cond_broadcast(&lt_park_cv);
                pthread_mutex_unlock(&lt_park_mu);
                return;
            }
        }
    }
}

static void *lt_worker_thread(void *arg) {
    lt_self = &lt_ws[(intptr_t)arg];
    lt_alt_stack();
    lt_worker_loop();
    return NULL;
}

// ---------------------------------------------------------------- timers

static pthread_mutex_t lt_timer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lt_timer_cv = PTHREAD_COND_INITIALIZER;
static lt_spin lt_timer_spin;
static lt_waitq lt_timer_q = { &lt_timer_spin, NULL, NULL };

// time.timeout: cancel a task at a moment (under lt_timer_mu)
typedef struct lt_deadline {
    int64_t at;
    lt_task *task;
    int fired;
    struct lt_deadline *next;
} lt_deadline;
static lt_deadline *lt_deadlines;

// The timer thread's wait. On macOS a condition variable's timeout is
// coalesced for background processes (services): sleep(10ms) took ~70ms.
// A kqueue timer marked critical isn't, so there the thread waits on a
// kqueue, and new timers wake it with a user event. Called holding
// lt_timer_mu; `ns` < 0 waits until woken.
#if defined(__APPLE__)
#include <sys/event.h>
static int lt_timer_kq = -1;
static void lt_timer_kick(void) {
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(lt_timer_kq, &ev, 1, NULL, 0, NULL);
}
static void lt_timer_wait(int64_t ns) {
    struct kevent ch[2], out[2];
    int n = 0;
    if (ns >= 0) EV_SET(&ch[n++], 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, ns, NULL);
    pthread_mutex_unlock(&lt_timer_mu);
    kevent(lt_timer_kq, ch, n, out, 2, NULL);
    pthread_mutex_lock(&lt_timer_mu);
}
static void lt_timer_setup(void) {
    lt_timer_kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(lt_timer_kq, &ev, 1, NULL, 0, NULL);
}
#else
static void lt_timer_kick(void) { pthread_cond_signal(&lt_timer_cv); }
static void lt_timer_wait(int64_t ns) {
    if (ns < 0) {
        pthread_cond_wait(&lt_timer_cv, &lt_timer_mu);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t n2 = ts.tv_nsec + ns % 1000000000;
    ts.tv_sec += ns / 1000000000 + n2 / 1000000000;
    ts.tv_nsec = n2 % 1000000000;
    pthread_cond_timedwait(&lt_timer_cv, &lt_timer_mu, &ts);
}
static void lt_timer_setup(void) {}
#endif

// ---- blocking calls (SQLite, DNS, files): a worker stuck in one can't run
// other tasks. Once any program part may block, the timer thread looks every
// 10 ms, and if a worker has been stuck for 10 ms while tasks wait to run
// and no worker is free, it starts a spare worker (up to LT_MAX_WORKERS).
static int lt_block_watch;
static void *lt_worker_thread(void *arg);
static void lt_block_enter(void) {
    lt_worker *w = lt_self_worker();
    if (!w) return;
    __atomic_store_n(&w->blocked_since, lt_monotonic_nanos(), __ATOMIC_RELEASE);
    if (!__atomic_load_n(&lt_block_watch, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&lt_timer_mu);
        lt_block_watch = 1;
        lt_timer_kick();
        pthread_mutex_unlock(&lt_timer_mu);
    }
}
static void lt_block_exit(void) {
    lt_worker *w = lt_self_worker();
    if (w) __atomic_store_n(&w->blocked_since, 0, __ATOMIC_RELEASE);
}
static void lt_check_blocked(int64_t now) {
    if (__atomic_load_n(&lt_idle, __ATOMIC_SEQ_CST) > 0 || !lt_any_work()) return;
    int n = __atomic_load_n(&lt_workers, __ATOMIC_ACQUIRE);
    bool stuck = false;
    for (int i = 0; i < n && !stuck; i++) {
        int64_t b = __atomic_load_n(&lt_ws[i].blocked_since, __ATOMIC_ACQUIRE);
        stuck = b && now - b > 10000000;
    }
    if (!stuck || n >= LT_MAX_WORKERS) return;
    lt_ws[n].seed = (uint32_t)n * 2654435761u + 1;
    __atomic_store_n(&lt_workers, n + 1, __ATOMIC_RELEASE);
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &a, lt_worker_thread, (void *)(intptr_t)n);
}

// Set by an HTTP server: called about once a second (holding lt_timer_mu)
// to close connections past their timeouts.
static void (*lt_sweep_hook)(int64_t now);
static int64_t lt_sweep_last;

static void *lt_timer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lt_timer_mu);
    for (;;) {
        int64_t now = lt_monotonic_nanos();
        int64_t next = INT64_MAX;
        if (__atomic_load_n(&lt_block_watch, __ATOMIC_ACQUIRE)) {
            lt_check_blocked(now);
            if (now + 10000000 < next) next = now + 10000000;
        }
        if (lt_sweep_hook) {
            if (now - lt_sweep_last >= 1000000000) {
                lt_sweep_last = now;
                lt_sweep_hook(now);
            }
            next = lt_sweep_last + 1000000000;
        }
        for (lt_deadline **p = &lt_deadlines; *p;) {
            lt_deadline *d = *p;
            if (d->at <= now) {
                *p = d->next;
                d->fired = 1;
                // wake first, then stop counting: in between, the deadlock
                // check must still see a reason to wait
                lt_task_cancel(d->task);
                __atomic_fetch_sub(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
                continue;
            }
            if (d->at < next) next = d->at;
            p = &d->next;
        }
        lt_spin_lock(&lt_timer_spin);
        lt_task *t = lt_timer_q.head;
        while (t) {
            lt_task *n = t->next;
            if (t->wake_at <= now) {
                lt_wq_remove(&lt_timer_q, t);
                __atomic_store_n(&t->parked_on, NULL, __ATOMIC_RELEASE);
                t->timed_out = 1;
                lt_ready(t);
                __atomic_fetch_sub(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
            } else if (t->wake_at < next) {
                next = t->wake_at;
            }
            t = n;
        }
        lt_spin_unlock(&lt_timer_spin);
        lt_timer_wait(next == INT64_MAX ? -1 : next - now);
    }
    return NULL;
}

static void lt_timer_cancel(lt_task *t) { (void)t; } // cancel() already removes it via parked_on

// Cancels `t` after `ns`; the handle goes to lt_deadline_stop exactly once.
static int64_t lt_cancel_after(lt_task *t, int64_t ns) {
    lt_deadline *d = (lt_deadline *)calloc(1, sizeof(lt_deadline));
    d->at = lt_monotonic_nanos() + (ns > 0 ? ns : 0);
    d->task = t;
    lt_task_dup(t);
    // a pending deadline can still wake things up: not a deadlock
    __atomic_fetch_add(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_lock(&lt_timer_mu);
    d->next = lt_deadlines;
    lt_deadlines = d;
    lt_timer_kick();
    pthread_mutex_unlock(&lt_timer_mu);
    return (int64_t)(intptr_t)d;
}

// Stops the deadline; true if it had already fired.
static bool lt_deadline_stop(int64_t handle) {
    lt_deadline *d = (lt_deadline *)(intptr_t)handle;
    pthread_mutex_lock(&lt_timer_mu);
    bool fired = d->fired;
    if (!fired) {
        for (lt_deadline **p = &lt_deadlines; *p; p = &(*p)->next) {
            if (*p == d) {
                *p = d->next;
                break;
            }
        }
        __atomic_fetch_sub(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
    }
    pthread_mutex_unlock(&lt_timer_mu);
    lt_task_drop(d->task);
    free(d);
    return fired;
}

static lt_err lt_sleep_nanos(int64_t ns) {
    lt_task *t = lt_current();
    if (lt_is_cancelled()) return lt_make_cancelled();
    if (ns <= 0) return (lt_err){ 0 };
    t->wake_at = lt_monotonic_nanos() + ns;
    t->timed_out = 0;
    __atomic_fetch_add(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
    // lock order everywhere: the timer mutex, then the timer queue
    pthread_mutex_lock(&lt_timer_mu);
    lt_spin_lock(&lt_timer_spin);
    lt_timer_kick();
    pthread_mutex_unlock(&lt_timer_mu);
    lt_park_on(&lt_timer_q, true);
    t = lt_current();
    if (!t->timed_out) {
        __atomic_fetch_sub(&lt_sleepers, 1, __ATOMIC_ACQ_REL);
        return lt_make_cancelled();
    }
    return (lt_err){ 0 };
}

// ---------------------------------------------------------------- spawn, wait, scopes

static lt_scope *lt_scope_new(void) {
    lt_scope *s = (lt_scope *)calloc(1, sizeof(lt_scope));
    lt_task *me = lt_current();
    if (me) {
        lt_spin_lock(&me->lock);
        s->parent_link = me->own_scope;
        me->own_scope = s;
        lt_spin_unlock(&me->lock);
    }
    return s;
}

static lt_task *lt_spawn(lt_scope *s, lt_fn fn, size_t result_size, void (*run)(lt_task *), void (*drop_result)(void *)) {
    lt_task *t = lt_task_new(fn, result_size, run, drop_result);
    t->rc = 3; // the Task value, the scope, the running task
    lt_task *me = lt_current();
    if (me) lt_spin_lock(&me->lock);
    t->scope = s;
    t->scope_prev = NULL;
    t->scope_next = s->tasks;
    if (s->tasks) s->tasks->scope_prev = t;
    s->tasks = t;
    if (me) lt_spin_unlock(&me->lock);
    if (me && __atomic_load_n(&me->cancelled, __ATOMIC_ACQUIRE)) lt_task_cancel(t);
    lt_ready_on(t, false);
    return t;
}

// wait until `t` is done (not interruptible: a cancelled parent cancels
// its children, which then finish quickly)
static void lt_join(lt_task *t) {
    lt_spin_lock(&t->lock);
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == LT_DONE) {
        lt_spin_unlock(&t->lock);
        return;
    }
    lt_park_on(&t->done_q, false);
}

// the error of a finished task (a new reference), or none
// A task that was waited for leaves its function's scope (the scope keeps
// only what it must still wait for): a loop that spawns and waits doesn't
// pile up finished tasks until the function returns.
static void lt_scope_forget(lt_task *t) {
    lt_scope *s = t->scope;
    if (!s) return;
    lt_task *me = lt_current();
    if (me) lt_spin_lock(&me->lock);
    if (t->scope_prev) t->scope_prev->scope_next = t->scope_next;
    else s->tasks = t->scope_next;
    if (t->scope_next) t->scope_next->scope_prev = t->scope_prev;
    t->scope = NULL;
    t->scope_next = t->scope_prev = NULL;
    if (me) lt_spin_unlock(&me->lock);
    lt_task_drop(t); // the scope's reference
}

static lt_err lt_task_outcome(lt_task *t) {
    lt_join(t);
    t->observed = 1;
    lt_scope_forget(t);
    if (t->error.obj) {
        lt_iface_dup(t->error);
        return t->error;
    }
    return (lt_err){ 0 };
}

// End of a function that started tasks: wait for all of them. The first
// error nobody waited for comes out of the function.
static lt_err lt_scope_end(lt_scope *s, bool cancel) {
    lt_task *me = lt_current();
    if (cancel) lt_cancel_scope(s);
    lt_err first = { 0 };
    for (lt_task *t = s->tasks; t; t = t->scope_next) {
        lt_join(t);
        if (!t->observed && t->error.obj && !first.obj) {
            // a cancelled child's `Cancelled` is not news
            if (!(cancel && t->cancelled)) {
                lt_iface_dup(t->error);
                first = t->error;
                // the others are stopped: one failure fails the function
                lt_cancel_scope(s);
            }
        }
    }
    if (me) lt_spin_lock(&me->lock);
    lt_task *t = s->tasks;
    s->tasks = NULL;
    if (me) {
        me->own_scope = s->parent_link;
        lt_spin_unlock(&me->lock);
    }
    while (t) {
        lt_task *n = t->scope_next;
        lt_task_drop(t);
        t = n;
    }
    free(s);
    return first;
}

// ---------------------------------------------------------------- shared state

// A readers-writer lock. `with v = s.lock()` is the writer: alone.
// `with v = s.read()` is a reader: readers run at the same time.
//
// Readers count themselves in per-worker slots (each on its own cache line),
// so readers on different cores don't touch a common word. A writer raises
// `wflag` (holding or waiting), which sends new readers to the slow path,
// and waits until the slots add up to zero. Both sides use sequentially
// consistent operations: a reader adds itself and then reads `wflag`; a
// writer sets `wflag` and then reads the slots, so at least one of them
// sees the other.
//
// Fairness: a waiting writer stops new readers; when a writer is done, the
// readers that waited meanwhile all go in before the next writer. Neither
// side can starve the other.
#define LT_RW_SLOTS 16
typedef struct { int64_t n; char pad[56]; } lt_rw_slot;

typedef struct lt_lock {
    lt_spin spin;
    int locked;         // a writer holds it
    int writers;        // writers holding or waiting (under spin)
    int wflag;          // writers != 0, read without the spin lock
    lt_waitq q;         // writers waiting
    lt_waitq rq;        // readers waiting
    lt_rw_slot *slots;  // reader counts; made by the first reader
} lt_lock;

// the locks a task holds are remembered so a panic that ends only its
// request can release them; a read lock is marked in the pointer's low bit
static void lt_lock_held_by(lt_task *t, lt_lock *l, bool add, bool read) {
    if (!t) return;
    lt_lock *k = (lt_lock *)((uintptr_t)l | (read ? 1 : 0));
    if (add) {
        if (t->nheld < 8) t->held[t->nheld++] = k;
    } else {
        for (int i = t->nheld - 1; i >= 0; i--)
            if (t->held[i] == k) {
                t->held[i] = t->held[--t->nheld];
                break;
            }
    }
}
static void lt_lock_held(lt_lock *l, bool add, bool read) { lt_lock_held_by(lt_current(), l, add, read); }

static void lt_lock_init(lt_lock *l) {
    memset(l, 0, sizeof *l);
    l->q.lock = &l->spin;
    l->rq.lock = &l->spin;
}
static void lt_lock_free(lt_lock *l) {
    if (l->slots) free(l->slots);
}

static int64_t lt_rw_readers(lt_lock *l) {
    lt_rw_slot *s = __atomic_load_n(&l->slots, __ATOMIC_RELAXED); // (callers hold the spin lock)
    if (!s) return 0;
    int64_t n = 0;
    for (int i = 0; i < LT_RW_SLOTS; i++) n += __atomic_load_n(&s[i].n, __ATOMIC_SEQ_CST);
    return n;
}
// The slots are made by the first reader, under the spin lock. Until then
// no reader has come, and writers skip `wflag` (and its fence) altogether;
// making them sets `wflag` for the writers there are.
__attribute__((noinline)) static lt_rw_slot *lt_rw_make_slots(lt_lock *l) {
    lt_spin_lock(&l->spin);
    lt_rw_slot *s = l->slots;
    if (!s) {
        if (posix_memalign((void **)&s, 64, sizeof(lt_rw_slot) * LT_RW_SLOTS) != 0) lt_oom();
        memset(s, 0, sizeof(lt_rw_slot) * LT_RW_SLOTS);
        __atomic_store_n(&l->wflag, l->writers ? 1 : 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&l->slots, s, __ATOMIC_SEQ_CST);
    }
    lt_spin_unlock(&l->spin);
    return s;
}
// the slot of the worker running task `t` (a task that moved to another
// worker meanwhile leaves from another slot: only the sum counts)
LT_INLINE lt_rw_slot *lt_rw_slot_of(lt_lock *l, lt_task *t) {
    lt_rw_slot *s = __atomic_load_n(&l->slots, __ATOMIC_ACQUIRE);
    if (LT_UNLIKELY(!s)) s = lt_rw_make_slots(l);
    return &s[t ? (unsigned)t->worker % LT_RW_SLOTS : 0];
}

// (spin held) the lock is free of readers and writers: give it to the
// first waiting writer
static lt_task *lt_rw_hand_to_writer(lt_lock *l) {
    if (!l->q.head || l->locked || lt_rw_readers(l) != 0) return NULL;
    lt_task *t = lt_wq_pop(&l->q);
    if (t) l->locked = 1;
    return t;
}

// (spin held) a writer is done and readers waited: they all go in, counted
// in here (kept out of line: the common path stays small)
__attribute__((noinline)) static void lt_rw_admit_readers(lt_lock *l) {
    int64_t n = 0;
    for (lt_task *t = l->rq.head; t; t = t->next) n++;
    __atomic_fetch_add(&lt_rw_slot_of(l, NULL)->n, n, __ATOMIC_SEQ_CST);
    lt_wake_all(&l->rq);
}

static void lt_lock_acquire(lt_lock *l) {
    lt_spin_lock(&l->spin);
    l->writers++;
    if (l->slots) __atomic_store_n(&l->wflag, 1, __ATOMIC_SEQ_CST);
    if (!l->locked && lt_rw_readers(l) == 0) {
        l->locked = 1;
        lt_spin_unlock(&l->spin);
        lt_lock_held(l, true, false);
        return;
    }
    // the last reader out, or the writer before, hands the lock over
    lt_park_on(&l->q, false);
    lt_lock_held(l, true, false);
}
static void lt_lock_release(lt_lock *l) {
    lt_lock_held(l, false, false);
    lt_spin_lock(&l->spin);
    l->writers--;
    l->locked = 0;
    lt_task *w = NULL;
    if (l->rq.head) lt_rw_admit_readers(l); // the readers that waited go first
    else w = lt_rw_hand_to_writer(l);
    if (l->writers == 0 && l->slots) __atomic_store_n(&l->wflag, 0, __ATOMIC_RELEASE);
    lt_spin_unlock(&l->spin);
    if (w) lt_ready(w);
}

// a reader leaves: if a writer waits and this was the last reader, the
// writer gets the lock
static void lt_rw_read_leave(lt_lock *l, lt_task *t) {
    __atomic_fetch_sub(&lt_rw_slot_of(l, t)->n, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&l->wflag, __ATOMIC_SEQ_CST)) return;
    lt_spin_lock(&l->spin);
    lt_task *w = lt_rw_hand_to_writer(l);
    lt_spin_unlock(&l->spin);
    if (w) lt_ready(w);
}

static void lt_lock_acquire_read(lt_lock *l) {
    lt_task *t = lt_current();
    for (;;) {
        __atomic_fetch_add(&lt_rw_slot_of(l, t)->n, 1, __ATOMIC_SEQ_CST);
        if (LT_LIKELY(!__atomic_load_n(&l->wflag, __ATOMIC_SEQ_CST))) break;
        // a writer holds the lock or waits for it: step back and wait
        lt_rw_read_leave(l, t);
        lt_spin_lock(&l->spin);
        if (!l->writers) {
            lt_spin_unlock(&l->spin);
            continue;
        }
        // the writer's release counts this reader in and wakes it
        lt_park_on(&l->rq, false);
        t = lt_current();
        break;
    }
    lt_lock_held_by(t, l, true, true);
}
static void lt_lock_release_read(lt_lock *l) {
    lt_task *t = lt_current();
    lt_lock_held_by(t, l, false, true);
    lt_rw_read_leave(l, t);
}

// after a panic that ends only a request: whatever the task still holds
static void lt_lock_release_held(lt_lock *k) {
    if ((uintptr_t)k & 1) lt_lock_release_read((lt_lock *)((uintptr_t)k & ~(uintptr_t)1));
    else lt_lock_release(k);
}

// ---------------------------------------------------------------- channels

typedef struct lt_chan {
    int64_t rc;
    lt_spin spin;
    lt_waitq recv_q, send_q;
    int64_t cap, count, head;
    int closed;
    size_t esize;
    void (*drop_item)(void *);
    char *buf;
} lt_chan;

static lt_chan *lt_chan_new(int64_t cap, size_t esize, void (*drop_item)(void *), int line) {
    if (cap < 1) lt_panic_at("a channel needs a capacity of at least 1", line);
    lt_chan *c = (lt_chan *)calloc(1, sizeof(lt_chan));
    c->rc = 1;
    c->recv_q.lock = &c->spin;
    c->send_q.lock = &c->spin;
    c->cap = cap;
    c->esize = esize;
    c->drop_item = drop_item;
    c->buf = (char *)malloc(esize * (size_t)cap);
    return c;
}
static void lt_chan_free(lt_chan *c) {
    for (int64_t i = 0; i < c->count; i++) c->drop_item(c->buf + ((c->head + i) % c->cap) * c->esize);
    free(c->buf);
    free(c);
}
LT_INLINE void lt_chan_dup(lt_chan *c) {
    if (c) __atomic_fetch_add(&c->rc, 1, __ATOMIC_RELAXED);
}
LT_INLINE void lt_chan_drop(lt_chan *c) {
    if (c && __atomic_sub_fetch(&c->rc, 1, __ATOMIC_ACQ_REL) == 0) lt_chan_free(c);
}

// takes ownership of *item
static lt_err lt_chan_send(lt_chan *c, void *item) {
    for (;;) {
        lt_spin_lock(&c->spin);
        if (c->closed) {
            lt_spin_unlock(&c->spin);
            c->drop_item(item);
            return lt_make_channel_closed();
        }
        if (lt_is_cancelled()) {
            lt_spin_unlock(&c->spin);
            c->drop_item(item);
            return lt_make_cancelled();
        }
        if (c->count < c->cap) {
            memcpy(c->buf + ((c->head + c->count) % c->cap) * c->esize, item, c->esize);
            c->count++;
            lt_task *r = lt_wq_pop(&c->recv_q);
            lt_spin_unlock(&c->spin);
            if (r) lt_ready_on(r, true);
            return (lt_err){ 0 };
        }
        lt_park_on(&c->send_q, true);
    }
}

// takes ownership of *item; true if it was queued, false (and dropped) if
// the channel is full or closed. Never waits.
static bool lt_chan_try_send(lt_chan *c, void *item) {
    lt_spin_lock(&c->spin);
    if (c->closed || c->count >= c->cap) {
        lt_spin_unlock(&c->spin);
        c->drop_item(item);
        return false;
    }
    memcpy(c->buf + ((c->head + c->count) % c->cap) * c->esize, item, c->esize);
    c->count++;
    lt_task *r = lt_wq_pop(&c->recv_q);
    lt_spin_unlock(&c->spin);
    if (r) lt_ready_on(r, true);
    return true;
}

// 1: got an item into *out; 0: closed (or cancelled when `stop_on_cancel`)
static int lt_chan_recv(lt_chan *c, void *out) {
    for (;;) {
        lt_spin_lock(&c->spin);
        if (c->count > 0) {
            memcpy(out, c->buf + c->head * c->esize, c->esize);
            c->head = (c->head + 1) % c->cap;
            c->count--;
            lt_task *s = lt_wq_pop(&c->send_q);
            lt_spin_unlock(&c->spin);
            if (s) lt_ready_on(s, true);
            return 1;
        }
        if (c->closed || lt_is_cancelled()) {
            lt_spin_unlock(&c->spin);
            return 0;
        }
        lt_park_on(&c->recv_q, true);
    }
}

// A value if one is waiting; never waits.
static int lt_chan_try_recv(lt_chan *c, void *out) {
    lt_spin_lock(&c->spin);
    if (c->count == 0) {
        lt_spin_unlock(&c->spin);
        return 0;
    }
    memcpy(out, c->buf + c->head * c->esize, c->esize);
    c->head = (c->head + 1) % c->cap;
    c->count--;
    lt_task *s = lt_wq_pop(&c->send_q);
    lt_spin_unlock(&c->spin);
    if (s) lt_ready_on(s, true);
    return 1;
}

static void lt_chan_close(lt_chan *c) {
    lt_spin_lock(&c->spin);
    c->closed = 1;
    lt_task *t;
    while ((t = lt_wq_pop(&c->recv_q))) lt_ready(t);
    while ((t = lt_wq_pop(&c->send_q))) lt_ready(t);
    lt_spin_unlock(&c->spin);
}

// ---------------------------------------------------------------- waiting for sockets

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define LT_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

static int lt_poll_fd = -1;
static pthread_once_t lt_poll_once = PTHREAD_ONCE_INIT;

// One record per file descriptor, never freed: the kernel's event points
// here, not at a task. Whoever takes `waiter` (under `spin`) wakes the task:
// the poller on an event, or lt_task_cancel. So a task is never touched
// after someone else woke it, and a stale event only wakes the next waiter
// for nothing (it retries its read or write).
struct lt_fdesc {
    lt_spin spin;
    lt_task *waiter;
};

#define LT_FD_CHUNK 1024
static lt_fdesc *lt_fd_chunks[1 << 12]; // up to 4M descriptors
static lt_spin lt_fd_grow;

static lt_fdesc *lt_fdesc_of(int fd) {
    if (fd < 0 || fd >= LT_FD_CHUNK * (1 << 12)) return NULL;
    int c = fd / LT_FD_CHUNK;
    lt_fdesc *chunk = __atomic_load_n(&lt_fd_chunks[c], __ATOMIC_ACQUIRE);
    if (!chunk) {
        lt_spin_lock(&lt_fd_grow);
        chunk = lt_fd_chunks[c];
        if (!chunk) {
            chunk = (lt_fdesc *)calloc(LT_FD_CHUNK, sizeof(lt_fdesc));
            __atomic_store_n(&lt_fd_chunks[c], chunk, __ATOMIC_RELEASE);
        }
        lt_spin_unlock(&lt_fd_grow);
    }
    return &chunk[fd % LT_FD_CHUNK];
}

// Takes the task waiting on `d` if it is `only` (or any, for NULL).
static lt_task *lt_fdesc_take(lt_fdesc *d, lt_task *only) {
    lt_spin_lock(&d->spin);
    lt_task *t = d->waiter;
    if (t && (!only || t == only)) d->waiter = NULL;
    else t = NULL;
    lt_spin_unlock(&d->spin);
    return t;
}

static void lt_io_wake(lt_task *t) {
    // the task is fully parked once its io_spin is free
    lt_spin_lock(&t->io_spin);
    lt_spin_unlock(&t->io_spin);
    // queued before it stops counting as waiting (see lt_next_task's
    // deadlock check); `t` isn't touched after lt_ready
    lt_ready(t);
    __atomic_fetch_sub(&lt_io_waiters, 1, __ATOMIC_ACQ_REL);
}

static void *lt_poll_thread(void *arg) {
    (void)arg;
    for (;;) {
#ifdef LT_KQUEUE
        struct kevent evs[64];
        int n = kevent(lt_poll_fd, NULL, 0, evs, 64, NULL);
        for (int i = 0; i < n; i++) {
            lt_fdesc *d = (lt_fdesc *)evs[i].udata;
#else
        struct epoll_event evs[64];
        int n = epoll_wait(lt_poll_fd, evs, 64, -1);
        for (int i = 0; i < n; i++) {
            lt_fdesc *d = (lt_fdesc *)evs[i].data.ptr;
#endif
            if (!d) continue;
            lt_task *t = lt_fdesc_take(d, NULL);
            if (t) lt_io_wake(t);
        }
    }
    return NULL;
}

static void lt_poll_start(void) {
#ifdef LT_KQUEUE
    lt_poll_fd = kqueue();
#else
    lt_poll_fd = epoll_create1(0);
#endif
    pthread_t th;
    pthread_create(&th, NULL, lt_poll_thread, NULL);
    pthread_detach(th);
}

// Parks the current task until `fd` can be read (or written). False when
// the task is cancelled (before or while waiting): the caller gives up.
// A wake-up can be spurious: the caller retries its read or write.
__attribute__((noinline)) static bool lt_io_wait(int fd, bool write) {
    pthread_once(&lt_poll_once, lt_poll_start);
    lt_task *t = lt_current();
    if (lt_is_cancelled()) return false;
    lt_fdesc *d = lt_fdesc_of(fd);
    lt_spin_lock(&t->io_spin);
    __atomic_fetch_add(&lt_io_waiters, 1, __ATOMIC_ACQ_REL);
    lt_spin_lock(&d->spin);
    d->waiter = t;
    lt_spin_unlock(&d->spin);
    __atomic_store_n(&t->io_fd, fd, __ATOMIC_RELEASE);
#ifdef LT_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, d);
    kevent(lt_poll_fd, &ev, 1, NULL, 0, NULL);
#else
    struct epoll_event ev;
    ev.events = (write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
    ev.data.ptr = d;
    if (epoll_ctl(lt_poll_fd, EPOLL_CTL_MOD, fd, &ev) != 0) epoll_ctl(lt_poll_fd, EPOLL_CTL_ADD, fd, &ev);
#endif
    __atomic_store_n(&t->state, LT_PARKED, __ATOMIC_RELAXED);
    lt_release_after = &t->io_spin;
    // cancelled while registering: don't sleep through it
    if (__atomic_load_n(&t->cancelled, __ATOMIC_ACQUIRE)) {
        lt_task *mine = lt_fdesc_take(d, t);
        if (mine) {
            __atomic_store_n(&t->io_fd, -1, __ATOMIC_RELEASE);
            __atomic_fetch_sub(&lt_io_waiters, 1, __ATOMIC_ACQ_REL);
            lt_release_after = NULL;
            lt_spin_unlock(&t->io_spin);
            __atomic_store_n(&t->state, LT_RUNNING, __ATOMIC_RELAXED);
            return false;
        }
    }
    lt_to_worker(t);
    t = lt_current();
    __atomic_store_n(&t->io_fd, -1, __ATOMIC_RELEASE);
    return !lt_is_cancelled();
}

// A task that nobody waits for (a connection of a server).
static void lt_spawn_detached(void (*run)(lt_task *), const void *arg, size_t arg_size) {
    lt_task *t = lt_task_new((lt_fn){ 0 }, arg_size, run, NULL);
    memcpy(t->result, arg, arg_size);
    t->rc = 1; // only the running reference
    lt_ready(t);
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#endif
#endif

// set by db.h: rolls back the task's open transactions
static void (*lt_tx_abort_hook)(lt_task *t);

// A panic in a task that set panic_jmp (an HTTP handler) ends only that task's
// current request: its locks are released and control returns to the setjmp.
static void lt_task_panic_hook(const char *msg, int line) {
    lt_task *t = lt_current();
    if (!t || !t->panic_jmp) return;
    fflush(stdout);
    if (line > 0) fprintf(stderr, "panic: %s\n  at %s:%d\n", msg, lt_file, line);
    else fprintf(stderr, "panic: %s\n", msg);
    while (t->nheld > 0) lt_lock_release_held(t->held[t->nheld - 1]);
    if (t->ntxs && lt_tx_abort_hook) lt_tx_abort_hook(t);
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    // AddressSanitizer clears the frames a longjmp skips only on a thread's
    // own stack; on a task's stack, clear them here
    {
        char here;
        __asan_unpoison_memory_region(&here, (size_t)((char *)t->panic_jmp - &here));
    }
#endif
#endif
    longjmp(*(jmp_buf *)t->panic_jmp, 1);
}

// ---------------------------------------------------------------- start



// Ctrl-C with tasks: cancel main's task (and so all of them) from a
// thread, since a signal handler can't take locks.
static void *lt_interrupt_thread(void *u) {
    (void)u;
    char x;
    while (read(lt_interrupt_pipe[0], &x, 1) < 0 && errno == EINTR) {
    }
    if (lt_main_task) lt_task_cancel(lt_main_task);
    return NULL;
}

// Run `entry` as the first task, on a pool of worker threads.
static lt_task *lt_run_main(lt_fn entry, size_t result_size, void (*run)(lt_task *)) {
    lt_task *m = lt_task_new(entry, result_size, run, NULL);
    m->rc = 2; // returned to the caller + running
    lt_main_task = m;
    lt_panic_hook = lt_task_panic_hook;
    lt_overflow_hook = lt_task_overflowed;
    lt_grow_hook = lt_task_grow;
    if (pipe(lt_interrupt_pipe) == 0) {
        pthread_t it;
        pthread_create(&it, NULL, lt_interrupt_thread, NULL);
        pthread_detach(it);
        lt_catch_interrupts();
    }
    lt_workers = lt_ncpu();
    if (lt_workers > LT_MAX_WORKERS / 2) lt_workers = LT_MAX_WORKERS / 2;
    lt_ws = (lt_worker *)calloc(LT_MAX_WORKERS, sizeof(lt_worker));
    for (int i = 0; i < lt_workers; i++) lt_ws[i].seed = (uint32_t)i * 2654435761u + 1;
    lt_self = &lt_ws[0];
    lt_timer_setup();
    pthread_t timer;
    pthread_create(&timer, NULL, lt_timer_thread, NULL);
    pthread_detach(timer);
    lt_ready(m);
    int first = lt_workers; // spares started later are detached
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)first);
    for (int i = 1; i < first; i++) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 1 << 20);
        pthread_create(&threads[i], &a, lt_worker_thread, (void *)(intptr_t)i);
    }
    // the main thread is a worker too
    lt_worker_loop();
    for (int i = 1; i < first; i++) pthread_join(threads[i], NULL);
    free(threads);
    return m;
}
