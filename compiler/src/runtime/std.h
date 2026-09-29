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
