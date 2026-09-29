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
    size_t *sizes;
    int nblocks, cap;
    char *cur, *end;
} lt_arena;

// Blocks double from 16 KB, so a small parse stays small; blocks of 1 MB
// and more come straight from the system and go back to it when freed
// (the system allocator would keep them, and a program that parses big
// texts again and again would grow).
static void *lt_arena_alloc(lt_arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->cur || (size_t)(a->end - a->cur) < n) {
        size_t sz = (size_t)16384 << (a->nblocks < 10 ? a->nblocks : 10);
        if (sz < n) sz = n;
        char *b = sz >= LT_BIG ? (char *)lt_big_alloc(sz) : (char *)malloc(sz);
        if (!b) lt_oom();
        if (a->nblocks == a->cap) {
            a->cap = a->cap ? a->cap * 2 : 8;
            a->blocks = (char **)realloc(a->blocks, sizeof(char *) * (size_t)a->cap);
            a->sizes = (size_t *)realloc(a->sizes, sizeof(size_t) * (size_t)a->cap);
        }
        a->blocks[a->nblocks] = b;
        a->sizes[a->nblocks++] = sz;
        a->cur = b;
        a->end = b + sz;
    }
    void *p = a->cur;
    a->cur += n;
    return p;
}
static void lt_arena_free(lt_arena *a) {
    for (int i = 0; i < a->nblocks; i++) {
        if (a->sizes[i] >= LT_BIG) lt_big_free(a->blocks[i], a->sizes[i]);
        else free(a->blocks[i]);
    }
    free(a->blocks);
    free(a->sizes);
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

// Where a decoding error is, in the words of its source:
//   json:  "json: at $.items[2].price"
//   cli:   "the option --count", "argument 2"
//   env:   "the environment variable PORT"
//   csv:   "csv: line 4, column quantity"
//   sql/db: "db: row 2, column age"
static void lt_dec_where(const char *src, const lt_path *p, char *buf, size_t cap, size_t *len) {
    const lt_path *nodes[64];
    int n = 0;
    for (const lt_path *q = p; q && n < 64; q = q->parent) nodes[n++] = q;
    // nodes[n - 1] is the outermost
    const lt_path *first = n > 0 ? nodes[n - 1] : NULL;
    const lt_path *second = n > 1 ? nodes[n - 2] : NULL;
    if ((strcmp(src, "cli") == 0 || strcmp(src, "env") == 0) && first && first->key) {
        if (strcmp(src, "env") == 0) {
            *len += (size_t)snprintf(buf + *len, cap - *len, "the environment variable ");
            for (int64_t i = 0; i < first->klen && *len + 1 < cap; i++) buf[(*len)++] = (char)toupper((unsigned char)first->key[i]);
            buf[*len < cap ? *len : cap - 1] = 0;
        } else if (first->klen == 4 && memcmp(first->key, "args", 4) == 0 && second && !second->key) {
            *len += (size_t)snprintf(buf + *len, cap - *len, "argument %lld", (long long)second->index + 1);
            return;
        } else {
            *len += (size_t)snprintf(buf + *len, cap - *len, "the option --");
            for (int64_t i = 0; i < first->klen && *len + 1 < cap; i++) buf[(*len)++] = first->key[i] == '_' ? '-' : first->key[i];
            buf[*len < cap ? *len : cap - 1] = 0;
        }
        // a repeated option: which of its values
        if (second && !second->key && *len < cap) *len += (size_t)snprintf(buf + *len, cap - *len, " (value %lld)", (long long)second->index + 1);
        return;
    }
    bool rows = strcmp(src, "csv") == 0 || strcmp(src, "sql") == 0 || strcmp(src, "db") == 0;
    if (rows && first && !first->key) {
        *len += (size_t)snprintf(buf + *len, cap - *len, "%s: %s %lld", src, strcmp(src, "csv") == 0 ? "line" : "row", (long long)first->index);
        if (second && second->key && *len < cap) *len += (size_t)snprintf(buf + *len, cap - *len, ", column %.*s", (int)second->klen, second->key);
        return;
    }
    *len += (size_t)snprintf(buf + *len, cap - *len, "%s: at ", src);
    lt_path_write(p, buf, cap, len);
}

static lt_err lt_dec_error(const char *source, const lt_path *p, const char *what, const lt_dyn *found) {
    char buf[1024];
    size_t len = 0;
    lt_dec_where(source, p, buf, sizeof buf, &len);
    if (len < sizeof buf) {
        if (!found) snprintf(buf + len, sizeof buf - len, ": %s", what);
        else if (found->kind == LT_D_STR) snprintf(buf + len, sizeof buf - len, ": expected %s, found \"%.*s\"", what, (int)(found->slen > 60 ? 60 : found->slen), found->s);
        else if (found->kind == LT_D_NUM && found->s) snprintf(buf + len, sizeof buf - len, ": expected %s, found %.*s", what, (int)(found->slen > 40 ? 40 : found->slen), found->s);
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
    } else if (strcmp(src, "csv") == 0 && p && !p->parent && !p->key) {
        snprintf(buf, sizeof buf, "csv: line %lld: the column `%s` is empty or missing", (long long)p->index, field);
    } else {
        size_t len = (size_t)snprintf(buf, sizeof buf, "%s: at ", src);
        lt_path_write(p, buf, sizeof buf, &len);
        if (len < sizeof buf) snprintf(buf + len, sizeof buf - len, ": the field `%s` is missing", field);
    }
    return lt_make_failure(lt_text_cstr(buf));
}

// same name, ignoring case and `_` / `-`: created_at, createdAt, CreatedAt, created-at
static bool lt_key_loose_eq(const char *a, int64_t al, const char *b, int64_t bl) {
    int64_t i = 0, j = 0;
    for (;;) {
        while (i < al && (a[i] == '_' || a[i] == '-')) i++;
        while (j < bl && (b[j] == '_' || b[j] == '-')) j++;
        if (i == al || j == bl) return i == al && j == bl;
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[j])) return false;
        i++, j++;
    }
}

static const lt_dyn *lt_dyn_get(const lt_dyn *o, const char *key, int64_t klen) {
    for (int64_t i = 0; i < o->n; i++)
        if (o->klens[i] == klen && memcmp(o->keys[i], key, (size_t)klen) == 0) return &o->items[i];
    // APIs often write keys in camelCase: `createdAt` fills `created_at`
    for (int64_t i = 0; i < o->n; i++)
        if (lt_key_loose_eq(o->keys[i], o->klens[i], key, klen)) return &o->items[i];
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
    if (d->kind == LT_D_NUM && !isinf(d->num)) {
        *out = d->is_int ? (double)d->i : d->num;
        return (lt_err){ 0 };
    }
    if (d->kind == LT_D_NUM) return lt_dec_error(src, p, "a number that fits a Float", d);
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
    // the children of the containers being read, until each one closes and
    // gets an array of its exact size in the arena
    lt_dyn *stk;
    const char **sk;
    int64_t *sl;
    int64_t top, cap;
} lt_jp;

static void lt_jp_push(lt_jp *p, const lt_dyn *d, const char *k, int64_t kl) {
    if (p->top == p->cap) {
        // the runtime's allocator: big ones go back to the system when freed
        int64_t nc = p->cap ? p->cap * 2 : 64;
        if (p->cap) {
            p->stk = (lt_dyn *)lt_realloc(p->stk, sizeof(lt_dyn) * (size_t)p->cap, sizeof(lt_dyn) * (size_t)nc);
            p->sk = (const char **)lt_realloc(p->sk, sizeof(char *) * (size_t)p->cap, sizeof(char *) * (size_t)nc);
            p->sl = (int64_t *)lt_realloc(p->sl, sizeof(int64_t) * (size_t)p->cap, sizeof(int64_t) * (size_t)nc);
        } else {
            p->stk = (lt_dyn *)lt_alloc(sizeof(lt_dyn) * (size_t)nc);
            p->sk = (const char **)lt_alloc(sizeof(char *) * (size_t)nc);
            p->sl = (int64_t *)lt_alloc(sizeof(int64_t) * (size_t)nc);
        }
        p->cap = nc;
    }
    p->stk[p->top] = *d;
    p->sk[p->top] = k;
    p->sl[p->top] = kl;
    p->top++;
}
// moves the children above `base` into the arena
static void lt_jp_close(lt_jp *p, lt_dyn *d, int64_t base, bool keys) {
    int64_t n = p->top - base;
    d->n = n;
    if (n > 0) {
        d->items = (lt_dyn *)lt_arena_alloc(p->arena, sizeof(lt_dyn) * (size_t)n);
        memcpy(d->items, p->stk + base, sizeof(lt_dyn) * (size_t)n);
        if (keys) {
            d->keys = (const char **)lt_arena_alloc(p->arena, sizeof(char *) * (size_t)n);
            memcpy(d->keys, p->sk + base, sizeof(char *) * (size_t)n);
            d->klens = (int64_t *)lt_arena_alloc(p->arena, sizeof(int64_t) * (size_t)n);
            memcpy(d->klens, p->sl + base, sizeof(int64_t) * (size_t)n);
        }
    }
    p->top = base;
}

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
        int64_t base = p->top;
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
                lt_dyn v;
                if (!lt_jp_value(p, &v)) { ok = false; break; }
                lt_jp_push(p, &v, k, kl);
                lt_jp_ws(p);
                if (p->s < p->end && *p->s == ',') { p->s++; continue; }
                if (p->s < p->end && *p->s == '}') { p->s++; break; }
                ok = lt_jp_fail(p, "expected `,` or `}`");
                break;
            }
        }
        lt_jp_close(p, d, base, true);
    } else if (c == '[') {
        p->s++;
        d->kind = LT_D_ARR;
        int64_t base = p->top;
        lt_jp_ws(p);
        if (p->s < p->end && *p->s == ']') {
            p->s++;
        } else {
            for (;;) {
                lt_dyn v;
                if (!lt_jp_value(p, &v)) { ok = false; break; }
                lt_jp_push(p, &v, NULL, 0);
                lt_jp_ws(p);
                if (p->s < p->end && *p->s == ',') { p->s++; continue; }
                if (p->s < p->end && *p->s == ']') { p->s++; break; }
                ok = lt_jp_fail(p, "expected `,` or `]`");
                break;
            }
        }
        lt_jp_close(p, d, base, false);
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
        // the digits as written (1.50 stays 1.50 for Decimal and templates)
        d->s = st;
        d->slen = n;
        d->num = strtod(tmp, &e);
        if (*e) {
            p->s = st;
            return lt_jp_fail(p, "a bad number");
        }
        if (!frac) {
            errno = 0;
            long long v = strtoll(tmp, &e, 10);
            // (-0 stays a Float: an Int has no negative zero)
            if (!*e && errno == 0 && !(v == 0 && tmp[0] == '-')) {
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
    lt_jp p;
    memset(&p, 0, sizeof p);
    p.s = p.start = text->data;
    p.end = text->data + text->len;
    p.arena = arena;
    bool ok = lt_jp_value(&p, d);
    if (p.cap) {
        lt_free(p.stk, sizeof(lt_dyn) * (size_t)p.cap);
        lt_free(p.sk, sizeof(char *) * (size_t)p.cap);
        lt_free(p.sl, sizeof(int64_t) * (size_t)p.cap);
    }
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
    int keys; // record field names: 0 as written, 1 camelCase (createdAt), 2 kebab-case (created-at)
    int omit_none; // leave out record fields that are none
} lt_buf;

static void lt_buf_grow(lt_buf *b, int64_t n) {
    if (b->len + n <= b->cap) return;
    int64_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n) nc *= 2;
    // through the runtime's allocator: a big buffer is mapped from the
    // system and given back when freed (malloc keeps freed big blocks)
    b->d = b->d ? (char *)lt_realloc(b->d, (size_t)b->cap, (size_t)nc) : (char *)lt_alloc((size_t)nc);
    b->cap = nc;
}
static void lt_buf_free(lt_buf *b) {
    if (b->d) lt_free(b->d, (size_t)b->cap);
    b->d = NULL;
    b->len = b->cap = 0;
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
// json.Number: the text of a number as written (or made from its value)
static lt_text *lt_json_number_text(const lt_dyn *d) {
    if (d->s) return lt_text_from(d->s, d->slen);
    if (d->is_int) return lt_int_to_text(d->i);
    char t[48];
    int n = lt_float_js(d->num, t);
    return lt_text_from(t, n);
}
// is it a number in JSON's grammar? -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
static bool lt_json_number_ok(const char *s, int64_t n) {
    int64_t i = 0;
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '0') i++;
    else if (i < n && s[i] >= '1' && s[i] <= '9')
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    else return false;
    if (i < n && s[i] == '.') {
        i++;
        if (!(i < n && s[i] >= '0' && s[i] <= '9')) return false;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (!(i < n && s[i] >= '0' && s[i] <= '9')) return false;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    }
    return i == n;
}
static void lt_json_number_put(lt_buf *b, lt_text *t) {
    if (!lt_json_number_ok(t->data, t->len)) {
        char m[160];
        snprintf(m, sizeof m, "json.Number(text: \"%.*s\") is not a JSON number", (int)(t->len > 60 ? 60 : t->len), t->data);
        lt_panic_at(m, 0);
    }
    lt_buf_put(b, t->data, t->len);
}
static void lt_json_key(lt_buf *b, const char *k, bool first) {
    if (!first) lt_buf_c(b, ',');
    lt_buf_newline(b);
    if (b->keys == 2 && strchr(k, '_')) {
        char c[256];
        size_t w = 0;
        for (const char *p = k; *p && w + 1 < sizeof c; p++) c[w++] = *p == '_' ? '-' : *p;
        lt_json_str(b, c, (int64_t)w);
    } else if (b->keys == 1 && strchr(k, '_')) {
        char c[256];
        size_t w = 0;
        bool up = false;
        for (const char *p = k; *p && w + 1 < sizeof c; p++) {
            if (*p == '_') {
                up = w > 0;
                continue;
            }
            c[w++] = up ? (char)toupper((unsigned char)*p) : *p;
            up = false;
        }
        lt_json_str(b, c, (int64_t)w);
    } else {
        lt_json_str(b, k, (int64_t)strlen(k));
    }
    lt_buf_c(b, ':');
    if (b->indent >= 0) lt_buf_c(b, ' ');
}
static lt_text *lt_buf_text(lt_buf *b) {
    lt_text *t = lt_text_from(b->d ? b->d : "", b->len);
    lt_buf_free(b);
    return t;
}
