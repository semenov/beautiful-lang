// db: SQLite.

#include <sqlite3.h>

// A connection is a pool of SQLite handles, so tasks sharing it run their
// statements side by side (a file is put in WAL mode: readers don't wait
// for the writer). A transaction takes a handle for itself until it ends;
// statements from the same task go to that handle, other tasks' statements
// go to other handles and never join it.
#define LT_DB_PINS 64
typedef struct {
    lt_handle h;
    lt_text *path;
    const char *file;         // path without "sqlite:"
    lt_spin spin;             // protects everything below
    sqlite3 **idle;           // handles nobody uses
    int nidle, open, max;
    bool closed;
#ifdef LT_THREADS
    lt_waitq waiting;         // tasks waiting for a handle
#endif
    struct {
        void *task;           // the task in a transaction
        sqlite3 *db;
        int depth;            // nested transactions are savepoints
    } pins[LT_DB_PINS];
    int npins;
    int64_t last_id;
} lt_db;

typedef struct {
    int kind; // 0 null, 1 int, 2 real, 3 text, 4 blob
    int64_t i;
    double f;
    const char *s;
    int64_t len;
} lt_dbval;

static void lt_db_free(lt_handle *h) {
    lt_db *d = (lt_db *)h;
    for (int i = 0; i < d->nidle; i++) sqlite3_close_v2(d->idle[i]);
    free(d->idle);
    lt_text_drop(d->path);
    free(d);
}

static lt_err lt_db_error(sqlite3 *db, const char *what, const char *sql) {
    char buf[1024];
    if (sql) snprintf(buf, sizeof buf, "db: %s: %s (in: %.200s)", what, db ? sqlite3_errmsg(db) : "?", sql);
    else snprintf(buf, sizeof buf, "db: %s: %s", what, db ? sqlite3_errmsg(db) : "?");
    return lt_make_failure(lt_text_cstr(buf));
}

static void *lt_db_task(void) {
#ifdef LT_THREADS
    return lt_current();
#else
    return NULL;
#endif
}

// a new handle (the caller has counted it in d->open)
static lt_err lt_db_connect(lt_db *d, sqlite3 **out) {
    sqlite3 *db = NULL;
    lt_block_enter();
    int r = sqlite3_open_v2(d->file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    lt_block_exit();
    if (r != SQLITE_OK) {
        char buf[512];
        snprintf(buf, sizeof buf, "db: can't open \"%s\": %s", d->file, db ? sqlite3_errmsg(db) : sqlite3_errstr(r));
        if (db) sqlite3_close_v2(db);
        return lt_make_failure(lt_text_cstr(buf));
    }
    sqlite3_busy_timeout(db, 5000);
    lt_block_enter();
    sqlite3_exec(db, "pragma foreign_keys = on", NULL, NULL, NULL);
    if (d->max > 1) sqlite3_exec(db, "pragma journal_mode = wal", NULL, NULL, NULL);
    lt_block_exit();
    *out = db;
    return (lt_err){ 0 };
}

static int lt_db_pin_of(lt_db *d, void *task) {
    for (int i = 0; i < d->npins; i++)
        if (d->pins[i].task == task) return i;
    return -1;
}

// A handle for one statement: the task's transaction's, an idle one, a new
// one, or (all in use) the next one given back. `*pinned`: it belongs to a
// transaction, so lt_db_put leaves it.
static lt_err lt_db_get(lt_db *d, sqlite3 **out, bool *pinned) {
    void *task = lt_db_task();
    lt_spin_lock(&d->spin);
    for (;;) {
        if (d->closed) {
            lt_spin_unlock(&d->spin);
            return lt_make_failure(lt_text_cstr("db: the connection is closed"));
        }
        int p = lt_db_pin_of(d, task);
        if (p >= 0) {
            *out = d->pins[p].db;
            *pinned = true;
            lt_spin_unlock(&d->spin);
            return (lt_err){ 0 };
        }
        *pinned = false;
        if (d->nidle > 0) {
            *out = d->idle[--d->nidle];
            lt_spin_unlock(&d->spin);
            return (lt_err){ 0 };
        }
        if (d->open < d->max) {
            d->open++;
            lt_spin_unlock(&d->spin);
            lt_err e = lt_db_connect(d, out);
            if (e.obj) {
                lt_spin_lock(&d->spin);
                d->open--;
                lt_spin_unlock(&d->spin);
            }
            return e;
        }
#ifdef LT_THREADS
        lt_park_on(&d->waiting, false); // releases d->spin
        lt_spin_lock(&d->spin);
#else
        lt_spin_unlock(&d->spin);
        return lt_make_failure(lt_text_cstr("db: all connections are in use"));
#endif
    }
}

static void lt_db_put(lt_db *d, sqlite3 *db, bool pinned) {
    if (pinned) return;
    lt_spin_lock(&d->spin);
    if (d->closed) {
        d->open--;
        lt_spin_unlock(&d->spin);
        sqlite3_close_v2(db);
        return;
    }
    d->idle[d->nidle++] = db;
#ifdef LT_THREADS
    lt_task *t = lt_wq_pop(&d->waiting);
#endif
    lt_spin_unlock(&d->spin);
#ifdef LT_THREADS
    if (t) lt_ready(t);
#endif
}

static void lt_db_note_id(lt_db *d, sqlite3 *db, int64_t before) {
    int64_t id = sqlite3_last_insert_rowid(db);
    if (id != before) __atomic_store_n(&d->last_id, id, __ATOMIC_RELAXED);
}

static lt_err lt_db_open(lt_text *path, lt_handle **out) {
    const char *p = path->data;
    if (strncmp(p, "sqlite:", 7) == 0) p += 7; // "sqlite:app.db", "sqlite::memory:"
    lt_db *d = (lt_db *)calloc(1, sizeof(lt_db));
    d->h.rc = 1;
    d->h.free = lt_db_free;
    d->path = path;
    lt_text_dup(path);
    d->file = p;
    // each handle to ":memory:" would be a database of its own
    bool memory = strcmp(p, ":memory:") == 0 || strstr(p, "mode=memory") != NULL;
    int n = lt_ncpu();
    d->max = memory ? 1 : (n < 4 ? 4 : n > 32 ? 32 : n);
    d->idle = (sqlite3 **)calloc((size_t)d->max, sizeof(sqlite3 *));
#ifdef LT_THREADS
    d->waiting.lock = &d->spin;
#endif
    // the first handle now, so a bad path fails here
    sqlite3 *db;
    d->open = 1;
    lt_err e = lt_db_connect(d, &db);
    if (e.obj) {
        d->open = 0;
        lt_handle_drop(&d->h);
        return e;
    }
    d->idle[d->nidle++] = db;
    *out = &d->h;
    return (lt_err){ 0 };
}

static lt_err lt_db_prepare(sqlite3 *db, lt_text *sql, const lt_dbval *vals, int64_t n, sqlite3_stmt **st) {
    lt_block_enter();
    int pr = sqlite3_prepare_v2(db, sql->data, (int)sql->len, st, NULL);
    lt_block_exit();
    if (pr != SQLITE_OK) return lt_db_error(db, "the SQL is wrong", sql->data);
    int want = sqlite3_bind_parameter_count(*st);
    if (want != n) {
        char buf[512];
        snprintf(buf, sizeof buf, "db: the SQL has %d `?` but %lld values were given (in: %.200s)", want, (long long)n, sql->data);
        sqlite3_finalize(*st);
        return lt_make_failure(lt_text_cstr(buf));
    }
    for (int64_t i = 0; i < n; i++) {
        int k = (int)i + 1;
        switch (vals[i].kind) {
        case 0: sqlite3_bind_null(*st, k); break;
        case 1: sqlite3_bind_int64(*st, k, vals[i].i); break;
        case 2: sqlite3_bind_double(*st, k, vals[i].f); break;
        case 3: sqlite3_bind_text(*st, k, vals[i].s, (int)vals[i].len, SQLITE_TRANSIENT); break;
        default: sqlite3_bind_blob(*st, k, vals[i].s, (int)vals[i].len, SQLITE_TRANSIENT); break;
        }
    }
    return (lt_err){ 0 };
}

// a step may wait for the disk or another writer's lock
static int lt_db_step(sqlite3_stmt *st) {
    lt_block_enter();
    int r = sqlite3_step(st);
    lt_block_exit();
    return r;
}

// runs a statement to the end on a handle of its own
static lt_err lt_db_run(lt_db *d, lt_text *sql, const lt_dbval *vals, int64_t n, int64_t *changed, int64_t *id) {
    sqlite3 *db;
    bool pinned;
    lt_err e = lt_db_get(d, &db, &pinned);
    if (e.obj) return e;
    int64_t before = sqlite3_last_insert_rowid(db);
    sqlite3_stmt *st;
    e = lt_db_prepare(db, sql, vals, n, &st);
    if (!e.obj) {
        int r;
        while ((r = lt_db_step(st)) == SQLITE_ROW) {
        }
        sqlite3_finalize(st);
        if (r != SQLITE_DONE) e = lt_db_error(db, "the statement failed", sql->data);
    }
    if (!e.obj) {
        lt_db_note_id(d, db, before);
        if (changed) *changed = sqlite3_changes(db);
        if (id) *id = sqlite3_last_insert_rowid(db);
    }
    lt_db_put(d, db, pinned);
    return e;
}

static lt_err lt_db_execute(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, int64_t *changed) {
    return lt_db_run((lt_db *)h, sql, vals, n, changed, NULL);
}

// db.Connection.insert: the id is read on the handle that ran the insert,
// which no other task uses meanwhile
static lt_err lt_db_insert(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, int64_t *id) {
    return lt_db_run((lt_db *)h, sql, vals, n, NULL, id);
}

// runs a query and calls `row` with each row as an object
static lt_err lt_db_query(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, lt_err (*row)(const lt_dyn *, void *), void *ctx) {
    lt_db *d = (lt_db *)h;
    sqlite3 *db;
    bool pinned;
    lt_err e = lt_db_get(d, &db, &pinned);
    if (e.obj) return e;
    sqlite3_stmt *st;
    e = lt_db_prepare(db, sql, vals, n, &st);
    if (e.obj) {
        lt_db_put(d, db, pinned);
        return e;
    }
    int cols = sqlite3_column_count(st);
    lt_arena ar = { 0 };
    lt_dyn o;
    memset(&o, 0, sizeof o);
    o.kind = LT_D_OBJ;
    o.n = cols;
    o.items = (lt_dyn *)lt_arena_alloc(&ar, sizeof(lt_dyn) * (size_t)(cols ? cols : 1));
    o.keys = (const char **)lt_arena_alloc(&ar, sizeof(char *) * (size_t)(cols ? cols : 1));
    o.klens = (int64_t *)lt_arena_alloc(&ar, sizeof(int64_t) * (size_t)(cols ? cols : 1));
    // copied: SQLite may prepare the statement again on the first step (a
    // bound `limit ?` can make it), which frees the names it gave out
    for (int c = 0; c < cols; c++) {
        const char *name = sqlite3_column_name(st, c);
        size_t len = strlen(name);
        char *copy = (char *)lt_arena_alloc(&ar, len + 1);
        memcpy(copy, name, len + 1);
        o.keys[c] = copy;
        o.klens[c] = (int64_t)len;
    }
    int r;
    while ((r = lt_db_step(st)) == SQLITE_ROW) {
        for (int c = 0; c < cols; c++) {
            lt_dyn *v = &o.items[c];
            memset(v, 0, sizeof *v);
            switch (sqlite3_column_type(st, c)) {
            case SQLITE_INTEGER:
                v->kind = LT_D_NUM;
                v->is_int = true;
                v->i = sqlite3_column_int64(st, c);
                v->num = (double)v->i;
                break;
            case SQLITE_FLOAT:
                v->kind = LT_D_NUM;
                v->num = sqlite3_column_double(st, c);
                break;
            case SQLITE_NULL:
                v->kind = LT_D_NULL;
                break;
            case SQLITE_BLOB:
                v->kind = LT_D_STR;
                v->raw = true;
                v->s = (const char *)sqlite3_column_blob(st, c);
                v->slen = sqlite3_column_bytes(st, c);
                break;
            default:
                v->kind = LT_D_STR;
                v->s = (const char *)sqlite3_column_text(st, c);
                v->slen = sqlite3_column_bytes(st, c);
            }
        }
        e = row(&o, ctx);
        if (e.obj) break;
    }
    sqlite3_finalize(st);
    lt_arena_free(&ar);
    if (!e.obj && r != SQLITE_DONE) e = lt_db_error(db, "the query failed", sql->data);
    lt_db_put(d, db, pinned);
    return e;
}

static int64_t lt_db_last_id(lt_handle *h) {
    return __atomic_load_n(&((lt_db *)h)->last_id, __ATOMIC_RELAXED);
}

// Closes the idle handles now and the others as they're given back.
static lt_err lt_db_close(lt_handle *h) {
    lt_db *d = (lt_db *)h;
    lt_spin_lock(&d->spin);
    d->closed = true;
    int n = d->nidle;
    d->nidle = 0;
    d->open -= n;
#ifdef LT_THREADS
    lt_wake_all(&d->waiting);
#endif
    lt_spin_unlock(&d->spin);
    lt_err e = { 0 };
    for (int i = 0; i < n; i++)
        if (sqlite3_close_v2(d->idle[i]) != SQLITE_OK && !e.obj) e = lt_db_error(d->idle[i], "can't close", NULL);
    return e;
}

#ifdef LT_THREADS
static void lt_db_forget_tx(lt_db *d) {
    lt_task *t = lt_current();
    if (!t) return;
    for (int i = 0; i < t->ntxs; i++)
        if (t->txs[i] == d) {
            t->txs[i] = t->txs[--t->ntxs];
            lt_handle_drop(&d->h);
            return;
        }
}

// after a panic: roll back the task's transactions, give the handles back
static void lt_db_abort(lt_task *t) {
    while (t->ntxs > 0) {
        lt_db *d = (lt_db *)t->txs[--t->ntxs];
        lt_spin_lock(&d->spin);
        int p = lt_db_pin_of(d, t);
        sqlite3 *db = NULL;
        if (p >= 0) {
            db = d->pins[p].db;
            d->pins[p] = d->pins[--d->npins];
        }
        lt_spin_unlock(&d->spin);
        if (db) {
            sqlite3_exec(db, "rollback", NULL, NULL, NULL);
            lt_db_put(d, db, false);
        }
        lt_handle_drop(&d->h);
    }
}
#else
static void lt_db_forget_tx(lt_db *d) { (void)d; }
#endif

// db.Connection.transaction: the task keeps one handle until lt_db_end.
// Inside a transaction, another one is a savepoint.
static lt_err lt_db_begin(lt_handle *h) {
    lt_db *d = (lt_db *)h;
    sqlite3 *db;
    bool pinned;
    lt_err e = lt_db_get(d, &db, &pinned);
    if (e.obj) return e;
    void *task = lt_db_task();
    int depth = 0;
    if (pinned) {
        lt_spin_lock(&d->spin);
        depth = d->pins[lt_db_pin_of(d, task)].depth;
        lt_spin_unlock(&d->spin);
    } else {
        lt_spin_lock(&d->spin);
        bool full = d->npins == LT_DB_PINS;
        lt_spin_unlock(&d->spin);
        if (full) {
            lt_db_put(d, db, false);
            return lt_make_failure(lt_text_cstr("db: too many transactions at once"));
        }
    }
    char sql[64];
    // immediate: take the write lock now; a transaction that read first
    // couldn't wait for it later
    if (depth == 0) snprintf(sql, sizeof sql, "begin immediate");
    else snprintf(sql, sizeof sql, "savepoint lt_%d", depth);
    lt_block_enter();
    int r = sqlite3_exec(db, sql, NULL, NULL, NULL);
    lt_block_exit();
    if (r != SQLITE_OK) {
        e = lt_db_error(db, "can't start a transaction", NULL);
        lt_db_put(d, db, pinned);
        return e;
    }
    lt_spin_lock(&d->spin);
    if (pinned) {
        d->pins[lt_db_pin_of(d, task)].depth++;
    } else {
        int i = d->npins++;
        d->pins[i].task = task;
        d->pins[i].db = db;
        d->pins[i].depth = 1;
    }
    lt_spin_unlock(&d->spin);
#ifdef LT_THREADS
    // a panic in an HTTP handler ends the request, not the task: the
    // transaction is rolled back then (lt_db_abort)
    lt_task *t = lt_current();
    if (!pinned && t && t->ntxs < 4) {
        lt_tx_abort_hook = lt_db_abort;
        lt_handle_dup(&d->h);
        t->txs[t->ntxs++] = d;
    }
#endif
    return (lt_err){ 0 };
}

static lt_err lt_db_end(lt_handle *h, bool commit) {
    lt_db *d = (lt_db *)h;
    void *task = lt_db_task();
    lt_spin_lock(&d->spin);
    int p = lt_db_pin_of(d, task);
    if (p < 0) {
        lt_spin_unlock(&d->spin);
        return lt_make_failure(lt_text_cstr("db: no transaction to end"));
    }
    sqlite3 *db = d->pins[p].db;
    int depth = --d->pins[p].depth;
    if (depth == 0) d->pins[p] = d->pins[--d->npins];
    lt_spin_unlock(&d->spin);
    char sql[80];
    if (depth == 0) snprintf(sql, sizeof sql, commit ? "commit" : "rollback");
    else if (commit) snprintf(sql, sizeof sql, "release lt_%d", depth);
    else snprintf(sql, sizeof sql, "rollback to lt_%d; release lt_%d", depth, depth);
    lt_block_enter();
    int r = sqlite3_exec(db, sql, NULL, NULL, NULL);
    lt_block_exit();
    lt_err e = { 0 };
    if (r != SQLITE_OK) {
        e = lt_db_error(db, commit ? "can't commit" : "can't roll back", NULL);
        // a failed commit leaves the transaction open: end it so the
        // handle can go back to the pool
        if (depth == 0 && !sqlite3_get_autocommit(db)) sqlite3_exec(db, "rollback", NULL, NULL, NULL);
    }
    if (depth == 0) {
        lt_db_put(d, db, false);
        lt_db_forget_tx(d);
    }
    return e;
}
