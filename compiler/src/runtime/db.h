// db: SQLite.

#include <sqlite3.h>

typedef struct {
    lt_handle h;
    sqlite3 *db;
    lt_text *path;
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
    if (d->db) sqlite3_close_v2(d->db);
    lt_text_drop(d->path);
    free(d);
}

static lt_err lt_db_error(lt_db *d, const char *what, const char *sql) {
    char buf[1024];
    if (sql) snprintf(buf, sizeof buf, "db: %s: %s (in: %.200s)", what, d && d->db ? sqlite3_errmsg(d->db) : "?", sql);
    else snprintf(buf, sizeof buf, "db: %s: %s", what, d && d->db ? sqlite3_errmsg(d->db) : "?");
    return lt_make_failure(lt_text_cstr(buf));
}

static lt_err lt_db_open(lt_text *path, lt_handle **out) {
    const char *p = path->data;
    if (strncmp(p, "sqlite:", 7) == 0) p += 7; // "sqlite:app.db", "sqlite::memory:"
    lt_db *d = (lt_db *)calloc(1, sizeof(lt_db));
    d->h.rc = 1;
    d->h.free = lt_db_free;
    d->path = path;
    lt_text_dup(path);
    int r = sqlite3_open_v2(p, &d->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (r != SQLITE_OK) {
        char buf[512];
        snprintf(buf, sizeof buf, "db: can't open \"%s\": %s", p, d->db ? sqlite3_errmsg(d->db) : sqlite3_errstr(r));
        lt_handle_drop(&d->h);
        return lt_make_failure(lt_text_cstr(buf));
    }
    sqlite3_busy_timeout(d->db, 5000);
    sqlite3_exec(d->db, "pragma foreign_keys = on", NULL, NULL, NULL);
    *out = &d->h;
    return (lt_err){ 0 };
}

static lt_err lt_db_prepare(lt_db *d, lt_text *sql, const lt_dbval *vals, int64_t n, sqlite3_stmt **st) {
    if (!d->db) return lt_make_failure(lt_text_cstr("db: the connection is closed"));
    if (sqlite3_prepare_v2(d->db, sql->data, (int)sql->len, st, NULL) != SQLITE_OK) return lt_db_error(d, "the SQL is wrong", sql->data);
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

static lt_err lt_db_execute(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, int64_t *changed) {
    lt_db *d = (lt_db *)h;
    sqlite3_stmt *st;
    lt_err e = lt_db_prepare(d, sql, vals, n, &st);
    if (e.obj) return e;
    int r;
    while ((r = sqlite3_step(st)) == SQLITE_ROW) {
    }
    sqlite3_finalize(st);
    if (r != SQLITE_DONE) return lt_db_error(d, "the statement failed", sql->data);
    if (changed) *changed = sqlite3_changes(d->db);
    return (lt_err){ 0 };
}

// db.Connection.insert: runs the statement and reads the new row's id
// under the connection's lock, so another task's insert on the same
// connection can't come in between
static lt_err lt_db_insert(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, int64_t *id) {
    lt_db *d = (lt_db *)h;
    if (!d->db) return lt_make_failure(lt_text_cstr("db: the connection is closed"));
    sqlite3_mutex *mu = sqlite3_db_mutex(d->db);
    sqlite3_mutex_enter(mu);
    sqlite3_stmt *st;
    lt_err e = lt_db_prepare(d, sql, vals, n, &st);
    if (e.obj) {
        sqlite3_mutex_leave(mu);
        return e;
    }
    int r;
    while ((r = sqlite3_step(st)) == SQLITE_ROW) {
    }
    sqlite3_finalize(st);
    if (r != SQLITE_DONE) {
        lt_err f = lt_db_error(d, "the statement failed", sql->data);
        sqlite3_mutex_leave(mu);
        return f;
    }
    *id = sqlite3_last_insert_rowid(d->db);
    sqlite3_mutex_leave(mu);
    return (lt_err){ 0 };
}

// runs a query and calls `row` with each row as an object
static lt_err lt_db_query(lt_handle *h, lt_text *sql, const lt_dbval *vals, int64_t n, lt_err (*row)(const lt_dyn *, void *), void *ctx) {
    lt_db *d = (lt_db *)h;
    sqlite3_stmt *st;
    lt_err e = lt_db_prepare(d, sql, vals, n, &st);
    if (e.obj) return e;
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
    while ((r = sqlite3_step(st)) == SQLITE_ROW) {
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
    if (e.obj) return e;
    if (r != SQLITE_DONE) return lt_db_error(d, "the query failed", sql->data);
    return (lt_err){ 0 };
}

static int64_t lt_db_last_id(lt_handle *h) {
    lt_db *d = (lt_db *)h;
    return d->db ? sqlite3_last_insert_rowid(d->db) : 0;
}

static lt_err lt_db_close(lt_handle *h) {
    lt_db *d = (lt_db *)h;
    if (d->db && sqlite3_close_v2(d->db) != SQLITE_OK) return lt_db_error(d, "can't close", NULL);
    d->db = NULL;
    return (lt_err){ 0 };
}
