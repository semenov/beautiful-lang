// template: Handlebars/Mustache-style templates, filled from the JSON form
// of a value. {{x}} escapes HTML (in HTML templates), {{{x}}} doesn't;
// {{#if}} {{#unless}} {{#each}} {{#with}} {{else}}; {{> partial}};
// {{! comment}}; {{~ and ~}} trim whitespace; a name missing from the
// data is an error (typos don't render as nothing).

enum { LT_T_TEXT, LT_T_VAR, LT_T_RAW, LT_T_SECTION, LT_T_PARTIAL };
enum { LT_S_IF = 1, LT_S_UNLESS, LT_S_EACH, LT_S_WITH };

typedef struct lt_tnode {
    int kind, sec, line;
    const char *s; // text, a path or a partial's name
    int64_t len;
    struct lt_tnode *kids, *els;
    int nkids, nels, capk, cape;
} lt_tnode;

typedef struct {
    char *name;
    char *src;
    bool escape;
    lt_tnode root;
} lt_tpl;

typedef struct lt_tlib {
    lt_handle h;
    lt_tpl **t;
    int n;
} lt_tlib;

static void lt_tnode_free(lt_tnode *n) {
    for (int i = 0; i < n->nkids; i++) lt_tnode_free(&n->kids[i]);
    for (int i = 0; i < n->nels; i++) lt_tnode_free(&n->els[i]);
    free(n->kids);
    free(n->els);
}

static void lt_tlib_free(lt_handle *h) {
    lt_tlib *l = (lt_tlib *)h;
    for (int i = 0; i < l->n; i++) {
        lt_tnode_free(&l->t[i]->root);
        free(l->t[i]->name);
        free(l->t[i]->src);
        free(l->t[i]);
    }
    free(l->t);
    free(l);
}

static lt_tnode *lt_tnode_add(lt_tnode *parent, bool in_else) {
    lt_tnode **arr = in_else ? &parent->els : &parent->kids;
    int *n = in_else ? &parent->nels : &parent->nkids, *cap = in_else ? &parent->cape : &parent->capk;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = (lt_tnode *)realloc(*arr, sizeof(lt_tnode) * (size_t)*cap);
    }
    lt_tnode *x = &(*arr)[(*n)++];
    memset(x, 0, sizeof *x);
    return x;
}

static lt_err lt_tpl_error(const char *name, int line, const char *what, const char *detail, int64_t dlen) {
    char buf[512];
    snprintf(buf, sizeof buf, "template%s%s: line %d: %s%.*s", name && *name ? " " : "", name ? name : "", line, what, (int)dlen, detail ? detail : "");
    return lt_make_failure(lt_text_cstr(buf));
}

static int lt_line_at(const char *src, const char *p) {
    int line = 1;
    for (const char *c = src; c < p; c++) line += *c == '\n';
    return line;
}

static const char *lt_trim_s(const char **s, int64_t *len) {
    while (*len > 0 && isspace((unsigned char)**s)) (*s)++, (*len)--;
    while (*len > 0 && isspace((unsigned char)(*s)[*len - 1])) (*len)--;
    return *s;
}

// Parses `t->src` into its tree.
static lt_err lt_tpl_parse(lt_tpl *t) {
    const char *src = t->src, *p = src, *end = src + strlen(src);
    lt_tnode *stack[64];
    bool in_else[64];
    int depth = 0;
    stack[0] = &t->root;
    in_else[0] = false;
    bool trim_next = false;
    while (p < end) {
        const char *open = strstr(p, "{{");
        const char *text_end = open ? open : end;
        // the text before the tag
        const char *ts = p;
        int64_t tl = text_end - p;
        if (trim_next) {
            while (tl > 0 && isspace((unsigned char)*ts)) ts++, tl--;
            trim_next = false;
        }
        lt_tnode *textn = NULL;
        if (tl > 0) {
            textn = lt_tnode_add(stack[depth], in_else[depth]);
            textn->kind = LT_T_TEXT;
            textn->s = ts;
            textn->len = tl;
        }
        if (!open) break;
        int line = lt_line_at(src, open);
        bool raw = open[2] == '{';
        const char *body = open + (raw ? 3 : 2);
        const char *close = NULL;
        if (!raw && body[0] == '!' && body[1] == '-' && body[2] == '-') {
            close = strstr(body, "--}}");
            if (close) close += 2;
        } else {
            close = strstr(body, raw ? "}}}" : "}}");
        }
        if (!close) return lt_tpl_error(t->name, line, "a tag without its closing }}", NULL, 0);
        const char *after = close + (raw ? 3 : 2);
        const char *c = body;
        int64_t clen = close - body;
        if (clen > 0 && c[0] == '~') {
            c++, clen--;
            if (textn) {
                while (textn->len > 0 && isspace((unsigned char)textn->s[textn->len - 1])) textn->len--;
            }
        }
        if (clen > 0 && c[clen - 1] == '~') {
            clen--;
            trim_next = true;
        }
        lt_trim_s(&c, &clen);
        char kind = clen > 0 ? c[0] : 0;
        bool block_tag = !raw && (kind == '#' || kind == '/' || kind == '!' || kind == '>' || (clen == 4 && memcmp(c, "else", 4) == 0));
        // a block tag alone on its line takes the line with it
        if (block_tag) {
            const char *ls = textn ? textn->s + textn->len : open;
            const char *q = ls;
            while (q > src && (q[-1] == ' ' || q[-1] == '\t')) q--;
            bool starts_line = q == src || q[-1] == '\n';
            const char *r = after;
            while (r < end && (*r == ' ' || *r == '\t')) r++;
            bool ends_line = r == end || *r == '\n' || (*r == '\r' && r + 1 < end && r[1] == '\n');
            if (starts_line && ends_line) {
                if (textn) textn->len -= (ls - q) < textn->len ? (ls - q) : textn->len;
                if (r < end) r += *r == '\r' ? 2 : 1;
                after = r;
            }
        }
        p = after;
        if (raw) {
            lt_tnode *n = lt_tnode_add(stack[depth], in_else[depth]);
            n->kind = LT_T_RAW;
            n->s = c;
            n->len = clen;
            n->line = line;
            continue;
        }
        if (kind == '!') continue;
        if (kind == '#') {
            c++, clen--;
            lt_trim_s(&c, &clen);
            const char *sp = memchr(c, ' ', (size_t)clen);
            int64_t wl = sp ? sp - c : clen;
            int sec = 0;
            if (wl == 2 && memcmp(c, "if", 2) == 0) sec = LT_S_IF;
            else if (wl == 6 && memcmp(c, "unless", 6) == 0) sec = LT_S_UNLESS;
            else if (wl == 4 && memcmp(c, "each", 4) == 0) sec = LT_S_EACH;
            else if (wl == 4 && memcmp(c, "with", 4) == 0) sec = LT_S_WITH;
            else return lt_tpl_error(t->name, line, "unknown block #", c, wl);
            if (!sp) return lt_tpl_error(t->name, line, "a block needs a name after #", c, wl);
            if (depth == 63) return lt_tpl_error(t->name, line, "blocks nested too deep", NULL, 0);
            lt_tnode *n = lt_tnode_add(stack[depth], in_else[depth]);
            n->kind = LT_T_SECTION;
            n->sec = sec;
            n->line = line;
            n->s = sp + 1;
            n->len = clen - wl - 1;
            lt_trim_s(&n->s, &n->len);
            stack[++depth] = n;
            in_else[depth] = false;
            continue;
        }
        if (kind == '/') {
            if (depth == 0) return lt_tpl_error(t->name, line, "a closing tag without its block: ", c, clen);
            depth--;
            continue;
        }
        if (clen == 4 && memcmp(c, "else", 4) == 0) {
            if (depth == 0) return lt_tpl_error(t->name, line, "{{else}} outside a block", NULL, 0);
            in_else[depth] = true;
            continue;
        }
        lt_tnode *n = lt_tnode_add(stack[depth], in_else[depth]);
        n->line = line;
        if (kind == '>') {
            c++, clen--;
            lt_trim_s(&c, &clen);
            n->kind = LT_T_PARTIAL;
        } else {
            n->kind = t->escape ? LT_T_VAR : LT_T_RAW;
        }
        n->s = c;
        n->len = clen;
    }
    if (depth > 0) {
        lt_tnode *open_block = stack[depth];
        static const char *names[] = { "", "if", "unless", "each", "with" };
        char what[64];
        snprintf(what, sizeof what, "{{#%s}} isn't closed with {{/%s}}", names[open_block->sec], names[open_block->sec]);
        return lt_tpl_error(t->name, open_block->line, what, NULL, 0);
    }
    return (lt_err){ 0 };
}

// ---- rendering

typedef struct {
    const lt_dyn *v;
    int64_t index, count; // in #each: the position; count 0 otherwise
    const char *key;
    int64_t klen;
} lt_tframe;

typedef struct {
    lt_tlib *lib;
    lt_tpl *tpl;
    lt_tframe frames[128];
    int nf;
    lt_buf out;
    int partials;
} lt_trender;

static bool lt_seg_eq(const char *a, int64_t al, const char *b) { return al == (int64_t)strlen(b) && memcmp(a, b, (size_t)al) == 0; }

// Finds `path` (a.b.c, this, ../x, @index...) from the top frame down.
// Returns NULL and sets *err for a name that isn't there.
static const lt_dyn *lt_tlookup(lt_trender *r, const char *path, int64_t len, int line, lt_err *err, lt_dyn *tmp) {
    int f = r->nf - 1;
    while (len >= 3 && memcmp(path, "../", 3) == 0) {
        path += 3, len -= 3;
        if (f > 0) f--;
    }
    if (len == 0 || lt_seg_eq(path, len, "this") || lt_seg_eq(path, len, ".")) return r->frames[f].v;
    if (path[0] == '@') {
        for (int i = f; i >= 0; i--) {
            lt_tframe *fr = &r->frames[i];
            if (fr->count == 0) continue;
            memset(tmp, 0, sizeof *tmp);
            if (lt_seg_eq(path, len, "@index")) {
                tmp->kind = LT_D_NUM, tmp->is_int = true, tmp->i = fr->index, tmp->num = (double)fr->index;
            } else if (lt_seg_eq(path, len, "@first")) {
                tmp->kind = LT_D_BOOL, tmp->b = fr->index == 0;
            } else if (lt_seg_eq(path, len, "@last")) {
                tmp->kind = LT_D_BOOL, tmp->b = fr->index == fr->count - 1;
            } else if (lt_seg_eq(path, len, "@key") && fr->key) {
                tmp->kind = LT_D_STR, tmp->s = fr->key, tmp->slen = fr->klen;
            } else {
                break;
            }
            return tmp;
        }
        *err = lt_tpl_error(r->tpl->name, line, "unknown ", path, len);
        return NULL;
    }
    const char *dot = memchr(path, '.', (size_t)len);
    int64_t first = dot ? dot - path : len;
    // the first part: in the nearest frame that has it
    const lt_dyn *v = NULL;
    for (int i = f; i >= 0 && !v; i--) {
        const lt_dyn *o = r->frames[i].v;
        if (o && o->kind == LT_D_OBJ) {
            for (int64_t k = 0; k < o->n; k++)
                if (o->klens[k] == first && memcmp(o->keys[k], path, (size_t)first) == 0) {
                    v = &o->items[k];
                    break;
                }
        }
    }
    if (!v) {
        *err = lt_tpl_error(r->tpl->name, line, "not in the data: ", path, first);
        return NULL;
    }
    const char *rest = dot ? dot + 1 : NULL;
    while (rest && rest < path + len) {
        const char *d2 = memchr(rest, '.', (size_t)(path + len - rest));
        int64_t sl = d2 ? d2 - rest : path + len - rest;
        if (v->kind == LT_D_NULL) return v; // a missing optional: nothing further
        if (v->kind != LT_D_OBJ) {
            *err = lt_tpl_error(r->tpl->name, line, "not a record: ", path, rest - path - 1);
            return NULL;
        }
        const lt_dyn *next = NULL;
        for (int64_t k = 0; k < v->n; k++)
            if (v->klens[k] == sl && memcmp(v->keys[k], rest, (size_t)sl) == 0) next = &v->items[k];
        if (!next) {
            *err = lt_tpl_error(r->tpl->name, line, "not in the data: ", path, (rest - path) + sl);
            return NULL;
        }
        v = next;
        rest = d2 ? d2 + 1 : NULL;
    }
    return v;
}

static bool lt_truthy(const lt_dyn *v) {
    switch (v->kind) {
    case LT_D_NULL: return false;
    case LT_D_BOOL: return v->b;
    case LT_D_NUM: return v->num != 0;
    case LT_D_STR: return v->slen > 0;
    case LT_D_ARR: return v->n > 0;
    default: return true;
    }
}

static void lt_tescape(lt_buf *b, const char *s, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': lt_buf_put(b, "&amp;", 5); break;
        case '<': lt_buf_put(b, "&lt;", 4); break;
        case '>': lt_buf_put(b, "&gt;", 4); break;
        case '"': lt_buf_put(b, "&quot;", 6); break;
        case '\'': lt_buf_put(b, "&#39;", 5); break;
        default: lt_buf_c(b, s[i]);
        }
    }
}

static lt_err lt_trender_nodes(lt_trender *r, lt_tnode *nodes, int n);

static lt_err lt_trender_node(lt_trender *r, lt_tnode *x) {
    lt_err e = { 0 };
    lt_dyn tmp;
    if (x->kind == LT_T_TEXT) {
        lt_buf_put(&r->out, x->s, x->len);
        return e;
    }
    if (x->kind == LT_T_PARTIAL) {
        for (int i = 0; i < r->lib->n; i++) {
            lt_tpl *p = r->lib->t[i];
            if (lt_seg_eq(x->s, x->len, p->name)) {
                if (++r->partials > 64) return lt_tpl_error(r->tpl->name, x->line, "partials include each other endlessly", NULL, 0);
                lt_tpl *saved = r->tpl;
                r->tpl = p;
                e = lt_trender_nodes(r, p->root.kids, p->root.nkids);
                r->tpl = saved;
                r->partials--;
                return e;
            }
        }
        return lt_tpl_error(r->tpl->name, x->line, "no template named ", x->s, x->len);
    }
    const lt_dyn *v = lt_tlookup(r, x->s, x->len, x->line, &e, &tmp);
    if (!v) return e;
    if (x->kind == LT_T_VAR || x->kind == LT_T_RAW) {
        bool esc = x->kind == LT_T_VAR;
        char num[40];
        switch (v->kind) {
        case LT_D_NULL: break;
        case LT_D_BOOL: lt_buf_put(&r->out, v->b ? "true" : "false", v->b ? 4 : 5); break;
        case LT_D_NUM:
            if (v->s) lt_buf_put(&r->out, v->s, v->slen);
            else if (v->is_int) lt_buf_put(&r->out, num, snprintf(num, sizeof num, "%lld", (long long)v->i));
            else {
                lt_text *ft = lt_float_to_text(v->num);
                lt_buf_put(&r->out, ft->data, ft->len);
                lt_text_drop(ft);
            }
            break;
        case LT_D_STR:
            if (esc) lt_tescape(&r->out, v->s, v->slen);
            else lt_buf_put(&r->out, v->s, v->slen);
            break;
        default:
            return lt_tpl_error(r->tpl->name, x->line, v->kind == LT_D_ARR ? "a list can't be printed (use {{#each}}): " : "a record can't be printed (pick a field): ", x->s, x->len);
        }
        return e;
    }
    // a section
    if (r->nf >= 127) return lt_tpl_error(r->tpl->name, x->line, "blocks nested too deep", NULL, 0);
    switch (x->sec) {
    case LT_S_IF:
    case LT_S_UNLESS: {
        bool yes = lt_truthy(v) == (x->sec == LT_S_IF);
        return yes ? lt_trender_nodes(r, x->kids, x->nkids) : lt_trender_nodes(r, x->els, x->nels);
    }
    case LT_S_WITH:
        if (!lt_truthy(v)) return lt_trender_nodes(r, x->els, x->nels);
        r->frames[r->nf++] = (lt_tframe){ v, 0, 0, NULL, 0 };
        e = lt_trender_nodes(r, x->kids, x->nkids);
        r->nf--;
        return e;
    case LT_S_EACH:
        if ((v->kind != LT_D_ARR && v->kind != LT_D_OBJ) || v->n == 0) {
            if (v->kind != LT_D_ARR && v->kind != LT_D_OBJ && v->kind != LT_D_NULL) return lt_tpl_error(r->tpl->name, x->line, "#each needs a list or a map: ", x->s, x->len);
            return lt_trender_nodes(r, x->els, x->nels);
        }
        for (int64_t i = 0; i < v->n && !e.obj; i++) {
            lt_tframe fr = { &v->items[i], i, v->n, NULL, 0 };
            if (v->kind == LT_D_OBJ) fr.key = v->keys[i], fr.klen = v->klens[i];
            r->frames[r->nf++] = fr;
            e = lt_trender_nodes(r, x->kids, x->nkids);
            r->nf--;
        }
        return e;
    }
    return e;
}

static lt_err lt_trender_nodes(lt_trender *r, lt_tnode *nodes, int n) {
    for (int i = 0; i < n; i++) {
        lt_err e = lt_trender_node(r, &nodes[i]);
        if (e.obj) return e;
    }
    return (lt_err){ 0 };
}

static lt_tlib *lt_tlib_new(void) {
    lt_tlib *l = (lt_tlib *)calloc(1, sizeof(lt_tlib));
    l->h.rc = 1;
    l->h.free = lt_tlib_free;
    return l;
}

static lt_err lt_tlib_add(lt_tlib *l, const char *name, const char *src, int64_t len, bool escape) {
    lt_tpl *t = (lt_tpl *)calloc(1, sizeof(lt_tpl));
    t->name = strdup(name);
    t->src = (char *)malloc((size_t)len + 1);
    memcpy(t->src, src, (size_t)len);
    t->src[len] = 0;
    t->escape = escape;
    l->t = (lt_tpl **)realloc(l->t, sizeof(lt_tpl *) * (size_t)(l->n + 1));
    l->t[l->n++] = t;
    return lt_tpl_parse(t);
}

// template.compile / compile_text: a library with one unnamed template
static lt_err lt_template_compile(lt_text *src, bool escape, lt_handle **out) {
    lt_tlib *l = lt_tlib_new();
    lt_err e = lt_tlib_add(l, "", src->data, src->len, escape);
    if (e.obj) {
        lt_tlib_free(&l->h);
        return e;
    }
    *out = &l->h;
    return (lt_err){ 0 };
}

// template.load: every file under `dir`, named by its path without the
// extension ("pages/home"); files ending in .html/.htm/.xml/.svg escape HTML
static lt_err lt_template_load(lt_text *dir, lt_handle **out) {
    lt_texts *files = NULL;
    lt_err e = lt_files_walk(dir, &files);
    if (e.obj) return e;
    lt_tlib *l = lt_tlib_new();
    for (int64_t i = 0; i < files->len && !e.obj; i++) {
        lt_text *path = files->items[i];
        lt_text *content = NULL;
        e = lt_files_read_text(path, &content);
        if (e.obj) break;
        const char *rel = path->data + dir->len;
        while (*rel == '/') rel++;
        char name[512];
        snprintf(name, sizeof name, "%s", rel);
        char *dot = strrchr(name, '.');
        char *slash = strrchr(name, '/');
        bool escape = false;
        if (dot && (!slash || dot > slash)) {
            escape = strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0 || strcasecmp(dot, ".xml") == 0 || strcasecmp(dot, ".svg") == 0;
            *dot = 0;
        }
        e = lt_tlib_add(l, name, content->data, content->len, escape);
        lt_text_drop(content);
    }
    for (int64_t i = 0; i < files->len; i++) lt_text_drop(files->items[i]);
    if (files->cap) lt_free(files, sizeof(lt_texts) + sizeof(lt_text *) * (size_t)files->cap);
    if (e.obj) {
        lt_tlib_free(&l->h);
        return e;
    }
    *out = &l->h;
    return (lt_err){ 0 };
}

// Renders template `name` ("" for a compiled one) with `json` as the data.
static lt_err lt_template_render(lt_handle *h, lt_text *name, lt_text *json, lt_text **out) {
    lt_tlib *l = (lt_tlib *)h;
    lt_tpl *t = NULL;
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->t[i]->name, name ? name->data : "") == 0) t = l->t[i];
    if (!t) {
        char buf[300];
        snprintf(buf, sizeof buf, "template: no template named \"%s\"", name ? name->data : "");
        return lt_make_failure(lt_text_cstr(buf));
    }
    lt_arena ar = { 0 };
    lt_dyn data;
    lt_err e = lt_json_parse(json, &ar, &data);
    if (e.obj) {
        lt_arena_free(&ar);
        return e;
    }
    lt_trender r;
    memset(&r, 0, sizeof r);
    r.lib = l;
    r.tpl = t;
    r.frames[0] = (lt_tframe){ &data, 0, 0, NULL, 0 };
    r.nf = 1;
    e = lt_trender_nodes(&r, t->root.kids, t->root.nkids);
    lt_arena_free(&ar);
    if (e.obj) {
        lt_buf_free(&r.out);
        return e;
    }
    *out = lt_buf_text(&r.out);
    return (lt_err){ 0 };
}

static lt_texts *lt_template_names(lt_handle *h) {
    lt_tlib *l = (lt_tlib *)h;
    lt_texts *out = lt_texts_new(l->n);
    for (int i = 0; i < l->n; i++) lt_texts_push(&out, lt_text_cstr(l->t[i]->name));
    return out;
}
