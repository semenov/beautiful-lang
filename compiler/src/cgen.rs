// C code generation from MIR.
//
// Every concrete type gets a C representation and helper functions:
//   Int -> int64_t, Float -> double, Bool -> bool, Text -> lt_text*
//   records/enums -> structs by value (boxed behind a pointer if recursive)
//   List/Map/Set -> pointers to reference-counted objects
//   T? -> the same pointer with NULL for none when T is a pointer,
//         otherwise struct { bool some; T v; }
//   functions -> lt_fn { code, env }, interfaces -> lt_iface { obj, vtable }
// Helpers (dup/drop/eq/hash/cmp/to_text, list and map operations) are
// generated on demand.

use crate::ast::{BinOp, UnOp};
use crate::lower::iface_methods;
use crate::mir::*;
use crate::rc::needs_rc;
use crate::types::{DefId, Program, Ty, TypeKind};
use std::collections::{HashMap, HashSet};
use std::fmt::Write;

#[derive(Clone, Debug)]
enum Kind {
    Unit,
    Int,
    Float,
    Bool,
    Text,
    List(usize),
    Map(usize, usize),
    Set(usize),
    Record { name: String, fields: Vec<(String, usize)>, boxed: bool },
    Enum { name: String, variants: Vec<(String, Vec<(String, usize)>)>, boxed: bool },
    // niche: None is a zero pointer / zero struct field
    Opt { inner: usize, niche: Niche },
    Func,
    Iface,
    Never,
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Niche {
    Ptr,
    Iface,
    Func,
    No,
}

struct TInfo {
    c: String,
    kind: Kind,
    rc: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum H {
    Dup,
    Drop,
    Eq,
    Hash,
    Cmp,
    ToText,
    Ops, // list / map / set operations, record/enum boxes
    SortBy(usize, bool),
}

pub struct CGen<'a> {
    prog: &'a Program,
    m: &'a Module,
    tys: Vec<TInfo>,
    ty_ids: HashMap<Ty, usize>,
    requested: Vec<(H, usize)>,
    done: HashSet<(H, usize)>,
    protos: String,
    helper_types: String,
    helpers: String,
    lits: HashMap<String, usize>,
    lit_defs: String,
    cur_line: u32,
    cur_fn: usize,
    vt_index: HashMap<(Ty, Vec<DefId>), usize>,
    file: String,
}

fn c_str(s: &str) -> String {
    let mut out = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\t' => out.push_str("\\t"),
            b'?' => out.push_str("\\?"),
            32..=126 => out.push(b as char),
            _ => {
                let _ = write!(out, "\\{:03o}", b);
            }
        }
    }
    out.push('"');
    out
}

fn c_float(v: f64) -> String {
    if v.is_nan() {
        return "(0.0/0.0)".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "(1.0/0.0)".into() } else { "(-1.0/0.0)".into() };
    }
    let s = format!("{:?}", v);
    if s.contains('.') || s.contains('e') {
        s
    } else {
        format!("{}.0", s)
    }
}

fn c_int(v: i64) -> String {
    if v == i64::MIN {
        "INT64_MIN".into()
    } else {
        format!("INT64_C({})", v)
    }
}

impl<'a> CGen<'a> {
    pub fn new(prog: &'a Program, m: &'a Module, file: String) -> CGen<'a> {
        let mut vt_index = HashMap::new();
        for (i, v) in m.vtables.iter().enumerate() {
            vt_index.insert((v.concrete.clone(), v.iface.clone()), i);
        }
        CGen {
            prog,
            m,
            tys: vec![],
            ty_ids: HashMap::new(),
            requested: vec![],
            done: HashSet::new(),
            protos: String::new(),
            helper_types: String::new(),
            helpers: String::new(),
            lits: HashMap::new(),
            lit_defs: String::new(),
            cur_line: 0,
            cur_fn: 0,
            vt_index,
            file,
        }
    }

    // ================= types =================

    fn is_recursive(&self, t: &Ty) -> bool {
        // does `t` contain itself by value?
        fn by_value(p: &Program, t: &Ty) -> Vec<Ty> {
            match t {
                Ty::Opt(x) => vec![(**x).clone()],
                Ty::Adt(d, args) => match &p.defs[*d].kind {
                    TypeKind::Record { fields } => fields.iter().map(|f| f.ty.subst(args)).collect(),
                    TypeKind::Enum { variants } => variants.iter().flat_map(|v| v.fields.iter().map(|f| f.ty.subst(args))).collect(),
                    TypeKind::Newtype(i) => vec![i.clone()],
                    _ => vec![],
                },
                _ => vec![],
            }
        }
        let mut stack: Vec<Ty> = by_value(self.prog, t);
        let mut seen: HashSet<Ty> = HashSet::new();
        while let Some(x) = stack.pop() {
            if x == *t {
                return true;
            }
            if seen.insert(x.clone()) {
                stack.extend(by_value(self.prog, &x));
            }
        }
        false
    }

    fn niche_of(&self, t: &Ty) -> Niche {
        match t {
            Ty::Text | Ty::Iface(_) | Ty::Func(_) => match t {
                Ty::Text => Niche::Ptr,
                Ty::Iface(_) => Niche::Iface,
                _ => Niche::Func,
            },
            Ty::Adt(d, _) => match &self.prog.defs[*d].kind {
                TypeKind::Builtin => Niche::Ptr, // List, Map, Set
                TypeKind::Newtype(i) => {
                    let i = i.clone();
                    self.niche_of(&i)
                }
                TypeKind::Record { .. } | TypeKind::Enum { .. } => {
                    if self.is_recursive(t) {
                        Niche::Ptr
                    } else {
                        Niche::No
                    }
                }
                TypeKind::Interface { .. } => Niche::Iface,
            },
            _ => Niche::No,
        }
    }

    fn tid(&mut self, t: &Ty) -> usize {
        if let Some(i) = self.ty_ids.get(t) {
            return *i;
        }
        // new types are their inner type
        if let Ty::Adt(d, _) = t {
            if let TypeKind::Newtype(inner) = &self.prog.defs[*d].kind {
                let inner = inner.clone();
                let i = self.tid(&inner);
                self.ty_ids.insert(t.clone(), i);
                return i;
            }
        }
        let id = self.tys.len();
        let rc = needs_rc(t, self.prog);
        self.tys.push(TInfo { c: format!("T{}", id), kind: Kind::Unit, rc });
        self.ty_ids.insert(t.clone(), id);
        let (c, kind) = match t {
            Ty::Unit => ("void".to_string(), Kind::Unit),
            Ty::Never => ("void".to_string(), Kind::Never),
            Ty::Int => ("int64_t".to_string(), Kind::Int),
            Ty::Float => ("double".to_string(), Kind::Float),
            Ty::Bool => ("bool".to_string(), Kind::Bool),
            Ty::Text => ("lt_text*".to_string(), Kind::Text),
            Ty::Func(_) => ("lt_fn".to_string(), Kind::Func),
            Ty::Iface(_) => ("lt_iface".to_string(), Kind::Iface),
            Ty::Opt(inner) => {
                let niche = match **inner {
                    Ty::Opt(_) => Niche::No,
                    _ => self.niche_of(inner),
                };
                let it = self.tid(inner);
                let c = if niche == Niche::No { format!("T{}", id) } else { self.tys[it].c.clone() };
                (c, Kind::Opt { inner: it, niche })
            }
            Ty::Adt(d, args) => {
                let def = &self.prog.defs[*d];
                match &def.kind {
                    TypeKind::Builtin => {
                        if *d == self.prog.b.list {
                            let e = self.tid(&args[0]);
                            (format!("T{}", id), Kind::List(e))
                        } else if *d == self.prog.b.map {
                            let k = self.tid(&args[0]);
                            let v = self.tid(&args[1]);
                            (format!("T{}", id), Kind::Map(k, v))
                        } else if *d == self.prog.b.set {
                            let e = self.tid(&args[0]);
                            (format!("T{}", id), Kind::Set(e))
                        } else {
                            panic!("unexpected builtin type {}", def.name)
                        }
                    }
                    TypeKind::Record { fields } => {
                        let boxed = self.is_recursive(t);
                        let name = self.prog.show(t);
                        let fs: Vec<(String, Ty)> = fields.iter().map(|f| (f.name.clone(), f.ty.subst(args))).collect();
                        let fields = fs.into_iter().map(|(n, ft)| (n, self.tid(&ft))).collect();
                        (format!("T{}", id), Kind::Record { name, fields, boxed })
                    }
                    TypeKind::Enum { variants } => {
                        let boxed = self.is_recursive(t);
                        let name = self.prog.show(t);
                        let vs: Vec<(String, Vec<(String, Ty)>)> = variants
                            .iter()
                            .map(|v| (v.name.clone(), v.fields.iter().map(|f| (f.name.clone(), f.ty.subst(args))).collect()))
                            .collect();
                        let variants = vs.into_iter().map(|(n, fs)| (n, fs.into_iter().map(|(fnm, ft)| (fnm, self.tid(&ft))).collect())).collect();
                        (format!("T{}", id), Kind::Enum { name, variants, boxed })
                    }
                    TypeKind::Interface { .. } => ("lt_iface".to_string(), Kind::Iface),
                    TypeKind::Newtype(_) => unreachable!(),
                }
            }
            Ty::Param(_) | Ty::Var(_) | Ty::Err => panic!("non-concrete type reached code generation: {:?}", t),
        };
        self.tys[id].c = c;
        self.tys[id].kind = kind;
        id
    }

    fn cty(&mut self, t: &Ty) -> String {
        let i = self.tid(t);
        self.tys[i].c.clone()
    }

    fn is_unit(&mut self, t: &Ty) -> bool {
        matches!(t, Ty::Unit | Ty::Never)
    }

    fn need(&mut self, h: H, id: usize) {
        if !self.done.contains(&(h, id)) {
            self.done.insert((h, id));
            self.requested.push((h, id));
        }
    }

    fn dup(&mut self, id: usize, x: &str) -> String {
        if !self.tys[id].rc {
            return String::new();
        }
        self.need(H::Dup, id);
        format!("dup_{}({});", id, x)
    }
    fn drop_(&mut self, id: usize, x: &str) -> String {
        if !self.tys[id].rc {
            return String::new();
        }
        self.need(H::Drop, id);
        format!("drop_{}({});", id, x)
    }

    fn zero(&self, id: usize) -> String {
        let c = &self.tys[id].c;
        match &self.tys[id].kind {
            Kind::Int | Kind::Float => "0".into(),
            Kind::Bool => "false".into(),
            Kind::Text | Kind::List(_) | Kind::Map(..) | Kind::Set(_) => "NULL".into(),
            Kind::Record { boxed: true, .. } | Kind::Enum { boxed: true, .. } => "NULL".into(),
            Kind::Opt { niche: Niche::Ptr, .. } => "NULL".into(),
            _ => format!("(({}){{0}})", c),
        }
    }

    fn opt_some(&self, opt: usize, v: &str) -> String {
        match &self.tys[opt].kind {
            Kind::Opt { niche: Niche::No, .. } => format!("(({}){{ true, {} }})", self.tys[opt].c, v),
            _ => v.to_string(),
        }
    }
    fn opt_none(&self, opt: usize) -> String {
        self.zero(opt)
    }
    fn opt_is_some(&self, opt: usize, x: &str) -> String {
        match &self.tys[opt].kind {
            Kind::Opt { niche: Niche::No, .. } => format!("({}).some", x),
            Kind::Opt { niche: Niche::Ptr, .. } => format!("(({}) != NULL)", x),
            Kind::Opt { niche: Niche::Iface, .. } => format!("(({}).obj != NULL)", x),
            Kind::Opt { niche: Niche::Func, .. } => format!("(({}).fn != NULL)", x),
            k => panic!("opt_is_some on {:?}", k),
        }
    }
    fn opt_val(&self, opt: usize, x: &str) -> String {
        match &self.tys[opt].kind {
            Kind::Opt { niche: Niche::No, .. } => format!("({}).v", x),
            _ => x.to_string(),
        }
    }
    fn opt_inner(&self, opt: usize) -> usize {
        match &self.tys[opt].kind {
            Kind::Opt { inner, .. } => *inner,
            k => panic!("not an optional: {:?}", k),
        }
    }

    fn opt_tid_of(&mut self, inner: &Ty) -> usize {
        self.tid(&Ty::opt(inner.clone()))
    }

    fn lit(&mut self, s: &str) -> String {
        if let Some(i) = self.lits.get(s) {
            return format!("((lt_text*)&lit{})", i);
        }
        let i = self.lits.len();
        self.lits.insert(s.to_string(), i);
        let _ = writeln!(self.lit_defs, "static struct {{ int64_t rc; int64_t len; char data[{}]; }} lit{} = {{ -1, {}, {} }};", s.len() + 1, i, s.len(), c_str(s));
        format!("((lt_text*)&lit{})", i)
    }

    // ================= helpers =================

    fn gen_helpers(&mut self) {
        while let Some((h, id)) = self.requested.pop() {
            match h {
                H::Dup | H::Drop => self.gen_dup_drop(id, h == H::Dup),
                H::Eq => self.gen_eq(id),
                H::Hash => self.gen_hash(id),
                H::Cmp => self.gen_cmp(id),
                H::ToText => self.gen_totext(id),
                H::Ops => self.gen_ops(id),
                H::SortBy(k, throws) => self.gen_sort_by(id, k, throws),
            }
        }
    }

    fn gen_dup_drop(&mut self, id: usize, dup: bool) {
        let c = self.tys[id].c.clone();
        let kind = self.tys[id].kind.clone();
        let name = if dup { format!("dup_{}", id) } else { format!("drop_{}", id) };
        let _ = writeln!(self.protos, "static inline void {}({} x);", name, c);
        let mut b = String::new();
        let obj_rc = |free: &str| {
            if dup {
                "if (x && x->rc > 0) x->rc++;".to_string()
            } else {
                format!("if (x && x->rc > 0 && --x->rc == 0) {}(x);", free)
            }
        };
        match &kind {
            Kind::Text => b = if dup { "lt_text_dup(x);".into() } else { "lt_text_drop(x);".into() },
            Kind::Func => b = if dup { "lt_fn_dup(x);".into() } else { "lt_fn_drop(x);".into() },
            Kind::Iface => b = if dup { "lt_iface_dup(x);".into() } else { "lt_iface_drop(x);".into() },
            Kind::List(_) | Kind::Map(..) | Kind::Set(_) => {
                self.need(H::Ops, id);
                b = obj_rc(&format!("{}_free", c));
            }
            Kind::Record { fields, boxed, .. } => {
                if *boxed {
                    self.need(H::Ops, id);
                    b = obj_rc(&format!("{}_free", c));
                } else {
                    for (i, (_, f)) in fields.iter().enumerate() {
                        let s = if dup { self.dup(*f, &format!("x.f{}", i)) } else { self.drop_(*f, &format!("x.f{}", i)) };
                        b += &s;
                    }
                }
            }
            Kind::Enum { variants, boxed, .. } => {
                if *boxed {
                    self.need(H::Ops, id);
                    b = obj_rc(&format!("{}_free", c));
                } else {
                    b += "switch (x.tag) {";
                    for (vi, (_, fs)) in variants.iter().enumerate() {
                        let mut body = String::new();
                        for (fi, (_, f)) in fs.iter().enumerate() {
                            let e = format!("x.u.v{}.f{}", vi, fi);
                            body += &if dup { self.dup(*f, &e) } else { self.drop_(*f, &e) };
                        }
                        if !body.is_empty() {
                            let _ = write!(b, " case {}: {} break;", vi, body);
                        }
                    }
                    b += " default: break; }";
                }
            }
            Kind::Opt { inner, niche } => {
                let inner = *inner;
                if *niche == Niche::No {
                    let s = if dup { self.dup(inner, "x.v") } else { self.drop_(inner, "x.v") };
                    b = format!("if (x.some) {{ {} }}", s);
                } else {
                    b = if dup { self.dup(inner, "x") } else { self.drop_(inner, "x") };
                }
            }
            _ => {}
        }
        let _ = writeln!(self.helpers, "static inline void {}({} x) {{ {} }}", name, c, b);
    }

    fn eq_expr(&mut self, id: usize, a: &str, b: &str) -> String {
        match &self.tys[id].kind {
            Kind::Int | Kind::Float | Kind::Bool => format!("({} == {})", a, b),
            Kind::Text => format!("lt_text_eq({}, {})", a, b),
            _ => {
                self.need(H::Eq, id);
                format!("eq_{}({}, {})", id, a, b)
            }
        }
    }
    fn hash_expr(&mut self, id: usize, a: &str) -> String {
        match &self.tys[id].kind {
            Kind::Int => format!("lt_int_hash({})", a),
            Kind::Bool => format!("lt_int_hash((int64_t){})", a),
            Kind::Float => format!("lt_float_hash({})", a),
            Kind::Text => format!("lt_text_hash({})", a),
            _ => {
                self.need(H::Hash, id);
                format!("hash_{}({})", id, a)
            }
        }
    }
    fn cmp_expr(&mut self, id: usize, a: &str, b: &str) -> String {
        match &self.tys[id].kind {
            Kind::Int | Kind::Float | Kind::Bool => format!("(({a}) < ({b}) ? -1 : (({a}) > ({b}) ? 1 : 0))", a = a, b = b),
            Kind::Text => format!("lt_text_cmp({}, {})", a, b),
            _ => {
                self.need(H::Cmp, id);
                format!("cmp_{}({}, {})", id, a, b)
            }
        }
    }
    fn totext_expr(&mut self, id: usize, a: &str, debug: bool) -> String {
        match &self.tys[id].kind {
            Kind::Int => format!("lt_int_to_text({})", a),
            Kind::Float => format!("lt_float_to_text({})", a),
            Kind::Bool => format!("lt_bool_to_text({})", a),
            Kind::Text => {
                if debug {
                    format!("lt_text_quote({})", a)
                } else {
                    format!("lt_text_ret({})", a)
                }
            }
            _ => {
                self.need(H::ToText, id);
                format!("totext_{}({}, {})", id, a, debug)
            }
        }
    }

    fn gen_eq(&mut self, id: usize) {
        let c = self.tys[id].c.clone();
        let kind = self.tys[id].kind.clone();
        let _ = writeln!(self.protos, "static bool eq_{}({} a, {} b);", id, c, c);
        let body = match &kind {
            Kind::Record { fields, boxed, .. } => {
                let acc = if *boxed { "->v." } else { "." };
                if *boxed {
                    let mut s = "if (a == b) return true; ".to_string();
                    for (i, (_, f)) in fields.iter().enumerate() {
                        let e = self.eq_expr(*f, &format!("a{}f{}", acc, i), &format!("b{}f{}", acc, i));
                        let _ = write!(s, "if (!{}) return false; ", e);
                    }
                    s + "return true;"
                } else {
                    let mut s = String::new();
                    for (i, (_, f)) in fields.iter().enumerate() {
                        let e = self.eq_expr(*f, &format!("a.f{}", i), &format!("b.f{}", i));
                        let _ = write!(s, "if (!{}) return false; ", e);
                    }
                    s + "return true;"
                }
            }
            Kind::Enum { variants, boxed, .. } => {
                let p = if *boxed { "->v." } else { "." };
                let mut s = format!("if (a{p}tag != b{p}tag) return false; switch (a{p}tag) {{", p = p);
                for (vi, (_, fs)) in variants.iter().enumerate() {
                    if fs.is_empty() {
                        continue;
                    }
                    let _ = write!(s, " case {}:", vi);
                    for (fi, (_, f)) in fs.iter().enumerate() {
                        let e = self.eq_expr(*f, &format!("a{}u.v{}.f{}", p, vi, fi), &format!("b{}u.v{}.f{}", p, vi, fi));
                        let _ = write!(s, " if (!{}) return false;", e);
                    }
                    s += " break;";
                }
                s + " default: break; } return true;"
            }
            Kind::Opt { inner, niche } => {
                let inner = *inner;
                let sa = self.opt_is_some(id, "a");
                let sb = self.opt_is_some(id, "b");
                let va = self.opt_val(id, "a");
                let vb = self.opt_val(id, "b");
                let e = self.eq_expr(inner, &va, &vb);
                let _ = niche;
                format!("if ({} != {}) return false; if (!{}) return true; return {};", sa, sb, sa, e)
            }
            Kind::List(e) => {
                let e = *e;
                let x = self.eq_expr(e, "a->items[i]", "b->items[i]");
                format!("if (a == b) return true; if (a->len != b->len) return false; for (int64_t i = 0; i < a->len; i++) if (!{}) return false; return true;", x)
            }
            Kind::Map(_, v) => {
                let v = *v;
                self.need(H::Ops, id);
                let x = self.eq_expr(v, "a->e[i].v", "b->e[j].v");
                format!("if (a == b) return true; if (a->len != b->len) return false; for (int64_t i = 0; i < a->n; i++) {{ if (!a->e[i].h) continue; int64_t j = {c}_find(b, a->e[i].k, a->e[i].h); if (j < 0 || !{x}) return false; }} return true;", c = c, x = x)
            }
            Kind::Set(_) => {
                self.need(H::Ops, id);
                format!("if (a == b) return true; if (a->len != b->len) return false; for (int64_t i = 0; i < a->n; i++) {{ if (!a->e[i].h) continue; if ({c}_find(b, a->e[i].k, a->e[i].h) < 0) return false; }} return true;", c = c)
            }
            Kind::Iface => "if (a.obj == b.obj) return true; if (a.vt->type_id != b.vt->type_id) return false; return a.vt->eq(a.obj, b.obj);".into(),
            Kind::Func => "(void)a; (void)b; lt_panic_at(\"functions can't be compared\", 0);".into(),
            Kind::Int | Kind::Float | Kind::Bool => "return a == b;".into(),
            Kind::Text => "return lt_text_eq(a, b);".into(),
            _ => "(void)a; (void)b; return true;".into(),
        };
        let _ = writeln!(self.helpers, "static bool eq_{}({} a, {} b) {{ {} }}", id, c, c, body);
    }

    fn gen_hash(&mut self, id: usize) {
        let c = self.tys[id].c.clone();
        let kind = self.tys[id].kind.clone();
        let _ = writeln!(self.protos, "static uint64_t hash_{}({} a);", id, c);
        let body = match &kind {
            Kind::Record { fields, boxed, .. } => {
                let acc = if *boxed { "->v." } else { "." };
                let mut s = "uint64_t h = 7;".to_string();
                for (i, (_, f)) in fields.iter().enumerate() {
                    let e = self.hash_expr(*f, &format!("a{}f{}", acc, i));
                    let _ = write!(s, " h = lt_hash_combine(h, {});", e);
                }
                s + " return h;"
            }
            Kind::Enum { variants, boxed, .. } => {
                let p = if *boxed { "->v." } else { "." };
                let mut s = format!("uint64_t h = lt_int_hash(a{}tag); switch (a{}tag) {{", p, p);
                for (vi, (_, fs)) in variants.iter().enumerate() {
                    if fs.is_empty() {
                        continue;
                    }
                    let _ = write!(s, " case {}:", vi);
                    for (fi, (_, f)) in fs.iter().enumerate() {
                        let e = self.hash_expr(*f, &format!("a{}u.v{}.f{}", p, vi, fi));
                        let _ = write!(s, " h = lt_hash_combine(h, {});", e);
                    }
                    s += " break;";
                }
                s + " default: break; } return h;"
            }
            Kind::Opt { inner, .. } => {
                let inner = *inner;
                let sa = self.opt_is_some(id, "a");
                let va = self.opt_val(id, "a");
                let e = self.hash_expr(inner, &va);
                format!("return {} ? lt_hash_combine(1, {}) : 0;", sa, e)
            }
            Kind::List(e) => {
                let e = *e;
                let x = self.hash_expr(e, "a->items[i]");
                format!("uint64_t h = 3; for (int64_t i = 0; i < a->len; i++) h = lt_hash_combine(h, {}); return h;", x)
            }
            Kind::Map(k, v) => {
                let (k, v) = (*k, *v);
                let hk = self.hash_expr(k, "a->e[i].k");
                let hv = self.hash_expr(v, "a->e[i].v");
                format!("uint64_t h = 5; for (int64_t i = 0; i < a->n; i++) if (a->e[i].h) h += lt_hash_combine({}, {}); return lt_mix(h);", hk, hv)
            }
            Kind::Set(e) => {
                let e = *e;
                let hk = self.hash_expr(e, "a->e[i].k");
                format!("uint64_t h = 9; for (int64_t i = 0; i < a->n; i++) if (a->e[i].h) h += {}; return lt_mix(h);", hk)
            }
            Kind::Iface => "return a.vt->hash(a.obj);".into(),
            Kind::Int => "return lt_int_hash(a);".into(),
            Kind::Float => "return lt_float_hash(a);".into(),
            Kind::Bool => "return lt_int_hash(a);".into(),
            Kind::Text => "return lt_text_hash(a);".into(),
            _ => "(void)a; return 0;".into(),
        };
        let _ = writeln!(self.helpers, "static uint64_t hash_{}({} a) {{ {} }}", id, c, body);
    }

    fn gen_cmp(&mut self, id: usize) {
        let c = self.tys[id].c.clone();
        let _ = writeln!(self.protos, "static int64_t cmp_{}({} a, {} b);", id, c, c);
        let body = match &self.tys[id].kind {
            Kind::Int | Kind::Float | Kind::Bool => "return a < b ? -1 : (a > b ? 1 : 0);".to_string(),
            Kind::Text => "return lt_text_cmp(a, b);".into(),
            _ => "(void)a; (void)b; lt_panic_at(\"these values can't be ordered\", 0);".into(),
        };
        let _ = writeln!(self.helpers, "static int64_t cmp_{}({} a, {} b) {{ {} }}", id, c, c, body);
    }

    fn gen_totext(&mut self, id: usize) {
        let c = self.tys[id].c.clone();
        let kind = self.tys[id].kind.clone();
        let _ = writeln!(self.protos, "static lt_text *totext_{}({} a, bool debug);", id, c);
        // builds a list of parts and concatenates
        let mut s = String::from("lt_texts *p = lt_texts_new(8); (void)debug;");
        let lit = |g: &mut CGen, s: &mut String, t: &str| {
            let l = g.lit(t);
            let _ = write!(s, " lt_texts_push(&p, {});", l);
        };
        match &kind {
            Kind::Record { name, fields, boxed } => {
                let acc = if *boxed { "->v." } else { "." };
                lit(self, &mut s, &format!("{}(", name));
                for (i, (fname, f)) in fields.iter().enumerate() {
                    lit(self, &mut s, &format!("{}{}: ", if i > 0 { ", " } else { "" }, fname));
                    let e = self.totext_expr(*f, &format!("a{}f{}", acc, i), true);
                    let _ = write!(s, " lt_texts_push(&p, {});", e);
                }
                lit(self, &mut s, ")");
            }
            Kind::Enum { variants, boxed, .. } => {
                let p = if *boxed { "->v." } else { "." };
                let _ = write!(s, " switch (a{}tag) {{", p);
                for (vi, (vname, fs)) in variants.iter().enumerate() {
                    let _ = write!(s, " case {}:", vi);
                    if fs.is_empty() {
                        lit(self, &mut s, vname);
                    } else {
                        lit(self, &mut s, &format!("{}(", vname));
                        for (fi, (fname, f)) in fs.iter().enumerate() {
                            lit(self, &mut s, &format!("{}{}: ", if fi > 0 { ", " } else { "" }, fname));
                            let e = self.totext_expr(*f, &format!("a{}u.v{}.f{}", p, vi, fi), true);
                            let _ = write!(s, " lt_texts_push(&p, {});", e);
                        }
                        lit(self, &mut s, ")");
                    }
                    s += " break;";
                }
                s += " default: break; }";
            }
            Kind::Opt { inner, .. } => {
                let inner = *inner;
                let sa = self.opt_is_some(id, "a");
                let va = self.opt_val(id, "a");
                let e = self.totext_expr(inner, &va, true);
                let none = self.lit("none");
                let _ = write!(s, " if ({}) lt_texts_push(&p, {}); else lt_texts_push(&p, {});", sa, e, none);
            }
            Kind::List(e) => {
                let e = *e;
                lit(self, &mut s, "[");
                let x = self.totext_expr(e, "a->items[i]", true);
                let sep = self.lit(", ");
                let _ = write!(s, " for (int64_t i = 0; i < a->len; i++) {{ if (i) lt_texts_push(&p, {}); lt_texts_push(&p, {}); }}", sep, x);
                lit(self, &mut s, "]");
            }
            Kind::Map(k, v) => {
                let (k, v) = (*k, *v);
                lit(self, &mut s, "{");
                let xk = self.totext_expr(k, "a->e[i].k", true);
                let xv = self.totext_expr(v, "a->e[i].v", true);
                let sep = self.lit(", ");
                let colon = self.lit(": ");
                let _ = write!(s, " bool first = true; for (int64_t i = 0; i < a->n; i++) {{ if (!a->e[i].h) continue; if (!first) lt_texts_push(&p, {}); first = false; lt_texts_push(&p, {}); lt_texts_push(&p, {}); lt_texts_push(&p, {}); }}", sep, xk, colon, xv);
                lit(self, &mut s, "}");
            }
            Kind::Set(e) => {
                let e = *e;
                lit(self, &mut s, "{");
                let xk = self.totext_expr(e, "a->e[i].k", true);
                let sep = self.lit(", ");
                let _ = write!(s, " bool first = true; for (int64_t i = 0; i < a->n; i++) {{ if (!a->e[i].h) continue; if (!first) lt_texts_push(&p, {}); first = false; lt_texts_push(&p, {}); }}", sep, xk);
                lit(self, &mut s, "}");
            }
            Kind::Iface => {
                s += " lt_texts_push(&p, a.vt->to_text(a.obj, debug));";
            }
            Kind::Func => {
                lit(self, &mut s, "<function>");
            }
            Kind::Int => s += " lt_texts_push(&p, lt_int_to_text(a));",
            Kind::Float => s += " lt_texts_push(&p, lt_float_to_text(a));",
            Kind::Bool => s += " lt_texts_push(&p, lt_bool_to_text(a));",
            Kind::Text => s += " lt_texts_push(&p, debug ? lt_text_quote(a) : lt_text_ret(a));",
            _ => {}
        }
        let texts = self.tid(&Ty::Adt(self.prog.b.list, vec![Ty::Text]));
        self.need(H::Ops, texts);
        let _ = write!(s, " lt_text *r = lt_text_concat_n((int)p->len, p->items); drop_{}(p); return r;", texts);
        self.need(H::Drop, texts);
        let _ = writeln!(self.helpers, "static lt_text *totext_{}({} a, bool debug) {{ {} }}", id, c, s);
    }

    // list / map / set operations and boxes
    fn gen_ops(&mut self, id: usize) {
        let kind = self.tys[id].kind.clone();
        match kind {
            Kind::List(e) => self.gen_list(id, e),
            Kind::Map(k, v) => self.gen_map(id, k, Some(v)),
            Kind::Set(e) => self.gen_map(id, e, None),
            Kind::Record { boxed: true, fields, .. } => self.gen_box(id, fields.iter().map(|f| f.1).collect(), None),
            Kind::Enum { boxed: true, variants, .. } => {
                let vs: Vec<Vec<usize>> = variants.iter().map(|v| v.1.iter().map(|f| f.1).collect()).collect();
                self.gen_box(id, vec![], Some(vs))
            }
            _ => {}
        }
    }

    fn gen_box(&mut self, id: usize, fields: Vec<usize>, variants: Option<Vec<Vec<usize>>>) {
        let c = self.tys[id].c.clone();
        let mut drops = String::new();
        let mut dups = String::new();
        match &variants {
            None => {
                for (i, f) in fields.iter().enumerate() {
                    drops += &self.drop_(*f, &format!("x->v.f{}", i));
                    dups += &self.dup(*f, &format!("n->v.f{}", i));
                }
            }
            Some(vs) => {
                drops += "switch (x->v.tag) {";
                dups += "switch (n->v.tag) {";
                for (vi, fs) in vs.iter().enumerate() {
                    let _ = write!(drops, " case {}:", vi);
                    let _ = write!(dups, " case {}:", vi);
                    for (fi, f) in fs.iter().enumerate() {
                        drops += &self.drop_(*f, &format!("x->v.u.v{}.f{}", vi, fi));
                        dups += &self.dup(*f, &format!("n->v.u.v{}.f{}", vi, fi));
                    }
                    drops += " break;";
                    dups += " break;";
                }
                drops += " default: break; }";
                dups += " default: break; }";
            }
        }
        let _ = writeln!(self.protos, "static void {c}_free({c} x);", c = c);
        let _ = writeln!(self.helpers, "static void {c}_free({c} x) {{ {d} lt_free(x, sizeof(*x)); }}", c = c, d = drops);
        let _ = writeln!(self.helpers, "static inline {c} {c}_box({c}_v v) {{ {c} n = ({c})lt_alloc(sizeof(*n)); n->rc = 1; n->v = v; return n; }}", c = c);
        let _ = writeln!(self.helpers, "static inline void {c}_unique({c} *p) {{ {c} x = *p; if (x->rc == 1) return; {c} n = ({c})lt_alloc(sizeof(*n)); n->rc = 1; n->v = x->v; {dups} if (x->rc > 0) x->rc--; *p = n; }}", c = c, dups = dups);
        let _ = writeln!(self.protos, "static inline {c} {c}_box({c}_v v);\nstatic inline void {c}_unique({c} *p);", c = c);
    }

    fn gen_list(&mut self, id: usize, e: usize) {
        let l = self.tys[id].c.clone();
        let ec = self.tys[e].c.clone();
        let st = if matches!(self.tys[e].kind, Kind::Text) { "lt_texts".to_string() } else { format!("struct {}_s", l) };
        let opt = {
            // T? for pop/min/max
            let ety = self.ty_of(e);
            self.opt_tid_of(&ety)
        };
        let oc = self.tys[opt].c.clone();
        let drop_e = self.drop_(e, "l->items[i]");
        let dup_e = self.dup(e, "n->items[i]");
        let dup_v = self.dup(e, "v");
        let some_v = self.opt_some(opt, "v");
        let none = self.opt_none(opt);
        let cmp = self.cmp_expr(e, "a", "b");
        let cmp_mm = self.cmp_expr(e, "l->items[i]", "best");
        let is_int = matches!(self.tys[e].kind, Kind::Int);
        let is_float = matches!(self.tys[e].kind, Kind::Float);
        let mut s = String::new();
        let _ = write!(
            s,
            r#"
static {l} {l}_new(int64_t cap) {{ if (cap <= 0) return ({l})&lt_empty_list; {l} l = ({l})lt_alloc({l}_SIZE(cap)); l->rc = 1; l->len = 0; l->cap = cap; return l; }}
static void {l}_free({l} l) {{ for (int64_t i = 0; i < l->len; i++) {{ {drop_e} }} lt_free(l, {l}_SIZE(l->cap)); }}
static {l} {l}_clone({l} l, int64_t cap) {{ {l} n = {l}_new(cap); memcpy(n->items, l->items, sizeof({ec}) * (size_t)l->len); n->len = l->len; for (int64_t i = 0; i < n->len; i++) {{ {dup_e} }} return n; }}
static void {l}_grow({l} *p, int64_t need) {{
  {l} l = *p; int64_t nc = l->cap * 2; if (nc < need) nc = need; if (nc < 4) nc = 4;
  if (l->rc == 1) {{ l = ({l})lt_realloc(l, {l}_SIZE(l->cap), {l}_SIZE(nc)); l->cap = nc; *p = l; }}
  else {{ {l} n = {l}_clone(l, nc); if (l->rc > 0) l->rc--; *p = n; }}
}}
static inline void {l}_unique({l} *p) {{ {l} l = *p; if (l->rc != 1 && l->len > 0) {{ {l} n = {l}_clone(l, l->len); if (l->rc > 0) l->rc--; *p = n; }} }}
static inline void {l}_push({l} *p, {ec} v) {{ {l} l = *p; if (LT_UNLIKELY(l->rc != 1 || l->len == l->cap)) {{ {l}_grow(p, l->len + 1); l = *p; }} l->items[l->len++] = v; }}
static {l} {l}_lit(int64_t n, {ec} *items) {{ {l} l = {l}_new(n); memcpy(l->items, items, sizeof({ec}) * (size_t)n); l->len = n; return l; }}
static inline {ec} *{l}_at({l} l, int64_t i, int line) {{ if (LT_UNLIKELY((uint64_t)i >= (uint64_t)l->len)) lt_index_panic(i, l->len, line); return &l->items[i]; }}
static inline {ec} {l}_get({l} l, int64_t i, int line) {{ {ec} v = *{l}_at(l, i, line); {dup_v} return v; }}
static inline {ec} {l}_get_unchecked({l} l, int64_t i) {{ {ec} v = l->items[i]; {dup_v} return v; }}
static {l} {l}_slice({l} l, int64_t from, int64_t to) {{ if (from < 0) from = 0; if (to > l->len) to = l->len; if (to <= from) return ({l})&lt_empty_list; {l} n = {l}_new(to - from); memcpy(n->items, l->items + from, sizeof({ec}) * (size_t)(to - from)); n->len = to - from; for (int64_t i = 0; i < n->len; i++) {{ {dup_e} }} return n; }}
static void {l}_append_all({l} *p, {l} o) {{
  if (o->len == 0) {{ drop_{id}(o); return; }}
  {l} l = *p; if (l->rc != 1 || l->len + o->len > l->cap) {{ {l}_grow(p, l->len + o->len); l = *p; }}
  memcpy(l->items + l->len, o->items, sizeof({ec}) * (size_t)o->len);
  if (o->rc == 1) {{ l->len += o->len; lt_free(o, {l}_SIZE(o->cap)); }}
  else {{ int64_t start = l->len; l->len += o->len; for (int64_t k = start; k < l->len; k++) {{ {ec} v = l->items[k]; {dup_v} }} drop_{id}(o); }}
}}
static void {l}_insert({l} *p, {ec} v, int64_t at, int line) {{
  {l} l = *p; if (at < 0 || at > l->len) lt_index_panic(at, l->len, line);
  if (l->rc != 1 || l->len == l->cap) {{ {l}_grow(p, l->len + 1); l = *p; }}
  memmove(l->items + at + 1, l->items + at, sizeof({ec}) * (size_t)(l->len - at)); l->items[at] = v; l->len++;
}}
static void {l}_remove_at({l} *p, int64_t at, int line) {{
  {l}_at(*p, at, line); {l}_unique(p); {l} l = *p; int64_t i = at; {drop_e}
  memmove(l->items + at, l->items + at + 1, sizeof({ec}) * (size_t)(l->len - at - 1)); l->len--;
}}
static {oc} {l}_pop({l} *p) {{ if ((*p)->len == 0) return {none}; {l}_unique(p); {l} l = *p; {ec} v = l->items[--l->len]; return {some_v}; }}
static void {l}_reverse({l} *p) {{ {l}_unique(p); {l} l = *p; for (int64_t i = 0, j = l->len - 1; i < j; i++, j--) {{ {ec} t = l->items[i]; l->items[i] = l->items[j]; l->items[j] = t; }} }}
static void {l}_clear({l} *p) {{ {l} l = *p; if (l->rc == 1) {{ for (int64_t i = 0; i < l->len; i++) {{ {drop_e} }} l->len = 0; }} else {{ drop_{id}(l); *p = ({l})&lt_empty_list; }} }}
static inline int64_t {l}_cmp({ec} a, {ec} b) {{ return {cmp}; }}
static void {l}_msort({ec} *a, int64_t n, {ec} *tmp) {{
  if (n <= 16) {{ for (int64_t i = 1; i < n; i++) {{ {ec} x = a[i]; int64_t j = i - 1; while (j >= 0 && {l}_cmp(a[j], x) > 0) {{ a[j + 1] = a[j]; j--; }} a[j + 1] = x; }} return; }}
  int64_t h = n / 2; {l}_msort(a, h, tmp); {l}_msort(a + h, n - h, tmp);
  if ({l}_cmp(a[h - 1], a[h]) <= 0) return;
  memcpy(tmp, a, sizeof({ec}) * (size_t)h);
  int64_t i = 0, j = h, k = 0;
  while (i < h && j < n) {{ if ({l}_cmp(a[j], tmp[i]) < 0) a[k++] = a[j++]; else a[k++] = tmp[i++]; }}
  while (i < h) a[k++] = tmp[i++];
}}
static void {l}_sort({l} *p) {{ {l}_unique(p); {l} l = *p; if (l->len < 2) return; {ec} *tmp = ({ec} *)malloc(sizeof({ec}) * (size_t)(l->len / 2 + 1)); {l}_msort(l->items, l->len, tmp); free(tmp); }}
"#,
            l = l,
            ec = ec,
            id = id,
            drop_e = drop_e.replace("l->items[i]", "l->items[i]"),
            dup_e = dup_e,
            dup_v = dup_v,
            oc = oc,
            none = none,
            some_v = some_v,
            cmp = cmp,
        );
        let _ = writeln!(self.helper_types, "#define {l}_SIZE(cap) (sizeof({st}) + sizeof({ec}) * (size_t)(cap))", l = l, st = st, ec = ec);
        self.need(H::Drop, id);
        self.need(H::Dup, id);
        // sum / min / max
        if is_int || is_float {
            let add = if is_int { "r = lt_add(r, l->items[i], line);" } else { "r += l->items[i];" };
            let _ = writeln!(s, "static {ec} {l}_sum({l} l, int line) {{ {ec} r = 0; (void)line; for (int64_t i = 0; i < l->len; i++) {add} return r; }}", ec = ec, l = l, add = add);
        }
        let _ = writeln!(
            s,
            "static {oc} {l}_minmax({l} l, int sign) {{ if (l->len == 0) return {none}; {ec} best = l->items[0]; for (int64_t i = 1; i < l->len; i++) if ({cmp_mm} * sign < 0) best = l->items[i]; {ec} v = best; {dup_v} return {some_v}; }}",
            oc = oc,
            l = l,
            none = none,
            ec = ec,
            cmp_mm = cmp_mm,
            dup_v = dup_v,
            some_v = some_v
        );
        // prototypes
        let _ = writeln!(self.protos, "static {l} {l}_new(int64_t cap);\nstatic void {l}_free({l} l);\nstatic inline void {l}_push({l} *p, {ec} v);\nstatic {l} {l}_lit(int64_t n, {ec} *items);", l = l, ec = ec);
        self.helpers += &s;
    }

    fn gen_map(&mut self, id: usize, k: usize, v: Option<usize>) {
        let m = self.tys[id].c.clone();
        let kc = self.tys[k].c.clone();
        let (vc, vid) = match v {
            Some(v) => (self.tys[v].c.clone(), Some(v)),
            None => ("bool".to_string(), None),
        };
        let hk = self.hash_expr(k, "key");
        let eqk = self.eq_expr(k, "e->k", "key");
        let dupk_key = self.dup(k, "key");
        let dropk = self.drop_(k, "m->e[i].k");
        let dupk_n = self.dup(k, "n->e[n->n].k");
        let (dropv, dupv_n, dupv_v, dropv_slot) = match vid {
            Some(v) => (self.drop_(v, "m->e[i].v"), self.dup(v, "n->e[n->n].v"), self.dup(v, "v"), self.drop_(v, "*slot")),
            None => (String::new(), String::new(), String::new(), String::new()),
        };
        let dropkey = self.drop_(k, "key");
        let mut s = String::new();
        let _ = write!(
            s,
            r#"
static {m} {m}_new(int64_t cap) {{
  {m} m = ({m})lt_alloc(sizeof(struct {m}_s)); m->rc = 1; m->len = 0; m->n = 0;
  if (cap < 4) cap = 4; m->cap = cap; m->e = ({m}_e *)malloc(sizeof({m}_e) * (size_t)cap);
  int64_t ic = 8; while (ic < cap * 2) ic *= 2; m->icap = ic; m->idx = (int32_t *)malloc(sizeof(int32_t) * (size_t)ic); memset(m->idx, 0xff, sizeof(int32_t) * (size_t)ic);
  return m;
}}
static void {m}_free({m} m) {{ for (int64_t i = 0; i < m->n; i++) {{ if (!m->e[i].h) continue; {dropk} {dropv} }} free(m->e); free(m->idx); lt_free(m, sizeof(struct {m}_s)); }}
static int64_t {m}_find({m} m, {kc} key, uint64_t h) {{
  if (m->icap == 0) return -1; uint64_t mask = (uint64_t)m->icap - 1; uint64_t i = h & mask;
  for (;;) {{ int32_t s = m->idx[i]; if (s < 0) return -1; {m}_e *e = &m->e[s]; if (e->h == h && {eqk}) return s; i = (i + 1) & mask; }}
}}
static inline uint64_t {m}_hash({kc} key) {{ return {hk} | 1; }}
// rebuild with room for `cap` entries, dropping deleted ones
static {m} {m}_rebuild({m} m, int64_t cap, bool owned) {{
  {m} n = {m}_new(cap);
  for (int64_t i = 0; i < m->n; i++) {{
    if (!m->e[i].h) continue; n->e[n->n] = m->e[i];
    if (!owned) {{ {dupk_n} {dupv_n} }}
    uint64_t mask = (uint64_t)n->icap - 1; uint64_t j = n->e[n->n].h & mask; while (n->idx[j] >= 0) j = (j + 1) & mask; n->idx[j] = (int32_t)n->n; n->n++;
  }}
  n->len = n->n;
  if (owned) {{ free(m->e); free(m->idx); lt_free(m, sizeof(struct {m}_s)); }} else if (m->rc > 0) m->rc--;
  return n;
}}
static inline void {m}_unique({m} *p) {{ {m} m = *p; if (m->rc != 1) *p = {m}_rebuild(m, m->len, false); }}
// the value slot for `key` (borrowed), adding a zeroed entry if missing
static {vc} *{m}_slot_insert({m} *p, {kc} key) {{
  {m} m = *p; uint64_t h = {m}_hash(key);
  if (m->rc == 1) {{ int64_t f = {m}_find(m, key, h); if (f >= 0) return &m->e[f].v; }}
  else {{ int64_t f = {m}_find(m, key, h); m = *p = {m}_rebuild(m, m->len + (f < 0), false); if (f >= 0) return &m->e[{m}_find(m, key, h)].v; }}
  if (m->n == m->cap || (m->n + 1) * 2 > m->icap) {{ m = *p = {m}_rebuild(m, m->len < m->n / 2 ? m->cap : m->cap * 2, true); }}
  {m}_e *e = &m->e[m->n]; e->h = h; e->k = key; {dupk_key} memset(&e->v, 0, sizeof(e->v));
  uint64_t mask = (uint64_t)m->icap - 1; uint64_t j = h & mask; while (m->idx[j] >= 0) j = (j + 1) & mask; m->idx[j] = (int32_t)m->n;
  m->n++; m->len++; return &e->v;
}}
static {vc} *{m}_slot({m} *p, {kc} key, int line) {{
  {m}_unique(p); {m} m = *p; int64_t f = {m}_find(m, key, {m}_hash(key));
  if (f < 0) lt_panic_at("the key is not in the map", line); return &m->e[f].v;
}}
static void {m}_put({m} *p, {kc} key, {vc} v) {{ {vc} *slot = {m}_slot_insert(p, key); {dropv_slot} *slot = v; {dropkey} }}
static bool {m}_contains({m} m, {kc} key) {{ return {m}_find(m, key, {m}_hash(key)) >= 0; }}
static void {m}_remove({m} *p, {kc} key) {{
  if ({m}_find(*p, key, {m}_hash(key)) < 0) return; {m}_unique(p); {m} m = *p; int64_t i = {m}_find(m, key, {m}_hash(key));
  {dropk} {dropv} m->e[i].h = 0; m->len--;
}}
static void {m}_clear({m} *p) {{ drop_{id}(*p); *p = {m}_new(4); }}
"#,
            m = m,
            kc = kc,
            vc = vc,
            id = id,
            hk = hk,
            eqk = eqk,
            dupk_key = dupk_key,
            dropk = dropk,
            dropv = dropv,
            dupk_n = dupk_n,
            dupv_n = dupv_n,
            dropv_slot = dropv_slot,
            dropkey = dropkey,
        );
        let _ = writeln!(self.helper_types, "typedef struct {{ uint64_t h; {kc} k; {vc} v; }} {m}_e;\nstruct {m}_s {{ int64_t rc; int64_t len; int64_t n; int64_t cap; int64_t icap; {m}_e *e; int32_t *idx; }};", kc = kc, vc = vc, m = m);
        self.need(H::Drop, id);
        self.need(H::Dup, id);
        let _ = dupv_v;
        let _ = writeln!(self.protos, "struct {m}_s;\nstatic void {m}_free({m} m);", m = m);
        self.helpers += &s;
    }

    fn gen_sort_by(&mut self, list: usize, key: usize, throws: bool) {
        let l = self.tys[list].c.clone();
        let e = match self.tys[list].kind {
            Kind::List(e) => e,
            _ => unreachable!(),
        };
        let ec = self.tys[e].c.clone();
        let kc = self.tys[key].c.clone();
        let dup_x = self.dup(e, "x");
        let drop_k = self.drop_(key, "a[i].k");
        let cmp = self.cmp_expr(key, "x.k", "y.k");
        let name = format!("{}_sort_by_{}{}", l, key, if throws { "t" } else { "" });
        let pair = format!("{}_kp{}", l, key);
        let call = if throws {
            format!("{kc} kv; lt_err err = ((lt_err (*)(lt_env *, {ec}, {kc} *))f.fn)(f.env, x, &kv); if (err.obj) {{ for (int64_t i = 0; i < k; i++) {{ {drop_k} }} free(a); return err; }} a[k].k = kv;", kc = kc, ec = ec, drop_k = drop_k)
        } else {
            format!("a[k].k = (({kc} (*)(lt_env *, {ec}))f.fn)(f.env, x);", kc = kc, ec = ec)
        };
        let ret_t = if throws { "lt_err" } else { "void" };
        let ret_ok = if throws { "return (lt_err){0};" } else { "return;" };
        if !self.done.contains(&(H::Ops, usize::MAX / 2 + list * 1000 + key)) {
            self.done.insert((H::Ops, usize::MAX / 2 + list * 1000 + key));
            let _ = writeln!(self.helper_types, "typedef struct {{ {kc} k; {ec} e; }} {pair};", kc = kc, ec = ec, pair = pair);
            let _ = write!(
                self.helpers,
                r#"
static inline int64_t {pair}_cmp({pair} x, {pair} y) {{ return {cmp}; }}
static void {pair}_msort({pair} *a, int64_t n, {pair} *tmp) {{
  if (n <= 16) {{ for (int64_t i = 1; i < n; i++) {{ {pair} x = a[i]; int64_t j = i - 1; while (j >= 0 && {pair}_cmp(a[j], x) > 0) {{ a[j + 1] = a[j]; j--; }} a[j + 1] = x; }} return; }}
  int64_t h = n / 2; {pair}_msort(a, h, tmp); {pair}_msort(a + h, n - h, tmp);
  if ({pair}_cmp(a[h - 1], a[h]) <= 0) return;
  memcpy(tmp, a, sizeof({pair}) * (size_t)h);
  int64_t i = 0, j = h, k = 0;
  while (i < h && j < n) {{ if ({pair}_cmp(a[j], tmp[i]) < 0) a[k++] = a[j++]; else a[k++] = tmp[i++]; }}
  while (i < h) a[k++] = tmp[i++];
}}
"#,
                pair = pair,
                cmp = cmp
            );
        }
        let _ = writeln!(self.protos, "static {} {}({} *p, lt_fn f);", ret_t, name, l);
        let _ = write!(
            self.helpers,
            r#"
static {ret_t} {name}({l} *p, lt_fn f) {{
  {l}_unique(p); {l} l = *p; int64_t n = l->len; if (n < 2) {ret_ok}
  {pair} *a = ({pair} *)malloc(sizeof({pair}) * (size_t)n);
  for (int64_t k = 0; k < n; k++) {{ {ec} x = l->items[k]; {dup_x} {call} a[k].e = l->items[k]; }}
  {pair} *tmp = ({pair} *)malloc(sizeof({pair}) * (size_t)(n / 2 + 1));
  {pair}_msort(a, n, tmp);
  for (int64_t k = 0; k < n; k++) l->items[k] = a[k].e;
  for (int64_t i = 0; i < n; i++) {{ {drop_k} }}
  free(a); free(tmp); {ret_ok}
}}
"#,
            ret_t = ret_t,
            name = name,
            l = l,
            ec = ec,
            pair = pair,
            dup_x = dup_x,
            call = call,
            drop_k = drop_k,
            ret_ok = ret_ok
        );
    }

    fn ty_of(&self, id: usize) -> Ty {
        for (t, i) in &self.ty_ids {
            if *i == id {
                if let Ty::Adt(d, _) = t {
                    if matches!(self.prog.defs[*d].kind, TypeKind::Newtype(_)) {
                        continue;
                    }
                }
                return t.clone();
            }
        }
        panic!("unknown type id {}", id)
    }

    // ================= type definitions =================

    fn emit_types(&mut self) -> String {
        let mut out = String::new();
        // forward declarations of pointer types
        for id in 0..self.tys.len() {
            let c = self.tys[id].c.clone();
            match &self.tys[id].kind {
                Kind::List(e) if matches!(self.tys[*e].kind, Kind::Text) => {
                    let _ = writeln!(out, "typedef lt_texts *{};", c);
                }
                Kind::List(_) | Kind::Map(..) | Kind::Set(_) | Kind::Record { boxed: true, .. } | Kind::Enum { boxed: true, .. } => {
                    let _ = writeln!(out, "typedef struct {}_s *{};", c, c);
                }
                _ => {}
            }
        }
        // value structs in dependency order
        let mut emitted = vec![false; self.tys.len()];
        for id in 0..self.tys.len() {
            self.emit_value_struct(id, &mut emitted, &mut out);
        }
        // list bodies (need element types)
        for id in 0..self.tys.len() {
            if let Kind::List(e) = &self.tys[id].kind {
                if !matches!(self.tys[*e].kind, Kind::Text) {
                    let _ = writeln!(out, "struct {}_s {{ int64_t rc; int64_t len; int64_t cap; {} items[]; }};", self.tys[id].c, self.tys[*e].c);
                }
            }
        }
        out
    }

    fn emit_value_struct(&self, id: usize, emitted: &mut Vec<bool>, out: &mut String) {
        if emitted[id] {
            return;
        }
        emitted[id] = true;
        let c = &self.tys[id].c;
        let deps: Vec<usize> = match &self.tys[id].kind {
            Kind::Record { fields, .. } => fields.iter().map(|f| f.1).collect(),
            Kind::Enum { variants, .. } => variants.iter().flat_map(|v| v.1.iter().map(|f| f.1)).collect(),
            Kind::Opt { inner, niche: Niche::No } => vec![*inner],
            _ => vec![],
        };
        for d in deps {
            if self.by_value(d) {
                self.emit_value_struct(d, emitted, out);
            }
        }
        match &self.tys[id].kind {
            Kind::Record { name, fields, boxed } => {
                let mut body = String::new();
                for (i, (_, f)) in fields.iter().enumerate() {
                    let _ = write!(body, " {} f{};", self.tys[*f].c, i);
                }
                if fields.is_empty() {
                    body = " char _unused;".into();
                }
                if *boxed {
                    let _ = writeln!(out, "typedef struct {{{} }} {}_v; // {}", body, c, name);
                    let _ = writeln!(out, "struct {}_s {{ int64_t rc; {}_v v; }};", c, c);
                } else {
                    let _ = writeln!(out, "typedef struct {{{} }} {}; // {}", body, c, name);
                }
            }
            Kind::Enum { name, variants, boxed } => {
                let mut u = String::new();
                for (vi, (_, fs)) in variants.iter().enumerate() {
                    if fs.is_empty() {
                        continue;
                    }
                    let mut fb = String::new();
                    for (fi, (_, f)) in fs.iter().enumerate() {
                        let _ = write!(fb, " {} f{};", self.tys[*f].c, fi);
                    }
                    let _ = write!(u, " struct {{{} }} v{};", fb, vi);
                }
                let body = if u.is_empty() { " int32_t tag;".to_string() } else { format!(" int32_t tag; union {{{} }} u;", u) };
                if *boxed {
                    let _ = writeln!(out, "typedef struct {{{} }} {}_v; // {}", body, c, name);
                    let _ = writeln!(out, "struct {}_s {{ int64_t rc; {}_v v; }};", c, c);
                } else {
                    let _ = writeln!(out, "typedef struct {{{} }} {}; // {}", body, c, name);
                }
            }
            Kind::Opt { inner, niche: Niche::No } => {
                let _ = writeln!(out, "typedef struct {{ bool some; {} v; }} {};", self.tys[*inner].c, c);
            }
            _ => {}
        }
    }

    fn by_value(&self, id: usize) -> bool {
        matches!(self.tys[id].kind, Kind::Record { boxed: false, .. } | Kind::Enum { boxed: false, .. } | Kind::Opt { niche: Niche::No, .. })
    }

    // ================= functions =================

    fn fn_sig(&mut self, fi: usize) -> String {
        let f = &self.m.funcs[fi];
        let name = f.name.clone();
        let mut ps = vec![];
        if let FnKind::Closure(_) = f.kind {
            ps.push("lt_env *env_".to_string());
        }
        let params = f.params.clone();
        let mutating = f.mutating;
        let throws = f.throws;
        let ret = f.ret.clone();
        for (i, p) in params.iter().enumerate() {
            let t = self.m.funcs[fi].locals[*p].ty.clone();
            if self.is_unit(&t) {
                continue;
            }
            let c = self.cty(&t);
            if i == 0 && mutating {
                ps.push(format!("{} *l{}", c, p));
            } else {
                ps.push(format!("{} l{}", c, p));
            }
        }
        let rc = if self.is_unit(&ret) { "void".to_string() } else { self.cty(&ret) };
        if throws {
            if !self.is_unit(&ret) {
                ps.push(format!("{} *out_", rc));
            }
            if ps.is_empty() {
                ps.push("void".into());
            }
            format!("static lt_err {}({})", name, ps.join(", "))
        } else {
            if ps.is_empty() {
                ps.push("void".into());
            }
            format!("static {} {}({})", rc, name, ps.join(", "))
        }
    }

    // C function pointer type for calling a closure / interface method
    fn fn_ptr_type(&mut self, first: &str, params: &[Ty], ret: &Ty, throws: bool) -> String {
        let mut ps = vec![first.to_string()];
        for p in params {
            if self.is_unit(p) {
                continue;
            }
            ps.push(self.cty(p));
        }
        let rc = if self.is_unit(ret) { "void".to_string() } else { self.cty(ret) };
        if throws {
            if !self.is_unit(ret) {
                ps.push(format!("{} *", rc));
            }
            format!("lt_err (*)({})", ps.join(", "))
        } else {
            format!("{} (*)({})", rc, ps.join(", "))
        }
    }

    fn op_ty(&self, fi: usize, o: &Op) -> Ty {
        match o {
            Op::Local(l) => self.m.funcs[fi].locals[*l].ty.clone(),
            Op::Int(_) => Ty::Int,
            Op::Float(_) => Ty::Float,
            Op::Bool(_) => Ty::Bool,
            Op::Text(_) => Ty::Text,
            Op::Unit => Ty::Unit,
        }
    }

    fn op(&mut self, o: &Op) -> String {
        match o {
            Op::Local(l) => format!("l{}", l),
            Op::Int(v) => c_int(*v),
            Op::Float(v) => c_float(*v),
            Op::Bool(b) => b.to_string(),
            Op::Text(s) => self.lit(s),
            Op::Unit => String::new(),
        }
    }

    fn local_is_unit(&mut self, fi: usize, l: L) -> bool {
        let t = self.m.funcs[fi].locals[l].ty.clone();
        self.is_unit(&t)
    }

    fn line(&self) -> u32 {
        self.cur_line
    }

    pub fn gen_function(&mut self, fi: usize, out: &mut String) {
        self.cur_fn = fi;
        let f = &self.m.funcs[fi];
        let sig = self.fn_sig(fi);
        let _ = writeln!(self.protos, "{};", sig);
        let mut body = String::new();
        let _ = writeln!(body, "{} {{ // {}", sig, f.source_name);
        let params: HashSet<L> = f.params.iter().copied().collect();
        let nlocals = f.locals.len();
        for l in 0..nlocals {
            if params.contains(&l) {
                continue;
            }
            let t = self.m.funcs[fi].locals[l].ty.clone();
            if self.is_unit(&t) {
                continue;
            }
            let tid = self.tid(&t);
            let c = self.tys[tid].c.clone();
            let z = self.zero(tid);
            let _ = writeln!(body, "  {} l{} = {};", c, l, z);
        }
        let nblocks = self.m.funcs[fi].blocks.len();
        for bi in 0..nblocks {
            let blk = &self.m.funcs[fi].blocks[bi];
            self.cur_line = blk.line;
            let _ = writeln!(body, "b{}:;", bi);
            let stmts = blk.stmts.clone();
            let term = blk.term.clone();
            for s in &stmts {
                self.stmt(fi, s, &mut body);
            }
            self.term(fi, &term, &mut body);
        }
        body += "}\n";
        out.push_str(&body);
    }

    fn place_lvalue_read(&mut self, fi: usize, p: &Place) -> String {
        let root = &self.m.funcs[fi].locals[p.root];
        let mut e = if root.self_ptr { format!("(*l{})", p.root) } else { format!("l{}", p.root) };
        let mut ty = root.ty.clone();
        let line = self.line();
        for (proj, next) in &p.path {
            let tid = self.tid(&ty);
            e = match proj {
                Proj::Field(i) => match &self.tys[tid].kind {
                    Kind::Record { boxed: true, .. } => format!("{}->v.f{}", e, i),
                    _ => format!("{}.f{}", e, i),
                },
                Proj::Index(o) => {
                    let o = self.op(o);
                    self.need(H::Ops, tid);
                    format!("(*{}_at({}, {}, {}))", self.tys[tid].c, e, o, line)
                }
                Proj::MapKey(o, _) => {
                    let o = self.op(o);
                    self.need(H::Ops, tid);
                    format!("(*{}_slot(&{}, {}, {}))", self.tys[tid].c, e, o, line)
                }
            };
            ty = next.clone();
        }
        e
    }

    // A pointer to the place, making every step unique (copy-on-write).
    fn place_ptr(&mut self, fi: usize, p: &Place, out: &mut String) -> (String, Ty) {
        let root = &self.m.funcs[fi].locals[p.root];
        let mut e = if root.self_ptr { format!("l{}", p.root) } else { format!("(&l{})", p.root) };
        let mut ty = root.ty.clone();
        let line = self.line();
        for (proj, next) in &p.path {
            let tid = self.tid(&ty);
            let c = self.tys[tid].c.clone();
            e = match proj {
                Proj::Field(i) => match &self.tys[tid].kind {
                    Kind::Record { boxed: true, .. } => {
                        self.need(H::Ops, tid);
                        let _ = writeln!(out, "  {}_unique({});", c, e);
                        format!("(&(*{})->v.f{})", e, i)
                    }
                    _ => format!("(&(*{}).f{})", e, i),
                },
                Proj::Index(o) => {
                    let o = self.op(o);
                    self.need(H::Ops, tid);
                    let _ = writeln!(out, "  {}_at(*{}, {}, {}); {}_unique({});", c, e, o, line, c, e);
                    format!("{}_at(*{}, {}, {})", c, e, o, line)
                }
                Proj::MapKey(o, insert) => {
                    let o = self.op(o);
                    self.need(H::Ops, tid);
                    let tmp = format!("p{}_{}", p.root, out.len());
                    let vc = {
                        let nt = next.clone();
                        self.cty(&nt)
                    };
                    if *insert {
                        let _ = writeln!(out, "  {} *{} = {}_slot_insert({}, {});", vc, tmp, c, e, o);
                    } else {
                        let _ = writeln!(out, "  {} *{} = {}_slot({}, {}, {});", vc, tmp, c, e, o, line);
                    }
                    tmp
                }
            };
            ty = next.clone();
        }
        (e, ty)
    }

    fn stmt(&mut self, fi: usize, s: &Stmt, out: &mut String) {
        match s {
            Stmt::Line(n) => self.cur_line = *n,
            Stmt::Dup(l) => {
                let t = self.m.funcs[fi].locals[*l].ty.clone();
                let tid = self.tid(&t);
                let d = self.dup(tid, &format!("l{}", l));
                if !d.is_empty() {
                    let _ = writeln!(out, "  {}", d);
                }
            }
            Stmt::Drop(l) => {
                let t = self.m.funcs[fi].locals[*l].ty.clone();
                let tid = self.tid(&t);
                let d = self.drop_(tid, &format!("l{}", l));
                if !d.is_empty() {
                    let _ = writeln!(out, "  {}", d);
                }
            }
            Stmt::Assign(dst, rv) => self.assign(fi, *dst, rv, out),
            Stmt::CallT { dst, err, callee, args } => {
                let mut argv = self.call_args(fi, callee, args);
                if let Some(d) = dst {
                    if !self.local_is_unit(fi, *d) {
                        let t = self.m.funcs[fi].locals[*d].ty.clone();
                        let tid = self.tid(&t);
                        let z = self.zero(tid);
                        let _ = writeln!(out, "  l{} = {};", d, z);
                        argv.push(format!("&l{}", d));
                    }
                }
                let call = self.callee_expr(fi, callee, &argv, args, true);
                let _ = writeln!(out, "  l{} = {};", err, call);
            }
            Stmt::Store(p, v) => {
                let (ptr, ty) = self.place_ptr(fi, p, out);
                let tid = self.tid(&ty);
                let vs = self.op(v);
                let d = self.drop_(tid, &format!("*{}", ptr));
                let _ = writeln!(out, "  {{ {} *sp = {}; {} *sp = {}; }}", self.tys[tid].c, ptr, d.replace(&format!("*{}", ptr), "*sp"), vs);
            }
            Stmt::MutCall { dst, err, place, callee, args } => {
                let (ptr, ty) = self.place_ptr(fi, place, out);
                let mut argv: Vec<String> = vec![];
                for a in args {
                    let t = self.op_ty(fi, a);
                    if self.is_unit(&t) {
                        continue;
                    }
                    argv.push(self.op(a));
                }
                let dst_unit = dst.map(|d| self.local_is_unit(fi, d)).unwrap_or(true);
                let call = match callee {
                    Callee::Fn(idx) => {
                        let mut a = vec![ptr.clone()];
                        a.extend(argv);
                        if err.is_some() && !dst_unit {
                            a.push(format!("&l{}", dst.unwrap()));
                        }
                        format!("{}({})", self.m.funcs[*idx].name, a.join(", "))
                    }
                    Callee::Iface(slot) => {
                        // `ptr` points at an interface value
                        let recv_ty = ty.clone();
                        let ids = match &recv_ty {
                            Ty::Iface(ids) => ids.clone(),
                            _ => unreachable!(),
                        };
                        let mid = iface_methods(self.prog, &ids)[*slot];
                        let mf = &self.prog.fns[mid];
                        let ptys: Vec<Ty> = mf.params.iter().map(|p| p.1.clone()).collect();
                        let fp = self.fn_ptr_type("lt_obj *", &ptys, &mf.ret.clone(), mf.throws);
                        let _ = writeln!(out, "  lt_iface_unique({});", ptr);
                        let mut a = vec![format!("({})->obj", ptr)];
                        a.extend(argv);
                        if err.is_some() && !dst_unit {
                            a.push(format!("&l{}", dst.unwrap()));
                        }
                        format!("(({})(({})->vt->m[{}]))({})", fp, ptr, slot, a.join(", "))
                    }
                    Callee::Intrinsic(name, tys) => self.mut_intrinsic(fi, name, tys, &ptr, &argv, args, err.is_some()),
                    Callee::Closure(_) => unreachable!(),
                };
                match (err, dst) {
                    (Some(e), Some(d)) if !dst_unit => {
                        let t = self.m.funcs[fi].locals[*d].ty.clone();
                        let tid = self.tid(&t);
                        let z = self.zero(tid);
                        let _ = writeln!(out, "  l{} = {};", d, z);
                        let _ = writeln!(out, "  l{} = {};", e, call);
                    }
                    (Some(e), _) => {
                        let _ = writeln!(out, "  l{} = {};", e, call);
                    }
                    (None, Some(d)) if !dst_unit => {
                        let _ = writeln!(out, "  l{} = {};", d, call);
                    }
                    _ => {
                        let _ = writeln!(out, "  {};", call);
                    }
                }
            }
        }
    }

    fn call_args(&mut self, fi: usize, callee: &Callee, args: &[Op]) -> Vec<String> {
        let mut v = vec![];
        for (i, a) in args.iter().enumerate() {
            let t = self.op_ty(fi, a);
            if self.is_unit(&t) {
                continue;
            }
            if let Callee::Iface(_) = callee {
                if i == 0 {
                    continue;
                }
            }
            v.push(self.op(a));
        }
        v
    }

    fn callee_expr(&mut self, fi: usize, callee: &Callee, argv: &[String], args: &[Op], throws: bool) -> String {
        match callee {
            Callee::Fn(idx) => format!("{}({})", self.m.funcs[*idx].name, argv.join(", ")),
            Callee::Closure(f) => {
                let fty = self.op_ty(fi, f);
                let ft = match &fty {
                    Ty::Func(ft) => (**ft).clone(),
                    _ => unreachable!(),
                };
                let fp = self.fn_ptr_type("lt_env *", &ft.params, &ft.ret, ft.throws);
                let fv = self.op(f);
                let mut a = vec![format!("{}.env", fv)];
                a.extend(argv.iter().cloned());
                format!("(({})({}.fn))({})", fp, fv, a.join(", "))
            }
            Callee::Iface(slot) => {
                let recv = &args[0];
                let rty = self.op_ty(fi, recv);
                let ids = match &rty {
                    Ty::Iface(ids) => ids.clone(),
                    _ => unreachable!(),
                };
                let mid = iface_methods(self.prog, &ids)[*slot];
                let mf = self.prog.fns[mid].clone();
                let ptys: Vec<Ty> = mf.params.iter().map(|p| p.1.clone()).collect();
                let fp = self.fn_ptr_type("lt_obj *", &ptys, &mf.ret, mf.throws);
                let r = self.op(recv);
                let mut a = vec![format!("{}.obj", r)];
                a.extend(argv.iter().cloned());
                format!("(({})({}.vt->m[{}]))({})", fp, r, slot, a.join(", "))
            }
            Callee::Intrinsic(name, tys) => self.intrinsic(fi, name, tys, argv, args, throws),
        }
    }

    fn assign(&mut self, fi: usize, dst: L, rv: &Rv, out: &mut String) {
        let dty = self.m.funcs[fi].locals[dst].ty.clone();
        let unit = self.is_unit(&dty);
        let did = if unit { usize::MAX } else { self.tid(&dty) };
        let line = self.line();
        let set = |out: &mut String, e: String| {
            if unit {
                let _ = writeln!(out, "  {};", e);
            } else {
                let _ = writeln!(out, "  l{} = {};", dst, e);
            }
        };
        // borrowed reads: assign, then dup
        let dup_after = |g: &mut CGen, out: &mut String| {
            if !unit {
                let d = g.dup(did, &format!("l{}", dst));
                if !d.is_empty() {
                    let _ = writeln!(out, "  {}", d);
                }
            }
        };
        match rv {
            Rv::Use(o) => {
                if !unit && !matches!(o, Op::Unit) {
                    let e = self.op(o);
                    set(out, e);
                }
            }
            Rv::Read(p) => {
                let e = self.place_lvalue_read(fi, p);
                set(out, e);
                dup_after(self, out);
            }
            Rv::Field(o, i) => {
                let ot = self.op_ty(fi, o);
                let tid = self.tid(&ot);
                let e = self.op(o);
                let acc = match &self.tys[tid].kind {
                    Kind::Record { boxed: true, .. } => format!("{}->v.f{}", e, i),
                    _ => format!("{}.f{}", e, i),
                };
                set(out, acc);
                dup_after(self, out);
            }
            Rv::Tag(o) => {
                let ot = self.op_ty(fi, o);
                let tid = self.tid(&ot);
                let e = self.op(o);
                let acc = match &self.tys[tid].kind {
                    Kind::Enum { boxed: true, .. } => format!("{}->v.tag", e),
                    _ => format!("{}.tag", e),
                };
                set(out, acc);
            }
            Rv::VariantField(o, v, i) => {
                let ot = self.op_ty(fi, o);
                let tid = self.tid(&ot);
                let e = self.op(o);
                let acc = match &self.tys[tid].kind {
                    Kind::Enum { boxed: true, .. } => format!("{}->v.u.v{}.f{}", e, v, i),
                    _ => format!("{}.u.v{}.f{}", e, v, i),
                };
                set(out, acc);
                dup_after(self, out);
            }
            Rv::OptIsSome(o) => {
                let ot = self.op_ty(fi, o);
                let tid = self.tid(&ot);
                let e = self.op(o);
                let x = self.opt_is_some(tid, &e);
                set(out, x);
            }
            Rv::OptGet(o, panic) => {
                let ot = self.op_ty(fi, o);
                let tid = self.tid(&ot);
                let e = self.op(o);
                if *panic {
                    let c = self.opt_is_some(tid, &e);
                    let _ = writeln!(out, "  if (!{}) lt_panic_at(\"the key is not in the map\", {});", c, line);
                }
                let x = self.opt_val(tid, &e);
                set(out, x);
                dup_after(self, out);
            }
            Rv::IfaceIs(o, t) => {
                let e = self.op(o);
                let tid = self.tid(t);
                set(out, format!("({}.vt->type_id == {})", e, tid));
            }
            Rv::IfaceGet(o, t) => {
                let e = self.op(o);
                let tid = self.tid(t);
                set(out, format!("((B{}*){}.obj)->v", tid, e));
                self.need_box(tid);
                dup_after(self, out);
            }
            Rv::EnvField(i) => {
                let fname = &self.m.funcs[fi].name;
                set(out, format!("((Env_{}*)env_)->c{}", fname, i));
                dup_after(self, out);
            }
            Rv::Bin(op, a, b) => {
                let at = self.op_ty(fi, a);
                let x = self.op(a);
                let y = self.op(b);
                let e = match (&at, op) {
                    (Ty::Int, BinOp::Add) => format!("lt_add({}, {}, {})", x, y, line),
                    (Ty::Int, BinOp::Sub) => format!("lt_sub({}, {}, {})", x, y, line),
                    (Ty::Int, BinOp::Mul) => format!("lt_mul({}, {}, {})", x, y, line),
                    (Ty::Int, BinOp::Div) => format!("lt_div({}, {}, {})", x, y, line),
                    (Ty::Int, BinOp::Rem) => format!("lt_rem({}, {}, {})", x, y, line),
                    (_, BinOp::Add) => format!("({} + {})", x, y),
                    (_, BinOp::Sub) => format!("({} - {})", x, y),
                    (_, BinOp::Mul) => format!("({} * {})", x, y),
                    (_, BinOp::Div) => format!("({} / {})", x, y),
                    (_, BinOp::Rem) => format!("fmod({}, {})", x, y),
                    (_, BinOp::Eq) => format!("({} == {})", x, y),
                    (_, BinOp::Ne) => format!("({} != {})", x, y),
                    (_, BinOp::Lt) => format!("({} < {})", x, y),
                    (_, BinOp::Le) => format!("({} <= {})", x, y),
                    (_, BinOp::Gt) => format!("({} > {})", x, y),
                    (_, BinOp::Ge) => format!("({} >= {})", x, y),
                    (_, BinOp::And) => format!("({} && {})", x, y),
                    (_, BinOp::Or) => format!("({} || {})", x, y),
                };
                set(out, e);
            }
            Rv::Un(op, a) => {
                let at = self.op_ty(fi, a);
                let x = self.op(a);
                let e = match (op, &at) {
                    (UnOp::Not, _) => format!("(!{})", x),
                    (UnOp::Neg, Ty::Int) => format!("lt_neg({}, {})", x, line),
                    (UnOp::Neg, _) => format!("(-{})", x),
                };
                set(out, e);
            }
            Rv::Eq(a, b) => {
                let at = self.op_ty(fi, a);
                let tid = self.tid(&at);
                let x = self.op(a);
                let y = self.op(b);
                let e = self.eq_expr(tid, &x, &y);
                set(out, e);
            }
            Rv::Cmp(a, b) => {
                let at = self.op_ty(fi, a);
                let tid = self.tid(&at);
                let x = self.op(a);
                let y = self.op(b);
                let e = self.cmp_expr(tid, &x, &y);
                set(out, e);
            }
            Rv::Record(ops) => {
                let vals: Vec<String> = ops.iter().map(|o| self.op(o)).collect();
                let c = self.tys[did].c.clone();
                match &self.tys[did].kind {
                    Kind::Record { boxed: true, .. } => {
                        self.need(H::Ops, did);
                        let body = if vals.is_empty() { "0".into() } else { vals.join(", ") };
                        set(out, format!("{}_box(({}_v){{ {} }})", c, c, body));
                    }
                    _ => {
                        let body = if vals.is_empty() { "0".into() } else { vals.join(", ") };
                        set(out, format!("(({}){{ {} }})", c, body));
                    }
                }
            }
            Rv::Variant(idx, ops) => {
                let vals: Vec<String> = ops.iter().map(|o| self.op(o)).collect();
                let c = self.tys[did].c.clone();
                let inner = if vals.is_empty() { format!(".tag = {}", idx) } else { format!(".tag = {}, .u.v{} = {{ {} }}", idx, idx, vals.join(", ")) };
                match &self.tys[did].kind {
                    Kind::Enum { boxed: true, .. } => {
                        self.need(H::Ops, did);
                        set(out, format!("{}_box(({}_v){{ {} }})", c, c, inner));
                    }
                    _ => set(out, format!("(({}){{ {} }})", c, inner)),
                }
            }
            Rv::Some(o) => {
                let e = self.op(o);
                let x = self.opt_some(did, &e);
                set(out, x);
            }
            Rv::None => {
                let z = self.opt_none(did);
                set(out, z);
            }
            Rv::List(ops) => {
                self.need(H::Ops, did);
                let c = self.tys[did].c.clone();
                if ops.is_empty() {
                    set(out, format!("(({})&lt_empty_list)", c));
                } else {
                    let e = match &self.tys[did].kind {
                        Kind::List(e) => *e,
                        _ => unreachable!(),
                    };
                    let ec = self.tys[e].c.clone();
                    let vals: Vec<String> = ops.iter().map(|o| self.op(o)).collect();
                    set(out, format!("{}_lit({}, ({}[]){{ {} }})", c, vals.len(), ec, vals.join(", ")));
                }
            }
            Rv::Map(pairs) => {
                self.need(H::Ops, did);
                let c = self.tys[did].c.clone();
                let _ = writeln!(out, "  l{} = {}_new({});", dst, c, pairs.len());
                for (k, v) in pairs {
                    let (k, v) = (self.op(k), self.op(v));
                    let _ = writeln!(out, "  {}_put(&l{}, {}, {});", c, dst, k, v);
                }
            }
            Rv::EmptySet => {
                self.need(H::Ops, did);
                let c = self.tys[did].c.clone();
                set(out, format!("{}_new(4)", c));
            }
            Rv::Closure(idx, caps) => {
                let fname = self.m.funcs[*idx].name.clone();
                if caps.is_empty() {
                    set(out, format!("((lt_fn){{ (void *){}, NULL }})", fname));
                } else {
                    let vals: Vec<String> = caps.iter().map(|o| self.op(o)).collect();
                    let _ = writeln!(out, "  {{ Env_{f} *e_ = (Env_{f} *)lt_alloc(sizeof(Env_{f})); e_->h.rc = 1; e_->h.drop = Env_{f}_drop;", f = fname);
                    for (i, v) in vals.iter().enumerate() {
                        let _ = writeln!(out, "    e_->c{} = {};", i, v);
                    }
                    let _ = writeln!(out, "    l{} = (lt_fn){{ (void *){}, &e_->h }}; }}", dst, fname);
                }
            }
            Rv::ToIface(o, from) => {
                let e = self.op(o);
                let ids = match &dty {
                    Ty::Iface(ids) => ids.clone(),
                    _ => unreachable!(),
                };
                let vt = self.vt_index[&(from.clone(), ids)];
                set(out, format!("to_iface_vt{}({})", vt, e));
            }
            Rv::Call(callee, args) => {
                let argv = self.call_args(fi, callee, args);
                let e = self.callee_expr(fi, callee, &argv, args, false);
                set(out, e);
            }
        }
    }

    fn need_box(&mut self, _tid: usize) {}

    fn term(&mut self, fi: usize, t: &Term, out: &mut String) {
        let f = &self.m.funcs[fi];
        let throws = f.throws;
        let ret_unit = matches!(f.ret, Ty::Unit | Ty::Never);
        match t {
            Term::Goto(b) => {
                let _ = writeln!(out, "  goto b{};", b);
            }
            Term::If(c, a, b) => {
                let c = self.op(c);
                let _ = writeln!(out, "  if ({}) goto b{}; else goto b{};", c, a, b);
            }
            Term::IfErr(l, a, b) => {
                let _ = writeln!(out, "  if (l{}.obj) goto b{}; else goto b{};", l, a, b);
            }
            Term::Switch(o, cases, d) => {
                let o = self.op(o);
                let _ = write!(out, "  switch ({}) {{", o);
                for (v, b) in cases {
                    let _ = write!(out, " case {}: goto b{};", v, b);
                }
                let _ = writeln!(out, " default: goto b{}; }}", d);
            }
            Term::Return(o) => {
                let v = self.op(o);
                if throws {
                    if !ret_unit && !v.is_empty() {
                        let _ = writeln!(out, "  *out_ = {};", v);
                    }
                    let _ = writeln!(out, "  return (lt_err){{0}};");
                } else if ret_unit || v.is_empty() {
                    let _ = writeln!(out, "  return;");
                } else {
                    let _ = writeln!(out, "  return {};", v);
                }
            }
            Term::Throw(o) => {
                let v = self.op(o);
                if throws {
                    let _ = writeln!(out, "  return {};", v);
                } else {
                    let _ = writeln!(out, "  lt_panic_error({}, {});", v, self.line());
                }
            }
            Term::Unreachable => {
                let _ = writeln!(out, "  lt_panic_at(\"unreachable code was reached (a compiler bug)\", {});", self.line());
            }
        }
    }

    // ================= intrinsics =================

    fn intrinsic(&mut self, fi: usize, name: &str, tys: &[Ty], a: &[String], args: &[Op], throws: bool) -> String {
        let line = self.line();
        let t0 = tys.first().cloned();
        let tid0 = t0.as_ref().map(|t| self.tid(t));
        let lc = tid0.map(|t| self.tys[t].c.clone()).unwrap_or_default();
        let _ = throws;
        let ops_of = |g: &mut CGen| {
            if let Some(t) = tid0 {
                g.need(H::Ops, t);
            }
        };
        match name {
            "int_inc" => format!("({} + 1)", a[0]),
            "Int.to_float" => format!("((double){})", a[0]),
            "Int.to_text" => format!("lt_int_to_text({})", a[0]),
            "Int.div" => format!("lt_div({}, {}, {})", a[0], a[1], line),
            "Int.abs" => format!("lt_abs({}, {})", a[0], line),
            "Int.pow" => format!("lt_ipow({}, {}, {})", a[0], a[1], line),
            "Int.bit_and" => format!("({} & {})", a[0], a[1]),
            "Int.bit_or" => format!("({} | {})", a[0], a[1]),
            "Int.bit_xor" => format!("({} ^ {})", a[0], a[1]),
            "Int.shift_left" => format!("((int64_t)((uint64_t){} << ({} & 63)))", a[0], a[1]),
            "Int.shift_right" => format!("((int64_t)((uint64_t){} >> ({} & 63)))", a[0], a[1]),
            "Float.round" => format!("lt_f2i(round({}), {})", a[0], line),
            "Float.floor" => format!("lt_f2i(floor({}), {})", a[0], line),
            "Float.ceil" => format!("lt_f2i(ceil({}), {})", a[0], line),
            "Float.to_text" => format!("lt_float_to_text({})", a[0]),
            "Float.abs" => format!("fabs({})", a[0]),
            "Float.pow" => format!("pow({}, {})", a[0], a[1]),
            "Float.sqrt" => format!("sqrt({})", a[0]),
            "Bool.to_text" => format!("lt_bool_to_text({})", a[0]),
            "Text.length" => format!("lt_text_length({})", a[0]),
            "Text.byte_length" => format!("({}->len)", a[0]),
            "Text.lower" => format!("lt_text_lower({})", a[0]),
            "Text.upper" => format!("lt_text_upper({})", a[0]),
            "Text.trim" => format!("lt_text_trim({})", a[0]),
            "Text.split" => format!("lt_text_split({}, {})", a[0], a[1]),
            "Text.lines" => format!("lt_text_lines({})", a[0]),
            "Text.words" => format!("lt_text_words({})", a[0]),
            "Text.chars" => format!("lt_text_chars({})", a[0]),
            "Text.contains" => format!("lt_text_contains({}, {})", a[0], a[1]),
            "Text.starts_with" => format!("lt_text_starts_with({}, {})", a[0], a[1]),
            "Text.ends_with" => format!("lt_text_ends_with({}, {})", a[0], a[1]),
            "Text.replace" => format!("lt_text_replace({}, {}, {})", a[0], a[1], a[2]),
            "Text.slice" => format!("lt_text_slice({}, {}, {})", a[0], a[1], a[2]),
            "Text.repeat" => format!("lt_text_repeat({}, {}, {})", a[0], a[1], line),
            "Text.pad_start" => format!("lt_text_pad({}, {}, {}, true)", a[0], a[1], a[2]),
            "Text.pad_end" => format!("lt_text_pad({}, {}, {}, false)", a[0], a[1], a[2]),
            "Text.to_int" => format!("lt_text_to_int({}, {})", a[0], a[1]),
            "Text.to_float" => format!("lt_text_to_float({}, {})", a[0], a[1]),
            "Text.concat" => {
                if a.len() == 1 {
                    format!("lt_text_ret({})", a[0])
                } else {
                    format!("lt_text_concat_n({}, (lt_text*[]){{ {} }})", a.len(), a.join(", "))
                }
            }
            "to_text" | "debug_text" => {
                let t = tid0.unwrap();
                self.totext_expr(t, &a[0], name == "debug_text")
            }
            "print" => format!("lt_print({})", a[0]),
            "assert" => format!("lt_assert({}, {})", a[0], line),
            "panic" => format!("lt_panic_text({}, {})", a[0], line),
            "min" | "max" => {
                let t = tid0.unwrap();
                let c = self.cmp_expr(t, "a_", "b_");
                let ct = self.tys[t].c.clone();
                let pick = if name == "min" { "<=" } else { ">=" };
                let d = self.dup(t, "r_");
                format!("({{ {ct} a_ = {x}, b_ = {y}; {ct} r_ = ({c} {pick} 0) ? a_ : b_; {d} r_; }})", ct = ct, x = a[0], y = a[1], c = c, pick = pick, d = d)
            }
            "__less" => {
                let t = tid0.unwrap();
                let c = self.cmp_expr(t, &a[0], &a[1]);
                format!("({} < 0)", c)
            }
            "__list_with_capacity" => {
                let lt = Ty::Adt(self.prog.b.list, vec![tys[0].clone()]);
                let l = self.tid(&lt);
                self.need(H::Ops, l);
                format!("{}_new({})", self.tys[l].c, a[0])
            }
            "expect_failed" => {
                if a.len() > 2 {
                    format!("lt_expect_failed({}->data, {}, {}, {})", a[0], a[1], a[2], a[3])
                } else {
                    format!("lt_expect_failed({}->data, {}, NULL, NULL)", a[0], a[1])
                }
            }
            "expect_throws_failed" => format!("lt_expect_throws_failed({}->data, {})", a[0], a[1]),
            "iface_upcast" => format!("({}); lt_panic_at(\"converting between interface combinations is not supported yet\", {})", a[0], line),
            // lists
            "List.length" => format!("({}->len)", a[0]),
            "List.get" => {
                ops_of(self);
                format!("{}_get({}, {}, {})", lc, a[0], a[1], line)
            }
            "List.get_unchecked" => {
                ops_of(self);
                format!("{}_get_unchecked({}, {})", lc, a[0], a[1])
            }
            "List.take" => {
                ops_of(self);
                format!("{}_slice({}, 0, {})", lc, a[0], a[1])
            }
            "List.drop" => {
                ops_of(self);
                format!("{}_slice({}, {}, INT64_MAX)", lc, a[0], a[1])
            }
            "List.sum" => {
                ops_of(self);
                format!("{}_sum({}, {})", lc, a[0], line)
            }
            "List.min" | "List.max" => {
                ops_of(self);
                format!("{}_minmax({}, {})", lc, a[0], if name == "List.min" { 1 } else { -1 })
            }
            "List.join" => format!("lt_text_join({}, {})", a[0], a[1]),
            "List.count_each" => {
                let e = match &self.tys[tid0.unwrap()].kind {
                    Kind::List(e) => *e,
                    _ => unreachable!(),
                };
                let ety = self.ty_of(e);
                let mt = Ty::Adt(self.prog.b.map, vec![ety, Ty::Int]);
                let m = self.tid(&mt);
                self.need(H::Ops, m);
                let mc = self.tys[m].c.clone();
                format!("({{ {l} l_ = {x}; {m} m_ = {m}_new(16); for (int64_t i_ = 0; i_ < l_->len; i_++) {{ int64_t *s_ = {m}_slot_insert(&m_, l_->items[i_]); *s_ += 1; }} m_; }})", l = lc, x = a[0], m = mc)
            }
            // maps
            "Map.length" | "Set.length" => format!("({}->len)", a[0]),
            "Map.get" => {
                ops_of(self);
                let (k, v) = match &self.tys[tid0.unwrap()].kind {
                    Kind::Map(k, v) => (*k, *v),
                    _ => unreachable!(),
                };
                let _ = k;
                let vty = self.ty_of(v);
                let opt = self.opt_tid_of(&vty);
                let dupv = self.dup(v, "v_");
                let some = self.opt_some(opt, "v_");
                let none = self.opt_none(opt);
                let vc = self.tys[v].c.clone();
                format!("({{ int64_t i_ = {m}_find({x}, {k}, {m}_hash({k})); {oc} r_; if (i_ >= 0) {{ {vc} v_ = {x}->e[i_].v; {dupv} r_ = {some}; }} else r_ = {none}; r_; }})", m = lc, x = a[0], k = a[1], oc = self.tys[opt].c, vc = vc, dupv = dupv, some = some, none = none)
            }
            "Map.contains_key" | "Set.contains" => {
                ops_of(self);
                format!("{}_contains({}, {})", lc, a[0], a[1])
            }
            "Map.keys" | "Map.values" | "Map.entries" | "Set.to_list" => {
                ops_of(self);
                let (k, v) = match &self.tys[tid0.unwrap()].kind {
                    Kind::Map(k, v) => (*k, Some(*v)),
                    Kind::Set(k) => (*k, None),
                    _ => unreachable!(),
                };
                let (elem_ty, build): (Ty, String) = match name {
                    "Map.keys" | "Set.to_list" => {
                        let kt = self.ty_of(k);
                        let d = self.dup(k, "x_");
                        (kt, format!("x_ = m_->e[i_].k; {}", d))
                    }
                    "Map.values" => {
                        let vt = self.ty_of(v.unwrap());
                        let d = self.dup(v.unwrap(), "x_");
                        (vt, format!("x_ = m_->e[i_].v; {}", d))
                    }
                    _ => {
                        let kt = self.ty_of(k);
                        let vt = self.ty_of(v.unwrap());
                        let et = Ty::Adt(self.prog.b.entry, vec![kt, vt]);
                        let eid = self.tid(&et);
                        let dk = self.dup(k, "x_.f0");
                        let dv = self.dup(v.unwrap(), "x_.f1");
                        let ec = self.tys[eid].c.clone();
                        (et, format!("x_ = ({}){{ m_->e[i_].k, m_->e[i_].v }}; {} {}", ec, dk, dv))
                    }
                };
                let lt = Ty::Adt(self.prog.b.list, vec![elem_ty.clone()]);
                let l = self.tid(&lt);
                self.need(H::Ops, l);
                let lcn = self.tys[l].c.clone();
                let ec = self.cty(&elem_ty);
                format!("({{ {m} m_ = {x}; {l} r_ = {l}_new(m_->len); for (int64_t i_ = 0; i_ < m_->n; i_++) {{ if (!m_->e[i_].h) continue; {ec} x_; {b} r_->items[r_->len++] = x_; }} r_; }})", m = lc, x = a[0], l = lcn, ec = ec, b = build)
            }
            _ => {
                let _ = (fi, args);
                panic!("unknown intrinsic {}", name)
            }
        }
    }

    fn mut_intrinsic(&mut self, fi: usize, name: &str, tys: &[Ty], p: &str, a: &[String], args: &[Op], throws: bool) -> String {
        let line = self.line();
        let tid0 = self.tid(&tys[0]);
        self.need(H::Ops, tid0);
        let lc = self.tys[tid0].c.clone();
        match name {
            "List.append" => format!("{}_push({}, {})", lc, p, a[0]),
            "List.append_all" => format!("{}_append_all({}, {})", lc, p, a[0]),
            "List.insert" => format!("{}_insert({}, {}, {}, {})", lc, p, a[0], a[1], line),
            "List.remove_at" => format!("{}_remove_at({}, {}, {})", lc, p, a[0], line),
            "List.pop" => format!("{}_pop({})", lc, p),
            "List.sort" => format!("{}_sort({})", lc, p),
            "List.sort_by" => {
                let fty = self.op_ty(fi, &args[0]);
                let ft = match &fty {
                    Ty::Func(ft) => (**ft).clone(),
                    _ => unreachable!(),
                };
                let k = self.tid(&ft.ret);
                self.need(H::SortBy(k, ft.throws), tid0);
                let _ = throws;
                format!("{}_sort_by_{}{}({}, {})", lc, k, if ft.throws { "t" } else { "" }, p, a[0])
            }
            "List.reverse" => format!("{}_reverse({})", lc, p),
            "List.clear" | "Map.clear" | "Set.clear" => format!("{}_clear({})", lc, p),
            "Map.remove" | "Set.remove" => format!("{}_remove({}, {})", lc, p, a[0]),
            "Set.add" => {
                let e = match &self.tys[tid0].kind {
                    Kind::Set(e) => *e,
                    _ => unreachable!(),
                };
                let d = self.drop_(e, "k_");
                format!("({{ {kc} k_ = {k}; *{m}_slot_insert({p}, k_) = true; {d} }})", kc = self.tys[e].c, k = a[0], m = lc, p = p, d = d)
            }
            "Map.take" => {
                let (_k, v) = match &self.tys[tid0].kind {
                    Kind::Map(k, v) => (*k, *v),
                    _ => unreachable!(),
                };
                let vty = self.ty_of(v);
                let opt = self.opt_tid_of(&vty);
                let some = self.opt_some(opt, "v_");
                let none = self.opt_none(opt);
                let drop_k = {
                    let k = _k;
                    self.drop_(k, "(*p_)->e[i_].k")
                };
                format!("({{ {m} *p_ = {p}; {oc} r_ = {none}; if ({m}_find(*p_, {k}, {m}_hash({k})) >= 0) {{ {m}_unique(p_); int64_t i_ = {m}_find(*p_, {k}, {m}_hash({k})); {vc} v_ = (*p_)->e[i_].v; {dk} (*p_)->e[i_].h = 0; (*p_)->len--; r_ = {some}; }} r_; }})", m = lc, p = p, oc = self.tys[opt].c, none = none, k = a[0], vc = self.tys[v].c, dk = drop_k, some = some)
            }
            _ => panic!("unknown mutating intrinsic {}", name),
        }
    }

    // ================= interfaces and closures =================

    fn gen_vtables(&mut self, out: &mut String) {
        for (vi, vt) in self.m.vtables.iter().enumerate() {
            let tid = self.tid(&vt.concrete);
            let c = self.tys[tid].c.clone();
            let drop = self.drop_(tid, "b->v");
            let dup = self.dup(tid, "n->v");
            let eq = self.eq_expr(tid, "((B{t}*)a)->v", "((B{t}*)b)->v").replace("{t}", &tid.to_string());
            let hash = self.hash_expr(tid, "((B{t}*)a)->v").replace("{t}", &tid.to_string());
            let tt = self.totext_expr(tid, "((B{t}*)a)->v", false).replace("{t}", &tid.to_string());
            let ttd = self.totext_expr(tid, "((B{t}*)a)->v", true).replace("{t}", &tid.to_string());
            if !self.done.contains(&(H::Ops, usize::MAX - tid)) {
                self.done.insert((H::Ops, usize::MAX - tid));
                let _ = writeln!(out, "typedef struct {{ int64_t rc; {c} v; }} B{t};", c = c, t = tid);
                let _ = writeln!(out, "static void B{t}_drop(lt_obj *o) {{ B{t} *b = (B{t} *)o; {drop} lt_free(b, sizeof(B{t})); }}", t = tid, drop = drop);
                let _ = writeln!(out, "static bool B{t}_eq(lt_obj *a, lt_obj *b) {{ return {eq}; }}", t = tid, eq = eq);
                let _ = writeln!(out, "static uint64_t B{t}_hash(lt_obj *a) {{ return {h}; }}", t = tid, h = hash);
                let _ = writeln!(out, "static lt_text *B{t}_totext(lt_obj *a, bool debug) {{ return debug ? {d} : {n}; }}", t = tid, d = ttd, n = tt);
                let _ = writeln!(out, "static lt_obj *B{t}_clone(lt_obj *o) {{ B{t} *n = (B{t} *)lt_alloc(sizeof(B{t})); n->rc = 1; n->v = ((B{t} *)o)->v; {dup} return (lt_obj *)n; }}", t = tid, dup = dup);
            }
            // method thunks
            let mut slots = vec![];
            let ids = vt.iface.clone();
            let mids = iface_methods(self.prog, &ids);
            for (si, fidx) in vt.methods.iter().enumerate() {
                let mf = self.prog.fns[mids[si]].clone();
                let target = &self.m.funcs[*fidx];
                let tname = target.name.clone();
                let mutating = target.mutating;
                let throws = target.throws;
                let mut ps = vec!["lt_obj *o".to_string()];
                let mut call_args = vec![];
                if mutating {
                    call_args.push(format!("&((B{} *)o)->v", tid));
                } else {
                    call_args.push("self_".to_string());
                }
                for (pi, (_, pty)) in mf.params.iter().enumerate() {
                    if self.is_unit(pty) {
                        continue;
                    }
                    let pc = self.cty(pty);
                    ps.push(format!("{} a{}", pc, pi));
                    call_args.push(format!("a{}", pi));
                }
                let ret_unit = self.is_unit(&mf.ret);
                let rc = if ret_unit { "void".to_string() } else { self.cty(&mf.ret) };
                // the interface method's convention decides the thunk's signature
                let iface_throws = mf.throws;
                let (ret_t, tail) = if iface_throws {
                    if !ret_unit {
                        ps.push(format!("{} *out_", rc));
                    }
                    ("lt_err".to_string(), if throws { if !ret_unit { call_args.push("out_".into()); } "return".to_string() } else if ret_unit { "".to_string() } else { "*out_ =".to_string() })
                } else {
                    (rc.clone(), if ret_unit { "".to_string() } else { "return".to_string() })
                };
                let pre = if mutating { String::new() } else { format!("{c} self_ = ((B{t} *)o)->v; {d}", c = c, t = tid, d = self.dup(tid, "self_")) };
                let call = format!("{}({})", tname, call_args.join(", "));
                let body = if iface_throws && !throws {
                    format!("{} {} {}; return (lt_err){{0}};", pre, tail, call)
                } else {
                    format!("{} {} {};", pre, tail, call)
                };
                let _ = writeln!(out, "static {} vt{}_m{}({}) {{ {} }}", ret_t, vi, si, ps.join(", "), body);
                slots.push(format!("(void *)vt{}_m{}", vi, si));
            }
            let _ = writeln!(
                out,
                "static struct {{ lt_vt h; void *m[{n}]; }} vt{vi} = {{ {{ B{t}_drop, B{t}_eq, B{t}_hash, B{t}_totext, B{t}_clone, {t} }}, {{ {slots} }} }};",
                n = slots.len().max(1),
                vi = vi,
                t = tid,
                slots = if slots.is_empty() { "0".to_string() } else { slots.join(", ") }
            );
            let _ = writeln!(out, "static lt_iface to_iface_vt{vi}({c} v) {{ B{t} *b = (B{t} *)lt_alloc(sizeof(B{t})); b->rc = 1; b->v = v; return (lt_iface){{ (lt_obj *)b, &vt{vi}.h }}; }}", vi = vi, c = c, t = tid);
            let _ = writeln!(self.protos, "static lt_iface to_iface_vt{}({} v);", vi, c);
        }
    }

    fn gen_envs(&mut self, out: &mut String) {
        for fi in 0..self.m.funcs.len() {
            if let FnKind::Closure(caps) = &self.m.funcs[fi].kind {
                if caps.is_empty() {
                    continue;
                }
                let caps = caps.clone();
                let name = self.m.funcs[fi].name.clone();
                let mut fields = String::new();
                let mut drops = String::new();
                for (i, t) in caps.iter().enumerate() {
                    let tid = self.tid(t);
                    let _ = write!(fields, " {} c{};", self.tys[tid].c, i);
                    drops += &self.drop_(tid, &format!("x->c{}", i));
                }
                let _ = writeln!(out, "typedef struct {{ lt_env h;{} }} Env_{};", fields, name);
                let _ = writeln!(out, "static void Env_{n}_drop(lt_env *e) {{ Env_{n} *x = (Env_{n} *)e; {d} lt_free(x, sizeof(Env_{n})); }}", n = name, d = drops);
            }
        }
    }

    // ================= whole program =================

    pub fn generate(mut self, tests: bool) -> String {
        let mut fns = String::new();
        for fi in 0..self.m.funcs.len() {
            self.gen_function(fi, &mut fns);
        }
        let mut vt_out = String::new();
        self.gen_vtables(&mut vt_out);
        let mut env_out = String::new();
        self.gen_envs(&mut env_out);
        // lt_make_failure
        let fv = self.m.failure_vtable;
        let failure_ty = Ty::Adt(self.prog.b.failure, vec![]);
        let ftid = self.tid(&failure_ty);
        let fc = self.tys[ftid].c.clone();
        let make_failure = format!("static lt_err lt_make_failure(lt_text *msg) {{ return to_iface_vt{}(({}){{ msg, (lt_iface){{0}} }}); }}\n", fv, fc);
        // error message: Error.message is slot 0 of the Error interface
        let err_msg = "static lt_text *lt_error_message(lt_err e) { return ((lt_text *(*)(lt_obj *))e.vt->m[0])(e.obj); }\n\
static void lt_panic_error(lt_err e, int line) { lt_text *m = lt_error_message(e); lt_panic_text(m, line); }\n";
        let texts = self.tid(&Ty::Adt(self.prog.b.list, vec![Ty::Text]));
        self.need(H::Ops, texts);
        self.gen_helpers();
        // entry point
        let mut main = String::new();
        if tests {
            main += "int main(void) {\n  lt_init();\n  int failed = 0, passed = 0;\n";
            for (i, t) in self.m.tests.iter().enumerate() {
                let f = &self.m.funcs[*t];
                let name = match &f.kind {
                    FnKind::Test(n) => n.clone(),
                    _ => String::new(),
                };
                let _ = writeln!(main, "  {{ lt_err e = {}(); if (e.obj) {{ failed++; lt_text *m = lt_error_message(e); printf(\"FAIL  %s\\n      %.*s\\n\", {}, (int)m->len, m->data); lt_text_drop(m); lt_iface_drop(e); }} else {{ passed++; printf(\"ok    %s\\n\", {}); }} }}", f.name, c_str(&name), c_str(&name));
                let _ = i;
            }
            main += "  printf(\"\\n%d passed, %d failed\\n\", passed, failed);\n  fflush(stdout);\n  return failed ? 1 : 0;\n}\n";
        } else if let Some(m) = self.m.main {
            let f = &self.m.funcs[m];
            main += "int main(void) {\n  lt_init();\n";
            if f.throws {
                let _ = writeln!(main, "  lt_err e = {}();\n  if (e.obj) {{ fflush(stdout); lt_text *m = lt_error_message(e); fprintf(stderr, \"error: %.*s\\n\", (int)m->len, m->data); lt_text_drop(m); lt_iface_drop(e); return 1; }}", f.name);
            } else {
                let _ = writeln!(main, "  {}();", f.name);
            }
            main += "  fflush(stdout);\n  return 0;\n}\n";
        } else {
            main += "int main(void) { fprintf(stderr, \"this program has no `fn main()`\\n\"); return 1; }\n";
        }
        let types = self.emit_types();
        let mut out = String::new();
        out += include_str!("runtime/rt.h");
        out += "\n// ---- generated ----\n";
        out += "static void lt_index_panic(int64_t i, int64_t n, int line) { char b[128]; snprintf(b, sizeof b, \"index %lld is out of range for a list of length %lld\", (long long)i, (long long)n); lt_panic_at(b, line); }\n";
        out += "static void lt_panic_error(lt_err e, int line);\n";
        out += &types;
        out += &self.helper_types;
        out += &self.lit_defs;
        out += &self.protos;
        out += &extract_protos(&self.helpers);
        out += &extract_protos(&vt_out);
        out += &extract_protos(&env_out);
        out += &env_out;
        out += &vt_out;
        out += &make_failure;
        out += err_msg;
        out += &self.helpers;
        out += &fns;
        let _ = writeln!(out, "static const char *lt_file_init = {};", c_str(&self.file));
        out += &main.replacen("lt_init();", "lt_init(); lt_file = lt_file_init;", 1);
        out
    }
}

// Prototypes for every function defined in generated helper code, so that
// helpers can call each other in any order.
fn extract_protos(code: &str) -> String {
    let mut out = String::new();
    for line in code.lines() {
        if !line.starts_with("static ") || line.starts_with("static struct") || line.contains(" = ") && !line.contains('(') {
            continue;
        }
        if let Some(i) = line.find(" {") {
            let sig = &line[..i];
            if sig.contains('(') {
                out.push_str(sig);
                out.push_str(";\n");
            }
        }
    }
    out
}
