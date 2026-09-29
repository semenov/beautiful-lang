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
    int kind = e == ENOENT ? 1 : (e == EACCES || e == EPERM) ? 2 : e == EISDIR ? 3 : e == EEXIST ? 4 : 0;
    if (!path) kind = 0;
    return lt_make_file_error(kind, path, lt_text_cstr(buf));
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
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && S_ISDIR(st.st_mode)) {
        fclose(f);
        errno = EISDIR;
        return lt_os_error("can't read", path);
    }
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

// size, Unix permission bits, modification time (seconds since 1970), is a directory
static lt_err lt_files_stat(lt_text *path, int64_t *f) {
    struct stat st;
    if (stat(path->data, &st) != 0) return lt_os_error("can't read the details of", path);
    f[0] = (int64_t)st.st_size;
    f[1] = (int64_t)(st.st_mode & 07777);
#ifdef __APPLE__
    f[2] = (int64_t)st.st_mtimespec.tv_sec;
#else
    f[2] = (int64_t)st.st_mtim.tv_sec;
#endif
    f[3] = S_ISDIR(st.st_mode) ? 1 : 0;
    return (lt_err){ 0 };
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
#ifdef LT_DEBUG_ALLOC
    lt_exited_early = 1;
#endif
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

static int64_t lt_unix_millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

extern char **environ;
// "NAME=value" for every environment variable
static lt_texts *lt_environ(void) {
    lt_texts *l = lt_texts_new(32);
    for (char **e = environ; e && *e; e++) lt_texts_push(&l, lt_text_cstr(*e));
    return l;
}

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

// `lang test` collects each test's log lines here and shows them only if
// the test fails
static lt_buf *lt_log_capture;

// a failing test's log, each line indented under it
static void lt_print_indented(const char *d, int64_t n) {
    int64_t start = 0;
    for (int64_t i = 0; i < n; i++) {
        if (d[i] == '\n') {
            printf("        %.*s\n", (int)(i - start), d + start);
            start = i + 1;
        }
    }
}

// a field value in text logs: bare when it's one plain word, else quoted
static void lt_log_value(lt_buf *b, lt_text *v) {
    bool plain = v->len > 0;
    for (int64_t i = 0; i < v->len && plain; i++) {
        unsigned char c = (unsigned char)v->data[i];
        if (c <= ' ' || c == '"' || c == '=' || c == '\\' || c == 127) plain = false;
    }
    if (plain) lt_buf_put(b, v->data, v->len);
    else lt_json_str(b, v->data, v->len);
}

// `keys` / `vals`: fields (may be NULL)
static void lt_log_fields(const char *level, lt_text *msg, lt_texts *keys, lt_texts *vals) {
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
    int64_t nf = keys && vals ? (keys->len < vals->len ? keys->len : vals->len) : 0;
    lt_buf b = { 0 };
    if (json) {
        snprintf(stamp + sl, sizeof stamp - sl, ".%03ldZ", (long)(ts.tv_nsec / 1000000));
        lt_buf_put(&b, "{\"time\":\"", 9);
        lt_buf_put(&b, stamp, (int64_t)strlen(stamp));
        lt_buf_put(&b, "\",\"level\":\"", 11);
        for (const char *c = level; *c; c++) lt_buf_c(&b, (char)tolower((unsigned char)*c));
        lt_buf_put(&b, "\",\"message\":", 12);
        lt_json_str(&b, msg->data, msg->len);
        for (int64_t i = 0; i < nf; i++) {
            lt_buf_c(&b, ',');
            lt_json_str(&b, keys->items[i]->data, keys->items[i]->len);
            lt_buf_c(&b, ':');
            lt_json_str(&b, vals->items[i]->data, vals->items[i]->len);
        }
        lt_buf_put(&b, "}\n", 2);
    } else {
        lt_buf_put(&b, stamp, (int64_t)sl);
        lt_buf_put(&b, "Z ", 2);
        lt_buf_put(&b, level, (int64_t)strlen(level));
        lt_buf_c(&b, ' ');
        lt_buf_put(&b, msg->data, msg->len);
        for (int64_t i = 0; i < nf; i++) {
            lt_buf_c(&b, ' ');
            lt_buf_put(&b, keys->items[i]->data, keys->items[i]->len);
            lt_buf_c(&b, '=');
            lt_log_value(&b, vals->items[i]);
        }
        lt_buf_c(&b, '\n');
    }
    fflush(stdout);
    flockfile(stderr);
    if (lt_log_capture) lt_buf_put(lt_log_capture, b.d, b.len);
    else fwrite(b.d, 1, (size_t)b.len, stderr);
    funlockfile(stderr);
    lt_buf_free(&b);
}

static void lt_log(const char *level, lt_text *msg) {
    lt_log_fields(level, msg, NULL, NULL);
}

// log.Logger: the level as text
static void lt_log_named(lt_text *level, lt_text *msg, lt_texts *keys, lt_texts *vals) {
    lt_log_fields(level->data, msg, keys, vals);
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

// MD5: only for protocols that require it (HTTP digest auth, ETags,
// checksums of old formats); broken for anything that needs security.
static lt_bytes *lt_crypto_md5(lt_bytes *d) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    uint64_t bits = (uint64_t)d->len * 8;
    int64_t total = ((d->len + 9 + 63) / 64) * 64;
    unsigned char *m = (unsigned char *)calloc((size_t)total, 1);
    memcpy(m, d->data, (size_t)d->len);
    m[d->len] = 0x80;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (unsigned char)(bits >> (8 * i));
    for (int64_t off = 0; off < total; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] | (uint32_t)m[off + 4 * i + 1] << 8 | (uint32_t)m[off + 4 * i + 2] << 16 | (uint32_t)m[off + 4 * i + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3];
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & dd); g = i; }
            else if (i < 32) { f = (dd & b) | (~dd & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ dd; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~dd); g = (7 * i) % 16; }
            uint32_t tmp = dd;
            dd = c;
            c = b;
            uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd;
    }
    free(m);
    lt_bytes *out = lt_bytes_new(16);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out->data[4 * i + j] = (unsigned char)(h[i] >> (8 * j));
    out->len = 16;
    return out;
}

// SHA-1: only for protocols that require it (WebSocket handshakes, git);
// it isn't safe against collisions: use sha256 for anything new.
static lt_bytes *lt_crypto_sha1(lt_bytes *d) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint64_t bits = (uint64_t)d->len * 8;
    int64_t total = ((d->len + 9 + 63) / 64) * 64;
    unsigned char *m = (unsigned char *)calloc((size_t)total, 1);
    memcpy(m, d->data, (size_t)d->len);
    m[d->len] = 0x80;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (int64_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & dd), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ dd, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & dd) | (c & dd), k = 0x8F1BBCDC;
            else f = b ^ c ^ dd, k = 0xCA62C1D6;
            uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = dd, dd = c, c = (b << 30) | (b >> 2), b = a, a = tmp;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += dd, h[4] += e;
    }
    free(m);
    lt_bytes *out = lt_bytes_new(20);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out->data[i * 4 + j] = (unsigned char)(h[i] >> (24 - 8 * j));
    out->len = 20;
    return out;
}

// data XOR a repeating 4-byte key (WebSocket masking)
static lt_bytes *lt_ws_mask(lt_bytes *d, lt_bytes *key) {
    lt_bytes *out = lt_bytes_new(d->len);
    for (int64_t i = 0; i < d->len; i++) out->data[i] = d->data[i] ^ (key->len >= 4 ? key->data[i & 3] : 0);
    out->len = d->len;
    return out;
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
    if (lt_text_ascii(t)) return byte < t->len ? byte : t->len;
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
    if (isnan(v) || isinf(v)) return lt_float_to_text(v); // NaN, Infinity: as to_text
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
        lt_buf_free(&b);
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
                    lt_buf_free(&b);
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

// ---------------------------------------------------------------- time zones
// Through the C library (it knows the system's zone database), one
// conversion at a time: TZ is process-wide.

static pthread_mutex_t lt_tz_mu = PTHREAD_MUTEX_INITIALIZER;

static bool lt_tz_known(const char *name) {
    if (strcmp(name, "UTC") == 0) return true;
    if (!*name || name[0] == '/' || strstr(name, "..")) return false;
    char path[512];
    snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", name);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void lt_tz_enter(const char *name, char *saved, size_t cap) {
    pthread_mutex_lock(&lt_tz_mu);
    const char *old = getenv("TZ");
    snprintf(saved, cap, "%s", old ? old : "");
    setenv("TZ", strcmp(name, "UTC") == 0 ? "UTC0" : name, 1);
    tzset();
}

static void lt_tz_leave(const char *saved) {
    if (*saved) setenv("TZ", saved, 1);
    else unsetenv("TZ");
    tzset();
    pthread_mutex_unlock(&lt_tz_mu);
}

static lt_err lt_tz_unknown(lt_text *zone) {
    char buf[300];
    snprintf(buf, sizeof buf, "time: unknown time zone \"%s\" (names are like Europe/Berlin or America/New_York)", zone->data);
    return lt_make_failure(lt_text_cstr(buf));
}

// The wall clock in `zone` at a Unix time: year, month, day, hour, minute,
// second, offset (seconds east of UTC); and the zone's abbreviation.
static lt_err lt_tz_fields(lt_text *zone, int64_t at, lt_texts **abbr_out, int64_t *f) {
    if (!lt_tz_known(zone->data)) return lt_tz_unknown(zone);
    char saved[256];
    lt_tz_enter(zone->data, saved, sizeof saved);
    time_t t = (time_t)at;
    struct tm tm;
    localtime_r(&t, &tm);
    // the abbreviation lives in the C library's zone state: copy it first
    char abbr[16];
    snprintf(abbr, sizeof abbr, "%s", tm.tm_zone ? tm.tm_zone : "UTC");
    lt_tz_leave(saved);
    f[0] = tm.tm_year + 1900;
    f[1] = tm.tm_mon + 1;
    f[2] = tm.tm_mday;
    f[3] = tm.tm_hour;
    f[4] = tm.tm_min;
    f[5] = tm.tm_sec;
    f[6] = tm.tm_gmtoff;
    lt_texts_push(abbr_out, lt_text_cstr(abbr));
    return (lt_err){ 0 };
}

// The Unix time of a wall-clock time in `zone`. A time skipped by a clock
// change (02:30 when clocks jump to 03:00) moves forward; a repeated one
// (autumn) takes the first.
static lt_err lt_tz_unix(lt_text *zone, int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, int64_t s, int64_t *out) {
    if (!lt_tz_known(zone->data)) return lt_tz_unknown(zone);
    char saved[256];
    lt_tz_enter(zone->data, saved, sizeof saved);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (int)(y - 1900);
    tm.tm_mon = (int)(mo - 1);
    tm.tm_mday = (int)d;
    tm.tm_hour = (int)h;
    tm.tm_min = (int)mi;
    tm.tm_sec = (int)s;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    // a time skipped by a clock change: C libraries differ, so decide here:
    // read it with the offset from before the change (02:30 -> 03:30)
    struct tm back;
    localtime_r(&t, &back);
    if (back.tm_hour != (int)h || back.tm_min != (int)mi || back.tm_mday != (int)d) {
        struct tm want;
        memset(&want, 0, sizeof want);
        want.tm_year = (int)(y - 1900);
        want.tm_mon = (int)(mo - 1);
        want.tm_mday = (int)d;
        want.tm_hour = (int)h;
        want.tm_min = (int)mi;
        want.tm_sec = (int)s;
        time_t as_utc = timegm(&want);
        time_t before = as_utc - 6 * 3600;
        struct tm b2;
        localtime_r(&before, &b2);
        t = as_utc - b2.tm_gmtoff;
    }
    lt_tz_leave(saved);
    *out = (int64_t)t;
    return (lt_err){ 0 };
}

// This machine's zone: TZ, else the name /etc/localtime points to, else UTC.
static lt_text *lt_tz_local(void) {
    const char *tz = getenv("TZ");
    if (tz && *tz && lt_tz_known(tz[0] == ':' ? tz + 1 : tz)) return lt_text_cstr(tz[0] == ':' ? tz + 1 : tz);
    char buf[512];
    ssize_t n = readlink("/etc/localtime", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        const char *z = strstr(buf, "zoneinfo/");
        if (z && lt_tz_known(z + 9)) return lt_text_cstr(z + 9);
    }
    return lt_text_cstr("UTC");
}

// ---------------------------------------------------------------- http (used without a server too)

// `plus`: `+` is a space (queries); in paths it's a plus
static lt_text *lt_url_decode(const char *s, int64_t n, bool plus) {
  lt_text *t = lt_text_new(n); int64_t w = 0;
  for (int64_t i = 0; i < n; i++) {
    if (s[i] == '%' && i + 2 < n && lt_hex(s[i + 1]) >= 0 && lt_hex(s[i + 2]) >= 0) { t->data[w++] = (char)(lt_hex(s[i + 1]) * 16 + lt_hex(s[i + 2])); i += 2; }
    else t->data[w++] = (plus && s[i] == '+') ? ' ' : s[i];
  }
  t->len = w; t->data[w] = 0; return t;
}


// the IANA reason phrases
static const char *lt_http_reason(int64_t s) {
    switch (s) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default: return "";
    }
}
static lt_text *lt_http_status_text(int64_t s) { return lt_text_cstr(lt_http_reason(s)); }
