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
    lt_scope *own_scope;                   // the scope of the function running in it
    int64_t wake_at;                       // sleeping: deadline (ns)
    int timed_out;
    int io_fd;                             // waiting for this descriptor (-1: not)
    void *fiber;                           // ThreadSanitizer's view of the task
    lt_spin io_spin;                       // held while registering for I/O
    void *panic_jmp;                       // a jmp_buf: a panic ends this request only
    struct lt_lock *held[8];               // locks held, released after such a panic
    int nheld;
    char result[] __attribute__((aligned(16)));
} lt_task;

// A task's stack: 8 MB, like a program's main thread. Only the pages a task
// touches take memory.
#define LT_STACK_SIZE ((size_t)8 << 20)
#define LT_GUARD ((size_t)16384)
// A stack back in the pool gives its deep pages back to the system if the
// task went deeper than this (a marker word there was overwritten).
#define LT_STACK_KEEP ((size_t)256 << 10)
#define LT_STACK_MARK 0x5ac4ed5ac4ed5ac4ULL

static pthread_mutex_t lt_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lt_q_cv = PTHREAD_COND_INITIALIZER;
static lt_task *lt_q_head, *lt_q_tail;
static int lt_workers, lt_idle, lt_shutdown, lt_sleepers, lt_io_waiters;
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

static char *lt_stack_get(void) {
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
    char *s = (char *)mmap(NULL, LT_STACK_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (s == MAP_FAILED) lt_oom();
    mprotect(s, LT_GUARD, PROT_NONE); // overflow hits the guard page
    *(uint64_t *)(s + LT_STACK_SIZE - LT_STACK_KEEP) = LT_STACK_MARK;
    return s;
}
static void lt_stack_put(char *s) {
    uint64_t *mark = (uint64_t *)(s + LT_STACK_SIZE - LT_STACK_KEEP);
    if (*mark != LT_STACK_MARK) {
        madvise(s + LT_GUARD, LT_STACK_SIZE - LT_STACK_KEEP - LT_GUARD, MADV_DONTNEED);
        *mark = LT_STACK_MARK;
    }
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

static void lt_ready(lt_task *t) {
    pthread_mutex_lock(&lt_q_mu);
    __atomic_store_n(&t->state, LT_READY, __ATOMIC_RELAXED);
    t->next = NULL;
    if (lt_q_tail) lt_q_tail->next = t;
    else lt_q_head = t;
    lt_q_tail = t;
    if (lt_idle) pthread_cond_signal(&lt_q_cv);
    pthread_mutex_unlock(&lt_q_mu);
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
    pthread_mutex_lock(&lt_q_mu);
    while (!lt_q_head) {
        if (lt_shutdown) {
            pthread_mutex_unlock(&lt_q_mu);
            return NULL;
        }
        lt_idle++;
        if (lt_idle == lt_workers && __atomic_load_n(&lt_sleepers, __ATOMIC_ACQUIRE) == 0 && __atomic_load_n(&lt_io_waiters, __ATOMIC_ACQUIRE) == 0) {
            pthread_mutex_unlock(&lt_q_mu);
            lt_deadlock();
        }
        pthread_cond_wait(&lt_q_cv, &lt_q_mu);
        lt_idle--;
    }
    lt_task *t = lt_q_head;
    lt_q_head = t->next;
    if (!lt_q_head) lt_q_tail = NULL;
    t->next = NULL;
    pthread_mutex_unlock(&lt_q_mu);
    return t;
}

static lt_task *lt_main_task;

// Runs tasks until the main task is done (on whichever thread it finishes).
static void lt_worker_loop(void) {
    for (;;) {
        lt_task *t = lt_next_task();
        if (!t) return;
        lt_cur = t;
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
            lt_stack_put(f->stack);
            f->stack = NULL;
#ifdef LT_TSAN
            __tsan_destroy_fiber(f->fiber);
#endif
            bool is_main = f == lt_main_task;
            lt_task_drop(f); // the reference held while running
            if (is_main) {
                pthread_mutex_lock(&lt_q_mu);
                lt_shutdown = 1;
                pthread_cond_broadcast(&lt_q_cv);
                pthread_mutex_unlock(&lt_q_mu);
                return;
            }
        }
    }
}

static void *lt_worker_thread(void *arg) {
    (void)arg;
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

static void *lt_timer_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lt_timer_mu);
    for (;;) {
        int64_t now = lt_monotonic_nanos();
        int64_t next = INT64_MAX;
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
    t->scope_next = s->tasks;
    s->tasks = t;
    if (me) lt_spin_unlock(&me->lock);
    if (me && __atomic_load_n(&me->cancelled, __ATOMIC_ACQUIRE)) lt_task_cancel(t);
    lt_ready(t);
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
static lt_err lt_task_outcome(lt_task *t) {
    lt_join(t);
    t->observed = 1;
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

typedef struct lt_lock {
    lt_spin spin;
    int locked;
    lt_waitq q;
} lt_lock;

static void lt_lock_held(lt_lock *l, bool add) {
    lt_task *t = lt_current();
    if (!t) return;
    if (add) {
        if (t->nheld < 8) t->held[t->nheld++] = l;
    } else {
        for (int i = t->nheld - 1; i >= 0; i--)
            if (t->held[i] == l) {
                t->held[i] = t->held[--t->nheld];
                break;
            }
    }
}

static void lt_lock_init(lt_lock *l) {
    memset(l, 0, sizeof *l);
    l->q.lock = &l->spin;
}
static void lt_lock_acquire(lt_lock *l) {
    lt_spin_lock(&l->spin);
    if (!l->locked) {
        l->locked = 1;
        lt_spin_unlock(&l->spin);
        lt_lock_held(l, true);
        return;
    }
    // the releaser hands the lock over directly
    lt_park_on(&l->q, false);
    lt_lock_held(l, true);
}
static void lt_lock_release(lt_lock *l) {
    lt_lock_held(l, false);
    lt_spin_lock(&l->spin);
    lt_task *t = lt_wq_pop(&l->q);
    if (!t) l->locked = 0;
    lt_spin_unlock(&l->spin);
    if (t) lt_ready(t);
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
            if (r) lt_ready(r);
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
    if (r) lt_ready(r);
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
            if (s) lt_ready(s);
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
    if (s) lt_ready(s);
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

// A panic in a task that set panic_jmp (an HTTP handler) ends only that task's
// current request: its locks are released and control returns to the setjmp.
static void lt_task_panic_hook(const char *msg, int line) {
    lt_task *t = lt_current();
    if (!t || !t->panic_jmp) return;
    fflush(stdout);
    if (line > 0) fprintf(stderr, "panic: %s\n  at %s:%d\n", msg, lt_file, line);
    else fprintf(stderr, "panic: %s\n", msg);
    while (t->nheld > 0) lt_lock_release(t->held[t->nheld - 1]);
    longjmp(*(jmp_buf *)t->panic_jmp, 1);
}

// ---------------------------------------------------------------- start

static int lt_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}

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
    if (pipe(lt_interrupt_pipe) == 0) {
        pthread_t it;
        pthread_create(&it, NULL, lt_interrupt_thread, NULL);
        pthread_detach(it);
        lt_catch_interrupts();
    }
    lt_workers = lt_ncpu();
    lt_timer_setup();
    pthread_t timer;
    pthread_create(&timer, NULL, lt_timer_thread, NULL);
    pthread_detach(timer);
    lt_ready(m);
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)lt_workers);
    for (int i = 1; i < lt_workers; i++) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 1 << 20);
        pthread_create(&threads[i], &a, lt_worker_thread, NULL);
    }
    // the main thread is a worker too
    lt_worker_loop();
    for (int i = 1; i < lt_workers; i++) pthread_join(threads[i], NULL);
    free(threads);
    return m;
}
