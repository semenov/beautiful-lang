// Type checking: ast::Module -> types::Program.
//
// Declarations are collected first (types, then signatures), then every
// function body is checked into the typed tree. Inference is local:
// bidirectional checking plus unification variables inside one body.

use crate::ast::{self, BinOp, Expr, ExprKind, Pattern, Stmt, TypeExpr, UnOp};
use crate::diag::{Diag, Span};
use crate::types::*;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug)]
enum Global {
    Fn(FnId),
    Type(DefId),
}

struct LambdaCtx {
    level: usize,
    captures: Vec<LocalId>,
    throws: bool,
    // `None` while the lambda's return type is still unknown
    ret: Ty,
}

struct FnCtx {
    locals: Vec<LocalDef>,
    level: Vec<usize>, // lambda nesting level where each local was declared
    scopes: Vec<HashMap<String, LocalId>>,
    generics: Vec<String>,
    vars: Vec<Option<Ty>>,
    ret: Ty,
    throws_ok: bool,
    uses_throw: bool,
    in_test: bool,
    lambdas: Vec<LambdaCtx>,
    loops: Vec<usize>, // lambda level of each enclosing loop
    self_local: Option<LocalId>,
    self_mutable: bool,
    // set by `try` / `expect throws` for the call right below it
    in_try: bool,
    // the expression is a statement: `if`/`match` produce no value
    stmt_pos: bool,
    // inside the expression after `try`: whether a call in it can fail
    try_hits: Vec<bool>,
    // set by `spawn` for the call right below it
    in_spawn: bool,
    spawn_call: bool,
    // the value of a `with`: may create a resource
    with_value: bool,
    resource_ok: bool,
    lock_depth: usize,
    module: usize,
}

pub struct ModScope {
    pub name: String,
    globals: HashMap<String, Global>,
    // imported module names -> module index
    imports: HashMap<String, usize>,
    // the prelude and the standard library: may declare runtime functions
    privileged: bool,
}

pub struct Checker {
    pub prog: Program,
    pub diags: Vec<Diag>,
    // the files' text, for literals whose spelling matters (1.50 as a Decimal)
    pub source_texts: Vec<String>,
    modules: Vec<ModScope>,
    prim: HashMap<DefId, Ty>,
    f: Option<FnCtx>,
    // AST of every function (methods included) by FnId, for body checking
    fn_asts: Vec<Option<(ast::FnDecl, usize)>>,
    // top-level `let`s: 1 while being checked (to find cycles), 2 when done
    const_state: HashMap<FnId, u8>,
}

fn orderable(t: &Ty) -> bool {
    matches!(t, Ty::Int | Ty::Float | Ty::Text | Ty::Bool)
}

fn levenshtein(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    for i in 1..=a.len() {
        let mut cur = vec![i; b.len() + 1];
        for j in 1..=b.len() {
            let c = if a[i - 1] == b[j - 1] { 0 } else { 1 };
            cur[j] = (prev[j] + 1).min(cur[j - 1] + 1).min(prev[j - 1] + c);
        }
        prev = cur;
    }
    prev[b.len()]
}

fn synonym(name: &str) -> Option<&'static str> {
    Some(match name {
        "size" | "len" | "count_of" => "length",
        "push" | "add_item" | "push_back" => "append",
        "skip" => "drop",
        "toString" | "to_text" | "str" => "to_string",
        "reduce" | "inject" => "fold",
        "includes" | "has" => "contains",
        "extend" => "append_all",
        "get_or" | "unwrap_or" => "??",
        "toLowerCase" | "lowercase" => "lower",
        "toUpperCase" | "uppercase" => "upper",
        "strip" => "trim",
        "startsWith" => "starts_with",
        "endsWith" => "ends_with",
        "filter_map" => "map + filter",
        "reverse_sorted" => "sorted().reversed()",
        _ => return None,
    })
}

pub fn suggest<'a>(name: &str, candidates: impl Iterator<Item = &'a String>) -> Option<String> {
    if let Some(s) = synonym(name) {
        return Some(s.to_string());
    }
    let mut best: Option<(usize, &String)> = None;
    for c in candidates {
        let d = levenshtein(name, c);
        if d <= 2 && d < name.len() && best.map(|(bd, _)| d < bd).unwrap_or(true) {
            best = Some((d, c));
        }
    }
    best.map(|(_, c)| c.clone())
}

impl Checker {
    pub fn new() -> Checker {
        Checker {
            prog: Program {
                module_names: vec![],
                defs: vec![],
                fns: vec![],
                tests: vec![],
                b: Builtins { int: 0, float: 0, bool_: 0, text: 0, list: 0, map: 0, set: 0, entry: 0, indexed: 0, error: 0, failure: 0, task: 0, shared: 0, locked: 0, channel: 0, cancelled: 0, channel_closed: 0, bytes: 0, decimal: 0 },
                main: None,
            },
            diags: vec![],
            source_texts: vec![],
            modules: vec![],
            prim: HashMap::new(),
            f: None,
            fn_asts: vec![],
            const_state: HashMap::new(),
        }
    }

    fn err(&mut self, span: Span, msg: impl Into<String>) {
        self.diags.push(Diag::new(span, msg));
    }
    fn err_help(&mut self, span: Span, msg: impl Into<String>, help: impl Into<String>) {
        self.diags.push(Diag::new(span, msg).help(help));
    }
    fn show(&self, t: &Ty) -> String {
        let t = self.resolve(t);
        self.prog.show(&t)
    }

    fn lookup_global(&self, name: &str, md: usize) -> Option<Global> {
        if let Some(g) = self.modules[md].globals.get(name) {
            return Some(*g);
        }
        self.modules[0].globals.get(name).copied()
    }

    // `files` in `files.read(...)`: an imported module, unless a local has that name
    fn module_named(&self, name: &str) -> Option<usize> {
        let md = self.f.as_ref().map(|f| f.module)?;
        if self.f.as_ref().unwrap().scopes.iter().any(|s| s.contains_key(name)) {
            return None;
        }
        self.modules[md].imports.get(name).copied()
    }

    // A name from another module: it must be `pub`.
    fn lookup_in_module(&mut self, target: usize, name: &str, span: Span) -> Option<Global> {
        let g = self.modules[target].globals.get(name).copied();
        let mname = self.modules[target].name.clone();
        match g {
            None => {
                let cands: Vec<String> = self.modules[target].globals.keys().cloned().collect();
                match suggest(name, cands.iter()) {
                    Some(s) => self.err_help(span, format!("module `{}` has no `{}`", mname, name), format!("did you mean `{}.{}`?", mname, s)),
                    None => self.err(span, format!("module `{}` has no `{}`", mname, name)),
                }
                None
            }
            Some(g) => {
                let public = match g {
                    Global::Fn(f) => self.prog.fns[f].is_pub,
                    Global::Type(d) => self.prog.defs[d].is_pub,
                };
                if !public {
                    self.err(span, format!("`{}.{}` is private to its module", mname, name));
                }
                Some(g)
            }
        }
    }

    // ================= declaration collection =================

    // `modules[0]` is the prelude, the last one is the user's file; the rest
    // are standard library modules. Each comes with the names it imports.
    pub fn check_program(&mut self, mods: &[(String, ast::Module, bool)]) {
        for (name, m, privileged) in mods {
            let _ = m;
            self.prog.module_names.push(name.clone());
            self.modules.push(ModScope { name: name.clone(), globals: HashMap::new(), imports: HashMap::new(), privileged: *privileged });
        }
        for (i, (_, m, _)) in mods.iter().enumerate() {
            for imp in &m.imports {
                let local = imp.local_name().to_string();
                match mods.iter().position(|x| x.0 == imp.key) {
                    Some(j) if j != 0 => {
                        if self.modules[i].imports.insert(local.clone(), j).is_some() {
                            self.err_help(imp.span, format!("two imports are named `{}`", local), format!("rename one: `import {} as other_name`", imp.path.join(".")));
                        }
                    }
                    _ => self.err(imp.span, format!("unknown module `{}`", imp.path.join("."))),
                }
            }
        }
        for (i, (_, m, _)) in mods.iter().enumerate() {
            self.collect_types(m, i);
        }
        let pl = |c: &Checker, n: &str| match c.modules[0].globals.get(n) {
            Some(Global::Type(d)) => *d,
            _ => panic!("prelude type {} missing", n),
        };
        self.prog.b = Builtins {
            int: pl(self, "Int"),
            float: pl(self, "Float"),
            bool_: pl(self, "Bool"),
            text: pl(self, "String"),
            list: pl(self, "List"),
            map: pl(self, "Map"),
            set: pl(self, "Set"),
            entry: pl(self, "Entry"),
            indexed: pl(self, "Indexed"),
            error: pl(self, "Error"),
            failure: pl(self, "Failure"),
            task: pl(self, "Task"),
            shared: pl(self, "Shared"),
            locked: pl(self, "Locked"),
            channel: pl(self, "Channel"),
            cancelled: pl(self, "Cancelled"),
            channel_closed: pl(self, "ChannelClosed"),
            bytes: pl(self, "Bytes"),
            decimal: pl(self, "Decimal"),
        };
        let b = &self.prog.b;
        self.prim.insert(b.int, Ty::Int);
        self.prim.insert(b.float, Ty::Float);
        self.prim.insert(b.bool_, Ty::Bool);
        self.prim.insert(b.text, Ty::Text);
        for (i, (_, m, _)) in mods.iter().enumerate() {
            self.resolve_type_bodies(m, i);
        }
        for (i, (_, m, _)) in mods.iter().enumerate() {
            self.collect_fns(m, i);
        }
        for (i, (_, m, _)) in mods.iter().enumerate() {
            self.check_implements(m, i);
        }
        self.check_task_escapes();
        for (i, (_, m, _)) in mods.iter().enumerate() {
            self.check_defaults(m, i);
        }
        // bodies
        for id in 0..self.fn_asts.len() {
            if let Some((decl, md)) = self.fn_asts[id].clone() {
                if decl.is_const {
                    self.check_const(id);
                } else if decl.body.is_some() {
                    self.check_fn_body(id, &decl, md);
                }
            }
        }
        let user = mods.len() - 1;
        for item in &mods[user].1.items {
            if let ast::Item::Test(t) = item {
                self.check_test(t, user);
            }
        }
        if let Some(Global::Fn(id)) = self.modules[user].globals.get("main").copied() {
            let f = &self.prog.fns[id];
            if !f.params.is_empty() || f.ret != Ty::Unit {
                let sp = f.span;
                self.err(sp, "`main` takes no parameters and returns nothing");
            }
            self.prog.main = Some(id);
        }
    }

    fn add_def(&mut self, name: &str, generics: Vec<String>, span: Span, md: usize, is_pub: bool) -> DefId {
        self.prog.defs.push(TypeDef {
            name: name.to_string(),
            generics,
            kind: TypeKind::Builtin,
            methods: HashMap::new(),
            props: vec![],
            implements: vec![],
            span,
            is_prelude: md == 0,
            module: md,
            is_pub: is_pub || md == 0,
        });
        let id = self.prog.defs.len() - 1;
        let map = &mut self.modules[md].globals;
        if map.insert(name.to_string(), Global::Type(id)).is_some() {
            self.err(span, format!("`{}` is declared twice", name));
        }
        id
    }

    fn collect_types(&mut self, astm: &ast::Module, md: usize) {
        for item in &astm.items {
            match item {
                ast::Item::Record(r) => {
                    self.add_def(&r.name, r.generics.clone(), r.span, md, r.is_pub);
                }
                ast::Item::Enum(e) => {
                    self.add_def(&e.name, e.generics.clone(), e.span, md, e.is_pub);
                }
                ast::Item::Newtype(n) => {
                    self.add_def(&n.name, vec![], n.span, md, n.is_pub);
                }
                ast::Item::Interface(i) => {
                    self.add_def(&i.name, vec![], i.span, md, i.is_pub);
                }
                _ => {}
            }
        }
    }

    fn def_id(&self, name: &str, md: usize) -> DefId {
        match self.lookup_global(name, md) {
            Some(Global::Type(d)) => d,
            _ => unreachable!(),
        }
    }

    pub fn resolve_texpr(&mut self, t: &TypeExpr, generics: &[String], md: usize) -> Ty {
        match t {
            TypeExpr::Optional(inner, sp) => {
                let i = self.resolve_texpr(inner, generics, md);
                if matches!(i, Ty::Opt(_)) {
                    self.err(*sp, "`T??` is not a type: a value is either missing or not");
                }
                Ty::opt(i)
            }
            TypeExpr::Func { params, ret, throws, .. } => {
                let ps = params.iter().map(|p| self.resolve_texpr(p, generics, md)).collect();
                let r = match ret {
                    Some(r) => self.resolve_texpr(r, generics, md),
                    None => Ty::Unit,
                };
                Ty::func(ps, r, *throws)
            }
            TypeExpr::Combo(parts, sp) => {
                let mut ids = vec![];
                for p in parts {
                    match self.resolve_texpr(p, generics, md) {
                        Ty::Iface(v) => ids.extend(v),
                        Ty::Err => return Ty::Err,
                        other => {
                            let s = self.show(&other);
                            self.err(*sp, format!("`A + B` combines interfaces, but `{}` is not an interface", s));
                            return Ty::Err;
                        }
                    }
                }
                ids.sort();
                ids.dedup();
                Ty::Iface(ids)
            }
            TypeExpr::Named { path, args, span } => {
                if path.len() > 2 {
                    self.err(*span, format!("unknown type `{}`", path.join(".")));
                    return Ty::Err;
                }
                // `http.Request`: a type from an imported module
                let (md, name) = if path.len() == 2 {
                    match self.modules[md].imports.get(&path[0]).copied() {
                        Some(target) => match self.lookup_in_module(target, &path[1], *span) {
                            Some(Global::Type(_)) => (target, &path[1]),
                            Some(_) => {
                                self.err(*span, format!("`{}` is not a type", path.join(".")));
                                return Ty::Err;
                            }
                            None => return Ty::Err,
                        },
                        None => {
                            self.err_help(*span, format!("unknown module `{}`", path[0]), format!("add `import {}` at the top of the file", path[0]));
                            return Ty::Err;
                        }
                    }
                } else {
                    (md, &path[0])
                };
                if let Some(i) = generics.iter().position(|g| g == name) {
                    if !args.is_empty() {
                        self.err(*span, "a type parameter takes no type arguments");
                    }
                    return Ty::Param(i as u32);
                }
                let targs: Vec<Ty> = args.iter().map(|a| self.resolve_texpr(a, generics, md)).collect();
                match name.as_str() {
                    "Never" if self.modules[md].privileged => return Ty::Never,
                    _ => {}
                }
                match self.lookup_global(name, md) {
                    Some(Global::Type(d)) => {
                        if let Some(p) = self.prim.get(&d) {
                            return p.clone();
                        }
                        let def = &self.prog.defs[d];
                        if let TypeKind::Interface { .. } = def.kind {
                            return Ty::Iface(vec![d]);
                        }
                        // interfaces are resolved before their kind is set; check by AST later
                        if def.generics.len() != targs.len() {
                            let n = def.generics.len();
                            let dn = def.name.clone();
                            if n == 0 {
                                self.err(*span, format!("`{}` takes no type arguments", dn));
                            } else {
                                self.err(*span, format!("`{}` takes {} type argument(s): `{}<{}>`", dn, n, dn, def.generics.join(", ")));
                            }
                            return Ty::Err;
                        }
                        Ty::Adt(d, targs)
                    }
                    _ => {
                        let hint = match name.as_str() {
                            "Text" | "str" | "string" => Some("String"),
                            "int" | "i64" | "Integer" | "Long" => Some("Int"),
                            "float" | "Double" | "double" | "f64" | "Number" => Some("Float"),
                            "bool" | "Boolean" => Some("Bool"),
                            "Array" | "Vec" | "Vector" | "ArrayList" => Some("List"),
                            "Dict" | "HashMap" | "Dictionary" | "Record" => Some("Map"),
                            "HashSet" => Some("Set"),
                            "Option" | "Optional" | "Maybe" => Some("T?"),
                            "Result" => Some("`throws` on the function"),
                            "void" | "Void" | "Unit" | "None" => Some("no `->` at all"),
                            _ => None,
                        };
                        match hint {
                            Some(h) => self.err_help(*span, format!("unknown type `{}`", name), format!("use {}", h)),
                            None => self.err(*span, format!("unknown type `{}`", name)),
                        }
                        Ty::Err
                    }
                }
            }
        }
    }

    fn resolve_type_bodies(&mut self, astm: &ast::Module, md: usize) {
        // interfaces first so that fields can mention them
        for item in &astm.items {
            if let ast::Item::Interface(i) = item {
                let d = self.def_id(&i.name, md);
                self.prog.defs[d].kind = TypeKind::Interface { methods: vec![] };
            }
        }
        for item in &astm.items {
            match item {
                ast::Item::Record(r) => {
                    let d = self.def_id(&r.name, md);
                    let mut fields = vec![];
                    let mut props = vec![];
                    let mut seen = HashSet::new();
                    for f in &r.fields {
                        if !seen.insert(f.name.clone()) {
                            self.err(f.span, format!("field `{}` is declared twice", f.name));
                        }
                        let ty = self.resolve_texpr(&f.ty, &r.generics, md);
                        if r.builtin {
                            props.push((f.name.clone(), ty));
                        } else {
                            let doc = self.comment_above(f.span);
                            fields.push(FieldDef { name: f.name.clone(), ty, default: None, default_text: None, doc, span: f.span });
                        }
                    }
                    let def = &mut self.prog.defs[d];
                    def.props = props;
                    if !r.builtin {
                        def.kind = TypeKind::Record { fields };
                    }
                }
                ast::Item::Enum(e) => {
                    let d = self.def_id(&e.name, md);
                    let mut variants = vec![];
                    let mut seen = HashSet::new();
                    for v in &e.variants {
                        if !seen.insert(v.name.clone()) {
                            self.err(v.span, format!("variant `{}` is declared twice", v.name));
                        }
                        let fields = v
                            .fields
                            .iter()
                            .map(|f| FieldDef {
                                name: f.name.clone(),
                                ty: self.resolve_texpr(&f.ty, &e.generics, md),
                                default: None,
                                default_text: None,
                                doc: None,
                                span: f.span,
                            })
                            .collect();
                        variants.push(VariantDef { name: v.name.clone(), fields });
                    }
                    self.prog.defs[d].kind = TypeKind::Enum { variants };
                }
                ast::Item::Newtype(n) => {
                    let d = self.def_id(&n.name, md);
                    let ty = self.resolve_texpr(&n.ty, &[], md);
                    if let Ty::Adt(inner, _) = &ty {
                        if let TypeKind::Newtype(_) = self.prog.defs[*inner].kind {
                            self.err(n.span, "a new type can't wrap another new type");
                        }
                    }
                    self.prog.defs[d].kind = TypeKind::Newtype(ty);
                }
                _ => {}
            }
        }
    }

    fn new_fn(&mut self, decl: &ast::FnDecl, owner: Option<DefId>, owner_generics: &[String], md: usize) -> FnId {
        let mut generics: Vec<String> = owner_generics.to_vec();
        for g in &decl.generics {
            if generics.contains(g) {
                self.err(decl.span, format!("type parameter `{}` is already declared by the type", g));
            }
            generics.push(g.clone());
        }
        let mut params = vec![];
        let mut seen = HashSet::new();
        for p in &decl.params {
            if !seen.insert(p.name.clone()) {
                self.err(p.span, format!("parameter `{}` is declared twice", p.name));
            }
            params.push((p.name.clone(), self.resolve_texpr(&p.ty, &generics, md)));
        }
        let ret = match &decl.ret {
            // `-> Never`: the function never comes back (it exits, panics,
            // throws or loops forever); only as a return type
            Some(TypeExpr::Named { path, args, .. }) if path.len() == 1 && path[0] == "Never" && args.is_empty() && self.lookup_global("Never", md).is_none() => Ty::Never,
            Some(t) => self.resolve_texpr(t, &generics, md),
            // a `let` without a type: found from its value when it's checked
            None if decl.is_const => Ty::Err,
            None => Ty::Unit,
        };
        let self_mode = if decl.mutating {
            SelfMode::Mutating
        } else if decl.has_self {
            SelfMode::Value
        } else {
            SelfMode::None
        };
        if owner.is_some() && self_mode == SelfMode::None {
            self.err_help(decl.span, "a method takes `self` as its first parameter", "write `fn name(self, ...)`; a function that doesn't need `self` goes outside the type");
        }
        self.prog.fns.push(FnDef {
            name: decl.name.clone(),
            owner,
            generics,
            self_mode,
            params,
            ret,
            throws: decl.throws || decl.rethrows,
            rethrows: decl.rethrows,
            intrinsic: decl.intrinsic,
            body: None,
            span: decl.span,
            is_prelude: md == 0,
            module: md,
            // the prelude and the standard library's methods are its API
            is_pub: decl.is_pub || md == 0 || (owner.is_some() && self.modules[md].privileged),
            is_const: decl.is_const,
        });
        self.fn_asts.push(Some((decl.clone(), md)));
        self.prog.fns.len() - 1
    }

    fn add_methods(&mut self, d: DefId, methods: &[ast::FnDecl], generics: &[String], md: usize) {
        for m in methods {
            let id = self.new_fn(m, Some(d), generics, md);
            if self.prog.defs[d].methods.insert(m.name.clone(), id).is_some() {
                self.err(m.span, format!("method `{}` is declared twice (there is no overloading)", m.name));
            }
        }
    }

    fn collect_fns(&mut self, astm: &ast::Module, md: usize) {
        for item in &astm.items {
            match item {
                ast::Item::Fn(f) => {
                    let id = self.new_fn(f, None, &[], md);
                    let map = &mut self.modules[md].globals;
                    if map.insert(f.name.clone(), Global::Fn(id)).is_some() {
                        self.err(f.span, format!("`{}` is declared twice (there is no overloading)", f.name));
                    }
                    if f.body.is_none() && !self.modules[md].privileged {
                        self.err(f.span, "a function needs a body: `{ ... }`");
                    }
                }
                ast::Item::Record(r) => {
                    let d = self.def_id(&r.name, md);
                    self.add_methods(d, &r.methods, &r.generics, md);
                }
                ast::Item::Enum(e) => {
                    let d = self.def_id(&e.name, md);
                    self.add_methods(d, &e.methods, &e.generics, md);
                }
                ast::Item::Interface(i) => {
                    let d = self.def_id(&i.name, md);
                    let mut ids = vec![];
                    for im in &i.methods {
                        let id = self.new_fn(im, Some(d), &[], md);
                        // interface methods have no body to check
                        self.fn_asts[id] = None;
                        self.prog.defs[d].methods.insert(im.name.clone(), id);
                        ids.push(id);
                    }
                    self.prog.defs[d].kind = TypeKind::Interface { methods: ids };
                }
                _ => {}
            }
        }
    }

    fn check_implements(&mut self, astm: &ast::Module, md: usize) {
        for item in &astm.items {
            let (name, impls, generics) = match item {
                ast::Item::Record(r) => (&r.name, &r.implements, &r.generics),
                ast::Item::Enum(e) => (&e.name, &e.implements, &e.generics),
                _ => continue,
            };
            let d = self.def_id(name, md);
            for (iname, isp) in impls {
                let iface = match self.lookup_global(iname, md) {
                    Some(Global::Type(i)) if matches!(self.prog.defs[i].kind, TypeKind::Interface { .. }) => i,
                    _ => {
                        self.err(*isp, format!("`{}` is not an interface", iname));
                        continue;
                    }
                };
                if !generics.is_empty() {
                    self.err(*isp, "a generic type can't implement an interface yet");
                    continue;
                }
                self.prog.defs[d].implements.push(iface);
                let imethods = match &self.prog.defs[iface].kind {
                    TypeKind::Interface { methods } => methods.clone(),
                    _ => vec![],
                };
                for im in imethods {
                    let want = self.prog.fns[im].clone();
                    match self.prog.defs[d].methods.get(&want.name).copied() {
                        None => {
                            let sig = self.sig_text(&want);
                            self.err_help(*isp, format!("`{}` doesn't have the method `{}` required by `{}`", name, want.name, iname), format!("add `{}`", sig));
                        }
                        Some(have) => {
                            let h = self.prog.fns[have].clone();
                            let same_params = h.params.len() == want.params.len()
                                && h.params.iter().zip(&want.params).all(|(a, b)| a.1 == b.1);
                            let mode_ok = h.self_mode == want.self_mode
                                || (want.self_mode == SelfMode::Mutating && h.self_mode == SelfMode::Value);
                            let throws_ok = want.throws || !h.throws;
                            if !same_params || h.ret != want.ret || !mode_ok || !throws_ok || !h.generics.is_empty() {
                                let sig = self.sig_text(&want);
                                self.err_help(h.span, format!("`{}` doesn't match the method required by `{}`", want.name, iname), format!("the interface declares `{}`", sig));
                            }
                        }
                    }
                }
            }
        }
    }

    // A type from the standard library that must be closed: its `close` method.
    fn resource_close(&self, t: &Ty) -> Option<FnId> {
        match t {
            Ty::Adt(d, _) => {
                let def = &self.prog.defs[*d];
                if matches!(def.kind, TypeKind::Builtin) && def.module != 0 && self.modules[def.module].privileged {
                    def.methods.get("close").copied()
                } else if matches!(def.kind, TypeKind::Record { .. }) {
                    // a record with `close(self)` owns something to close: a resource too
                    let id = def.methods.get("close").copied()?;
                    let f = &self.prog.fns[id];
                    if f.self_mode != SelfMode::None && f.params.is_empty() {
                        Some(id)
                    } else {
                        None
                    }
                } else {
                    None
                }
            }
            _ => None,
        }
    }

    fn contains_task(&self, t: &Ty) -> bool {
        match t {
            Ty::Adt(d, a) => *d == self.prog.b.task || a.iter().any(|x| self.contains_task(x)),
            Ty::Opt(x) => self.contains_task(x),
            Ty::Func(f) => f.params.iter().any(|x| self.contains_task(x)) || self.contains_task(&f.ret),
            _ => false,
        }
    }

    fn check_task_escapes(&mut self) {
        let task = self.prog.b.task;
        for i in 0..self.prog.fns.len() {
            let f = &self.prog.fns[i];
            if !f.is_prelude && self.contains_task(&f.ret) {
                let sp = f.span;
                self.err_help(sp, "a task can't be returned", "a task belongs to the function that started it: `try task.wait()` there and return the result");
            }
        }
        for d in 0..self.prog.defs.len() {
            let def = &self.prog.defs[d];
            let bad = match &def.kind {
                TypeKind::Record { fields } => fields.iter().any(|f| self.contains_task(&f.ty)),
                TypeKind::Enum { variants } => variants.iter().any(|v| v.fields.iter().any(|f| self.contains_task(&f.ty))),
                _ => false,
            };
            if bad && d != task {
                let sp = def.span;
                self.err_help(sp, "a task can't be stored in a type", "a task belongs to the function that started it");
            }
        }
    }

    fn sig_text(&self, f: &FnDef) -> String {
        let mut ps = vec![];
        if f.self_mode != SelfMode::None {
            ps.push("self".to_string());
        }
        for (n, t) in &f.params {
            ps.push(format!("{}: {}", n, self.prog.show(t)));
        }
        let mut s = format!("{}fn {}({})", if f.self_mode == SelfMode::Mutating { "mutating " } else { "" }, f.name, ps.join(", "));
        if f.throws {
            s += " throws";
        }
        if f.ret != Ty::Unit {
            s += &format!(" -> {}", self.prog.show(&f.ret));
        }
        s
    }

    fn check_defaults(&mut self, astm: &ast::Module, md: usize) {
        // a default may build another record with its defaults
        // (`timing: Timing = Timing()`): check that one first
        let records: Vec<&ast::RecordDecl> = astm.items.iter().filter_map(|i| if let ast::Item::Record(r) = i { Some(r) } else { None }).collect();
        let mentions = |r: &ast::RecordDecl, name: &str| {
            let needle = format!("\"{}\"", name);
            r.fields.iter().any(|f| f.default.as_ref().map(|d| format!("{:?}", d).contains(&needle)).unwrap_or(false))
        };
        let mut order: Vec<usize> = vec![];
        let mut state = vec![0u8; records.len()];
        fn visit(i: usize, records: &[&ast::RecordDecl], state: &mut Vec<u8>, order: &mut Vec<usize>, mentions: &dyn Fn(&ast::RecordDecl, &str) -> bool) {
            if state[i] != 0 {
                return;
            }
            state[i] = 1;
            for j in 0..records.len() {
                if j != i && mentions(records[i], &records[j].name) {
                    visit(j, records, state, order, mentions);
                }
            }
            state[i] = 2;
            order.push(i);
        }
        for i in 0..records.len() {
            visit(i, &records, &mut state, &mut order, &mentions);
        }
        for i in order {
            let r = records[i];
            {
                if r.builtin {
                    continue;
                }
                let d = self.def_id(&r.name, md);
                for (i, f) in r.fields.iter().enumerate() {
                    if let Some(def) = &f.default {
                        self.begin_fn(r.generics.clone(), Ty::Unit, false, md);
                        let fty = match &self.prog.defs[d].kind {
                            TypeKind::Record { fields } => fields[i].ty.clone(),
                            _ => Ty::Err,
                        };
                        let e = self.expr_coerce(def, &fty);
                        let body = self.end_fn(TBlock { stmts: vec![], tail: Some(Box::new(e)), ty: fty });
                        let e = *body.block.tail.unwrap();
                        if !body.locals.is_empty() {
                            self.err(f.span, "a default value can't declare names");
                        }
                        let written = self.source_texts.get(def.span.file as usize).and_then(|s| s.get(def.span.lo as usize..def.span.hi as usize)).map(|s| s.to_string());
                        if let TypeKind::Record { fields } = &mut self.prog.defs[d].kind {
                            fields[i].default = Some(e);
                            fields[i].default_text = written;
                        }
                    }
                }
            }
        }
    }

    // ================= function bodies =================

    fn begin_fn(&mut self, generics: Vec<String>, ret: Ty, throws_ok: bool, md: usize) {
        self.f = Some(FnCtx {
            locals: vec![],
            level: vec![],
            scopes: vec![HashMap::new()],
            generics,
            vars: vec![],
            ret,
            throws_ok,
            uses_throw: false,
            in_test: false,
            lambdas: vec![],
            loops: vec![],
            self_local: None,
            self_mutable: false,
            in_try: false,
            stmt_pos: false,
            try_hits: vec![],
            in_spawn: false,
            spawn_call: false,
            with_value: false,
            resource_ok: false,
            lock_depth: 0,
            module: md,
        });
    }

    fn fcx(&mut self) -> &mut FnCtx {
        self.f.as_mut().unwrap()
    }
    fn fc(&self) -> &FnCtx {
        self.f.as_ref().unwrap()
    }

    fn end_fn(&mut self, mut block: TBlock) -> TBody {
        let mut locals = self.fcx().locals.clone();
        let mut unresolved: Option<Span> = None;
        self.zonk_block(&mut block, &mut unresolved);
        for l in &mut locals {
            l.ty = self.zonk_ty(&l.ty, &mut None);
        }
        if let Some(sp) = unresolved {
            self.err_help(sp, "can't figure out the type here", "add a type: `let xs: List<Int> = []`");
        }
        let params = vec![];
        self.f = None;
        TBody { locals, params, block }
    }

    fn check_fn_body(&mut self, id: FnId, decl: &ast::FnDecl, md: usize) {
        let f = self.prog.fns[id].clone();
        self.begin_fn(f.generics.clone(), f.ret.clone(), f.throws, md);
        let mut params = vec![];
        if f.self_mode != SelfMode::None {
            let owner = f.owner.unwrap();
            let n_owner = self.prog.defs[owner].generics.len();
            let self_ty = match self.prim.get(&owner) {
                Some(p) => p.clone(),
                None => Ty::Adt(owner, (0..n_owner as u32).map(Ty::Param).collect()),
            };
            let l = self.declare(decl.span, "self", self_ty, false);
            self.fcx().self_local = Some(l);
            self.fcx().self_mutable = f.self_mode == SelfMode::Mutating;
            params.push(l);
        }
        for (i, (name, ty)) in f.params.iter().enumerate() {
            let l = self.declare(decl.params[i].span, name, ty.clone(), false);
            params.push(l);
        }
        let body = decl.body.as_ref().unwrap();
        let block = self.block(body, false, None);
        if f.ret == Ty::Never && block.ty != Ty::Never && !self.block_returns(body) && !f.intrinsic {
            self.err_help(body.span, format!("`{}` returns `Never`, so it must not finish", f.name), "end it with `process.exit(...)`, `panic(...)`, `throw`, or a loop that never ends");
        }
        if f.ret != Ty::Unit && block.ty != Ty::Never && !matches!(f.ret, Ty::Never) {
            if !self.block_returns(body) {
                self.err_help(body.span, format!("`{}` must return a `{}` at the end", f.name, self.show(&f.ret)), "add `return ...` as the last line");
            }
        }
        let uses_throw = self.fc().uses_throw;
        let mut tb = self.end_fn(block);
        tb.params = params;
        if f.throws && !uses_throw && !f.intrinsic {
            // a `throws` function that can never fail is allowed (interfaces), no error
        }
        self.prog.fns[id].body = Some(tb);
    }

    // A use of a top-level `let`: a call of its hidden function.
    fn const_use(&mut self, id: FnId, span: Span) -> TExpr {
        self.check_const(id);
        let ty = self.prog.fns[id].ret.clone();
        TExpr { kind: TK::Call { callee: Callee::Fn(id, vec![]), args: vec![], throws: false }, ty, span }
    }

    // Checks a top-level `let` (once, on its first use or at the end), in a
    // context of its own: it may be reached from the middle of another body.
    fn check_const(&mut self, id: FnId) {
        match self.const_state.get(&id) {
            Some(2) => return,
            Some(_) => {
                let (n, sp) = (self.prog.fns[id].name.clone(), self.prog.fns[id].span);
                self.err_help(sp, format!("`{}` is defined through itself", n), "a top-level `let` can use other ones, but not in a circle");
                return;
            }
            None => {}
        }
        let (decl, md) = match self.fn_asts[id].clone() {
            Some(x) => x,
            None => return,
        };
        self.const_state.insert(id, 1);
        let value = match decl.body.as_ref().and_then(|b| b.stmts.first()) {
            Some(Stmt::Return(Some(v), _)) => v.clone(),
            _ => return,
        };
        if let Some(sp) = not_fixed(&value) {
            self.err_help(sp, "a top-level `let` holds a fixed value", "use literals, other top-level `let`s, and lists, maps and records of them; anything computed belongs in a function or in `main`");
            self.const_state.insert(id, 2);
            return;
        }
        let saved = self.f.take();
        let declared = self.prog.fns[id].ret.clone();
        self.begin_fn(vec![], declared.clone(), false, md);
        let e = if declared == Ty::Err { self.expr(&value, None) } else { self.expr_coerce(&value, &declared) };
        let sp = e.span;
        let ty = self.zonk_ty(&e.ty, &mut None);
        let mut body = self.end_fn(TBlock { stmts: vec![TStmt::Return(Some(e))], tail: None, ty: Ty::Never });
        if matches!(ty, Ty::Unit | Ty::Never) {
            self.err(sp, "a top-level `let` needs a value");
        }
        body.params = vec![];
        self.prog.fns[id].ret = ty;
        self.prog.fns[id].body = Some(body);
        self.f = saved;
        self.const_state.insert(id, 2);
    }

    // The `//` lines right above a declaration, joined.
    fn comment_above(&self, span: Span) -> Option<String> {
        let src = self.source_texts.get(span.file as usize)?;
        let before = src.get(..span.lo as usize)?;
        let mut lines: Vec<&str> = before.lines().collect();
        lines.pop(); // the declaration's own line, up to it
        let mut doc: Vec<String> = vec![];
        while let Some(l) = lines.pop() {
            match l.trim().strip_prefix("//") {
                Some(c) => doc.push(c.trim().to_string()),
                None => break,
            }
        }
        doc.reverse();
        if doc.is_empty() {
            None
        } else {
            Some(doc.join(" "))
        }
    }

    fn block_returns(&self, b: &ast::Block) -> bool {
        match b.stmts.last() {
            Some(Stmt::Return(..)) | Some(Stmt::Throw(..)) => true,
            Some(Stmt::Expr(e)) => self.expr_returns(e),
            Some(Stmt::While { cond, .. }) => matches!(cond.kind, ExprKind::Bool(true)),
            Some(Stmt::With { body, .. }) => self.block_returns(body),
            _ => false,
        }
    }
    fn expr_returns(&self, e: &Expr) -> bool {
        match &e.kind {
            ExprKind::If { then, els: Some(els), .. } => self.block_returns(then) && self.expr_returns(els),
            ExprKind::Block(b) => self.block_returns(b),
            ExprKind::Match { arms, .. } => arms.iter().all(|a| self.expr_returns(&a.body)),
            ExprKind::Diverge(_) => true,
            ExprKind::Call { callee, .. } => matches!(&callee.kind, ExprKind::Ident(n) if n == "panic"),
            _ => false,
        }
    }

    fn check_test(&mut self, t: &ast::TestDecl, md: usize) {
        self.begin_fn(vec![], Ty::Unit, true, md);
        self.fcx().in_test = true;
        let block = self.block(&t.body, false, None);
        let body = self.end_fn(block);
        self.prog.tests.push(TestDef { name: t.name.clone(), body, span: t.span });
    }

    // ---- locals and scopes ----

    fn declare(&mut self, span: Span, name: &str, ty: Ty, mutable: bool) -> LocalId {
        if name != "self" && name != "_" {
            let md = self.fc().module;
            if self.modules[md].imports.contains_key(name) {
                self.err_help(span, format!("`{}` is the name of an imported module", name), format!("pick another name, like `{}_value`", name));
            }
            let exists = self.fc().scopes.iter().any(|s| s.contains_key(name));
            if exists {
                self.err_help(span, format!("`{}` is already declared", name), "names can't be redeclared (no shadowing): pick another name, or use `var` and assign");
            }
        }
        let level = self.fc().lambdas.len();
        let fc = self.fcx();
        fc.locals.push(LocalDef { name: name.to_string(), ty, mutable });
        fc.level.push(level);
        let id = fc.locals.len() - 1;
        fc.scopes.last_mut().unwrap().insert(name.to_string(), id);
        id
    }

    // Rebinds a name in the current scope (flow typing), without the
    // redeclaration check.
    fn rebind(&mut self, name: &str, ty: Ty) -> LocalId {
        let level = self.fc().lambdas.len();
        let fc = self.fcx();
        fc.locals.push(LocalDef { name: name.to_string(), ty, mutable: false });
        fc.level.push(level);
        let id = fc.locals.len() - 1;
        fc.scopes.last_mut().unwrap().insert(name.to_string(), id);
        id
    }

    fn lookup_local(&mut self, name: &str) -> Option<LocalId> {
        let fc = self.fc();
        let mut found = None;
        for s in fc.scopes.iter().rev() {
            if let Some(id) = s.get(name) {
                found = Some(*id);
                break;
            }
        }
        let id = found?;
        self.note_use(id);
        Some(id)
    }

    // Records captures for every lambda between the local's level and now.
    fn note_use(&mut self, id: LocalId) {
        let fc = self.fcx();
        let lvl = fc.level[id];
        for lc in fc.lambdas.iter_mut() {
            if lc.level > lvl && !lc.captures.contains(&id) {
                lc.captures.push(id);
            }
        }
    }

    fn is_captured(&self, id: LocalId) -> bool {
        let fc = self.fc();
        fc.level[id] < fc.lambdas.len()
    }

    // ---- inference variables ----

    fn fresh(&mut self) -> Ty {
        let fc = self.fcx();
        fc.vars.push(None);
        Ty::Var((fc.vars.len() - 1) as u32)
    }

    fn resolve(&self, t: &Ty) -> Ty {
        match t {
            Ty::Var(v) => match self.f.as_ref().and_then(|f| f.vars.get(*v as usize).cloned().flatten()) {
                Some(t) => self.resolve(&t),
                None => t.clone(),
            },
            Ty::Adt(d, a) => Ty::Adt(*d, a.iter().map(|x| self.resolve(x)).collect()),
            Ty::Opt(x) => Ty::opt(self.resolve(x)),
            Ty::Func(f) => Ty::Func(Box::new(FnTy {
                params: f.params.iter().map(|x| self.resolve(x)).collect(),
                ret: self.resolve(&f.ret),
                throws: f.throws,
            })),
            t => t.clone(),
        }
    }

    fn occurs(&self, v: u32, t: &Ty) -> bool {
        match self.resolve(t) {
            Ty::Var(w) => w == v,
            Ty::Adt(_, a) => a.iter().any(|x| self.occurs(v, x)),
            Ty::Opt(x) => self.occurs(v, &x),
            Ty::Func(f) => f.params.iter().any(|x| self.occurs(v, x)) || self.occurs(v, &f.ret),
            _ => false,
        }
    }

    // Unifies exactly (no coercions). Function types unify ignoring `throws`
    // only when `loose` is set.
    fn unify(&mut self, a: &Ty, b: &Ty) -> bool {
        let a = self.resolve(a);
        let b = self.resolve(b);
        match (&a, &b) {
            (Ty::Err, _) | (_, Ty::Err) => true,
            (Ty::Var(x), Ty::Var(y)) if x == y => true,
            (Ty::Var(x), t) | (t, Ty::Var(x)) => {
                if self.occurs(*x, t) {
                    return false;
                }
                self.fcx().vars[*x as usize] = Some(t.clone());
                true
            }
            (Ty::Adt(d1, a1), Ty::Adt(d2, a2)) => {
                d1 == d2 && a1.len() == a2.len() && a1.clone().iter().zip(a2.clone().iter()).all(|(x, y)| self.unify(x, y))
            }
            (Ty::Opt(x), Ty::Opt(y)) => {
                let (x, y) = (*x.clone(), *y.clone());
                self.unify(&x, &y)
            }
            (Ty::Func(f), Ty::Func(g)) => {
                if f.params.len() != g.params.len() || f.throws != g.throws {
                    return false;
                }
                let (f, g) = (f.clone(), g.clone());
                f.params.iter().zip(g.params.iter()).all(|(x, y)| self.unify(x, y)) && self.unify(&f.ret, &g.ret)
            }
            _ => a == b,
        }
    }

    fn implements(&self, d: DefId, ifaces: &[DefId]) -> bool {
        ifaces.iter().all(|i| self.prog.defs[d].implements.contains(i))
    }

    // Checks that `e` fits `expected`, inserting conversions:
    // T -> T?, a type -> an interface it implements, Never -> anything,
    // a non-throwing function -> a throwing function type.
    fn coerce(&mut self, e: TExpr, expected: &Ty) -> TExpr {
        let exp = self.resolve(expected);
        let act = self.resolve(&e.ty);
        if act == Ty::Never || exp == Ty::Err || act == Ty::Err {
            return TExpr { ty: if act == Ty::Never { exp } else { act }, ..e };
        }
        match (&act, &exp) {
            (Ty::Opt(_), Ty::Opt(_)) | (Ty::Var(_), _) | (_, Ty::Var(_)) => {}
            (_, Ty::Opt(inner)) => {
                let inner = *inner.clone();
                let e = self.coerce(e, &inner);
                let sp = e.span;
                return TExpr { kind: TK::Some(Box::new(e)), ty: exp, span: sp };
            }
            // `sql.Query`: only SQL written right here, never text built at run time
            (_, Ty::Adt(d, _)) if self.sql_def(*d, "Query") && !matches!(&act, Ty::Adt(x, _) if x == d) => {
                let sp = e.span;
                if matches!(e.kind, TK::Text(_)) {
                    return TExpr { kind: TK::Wrap(Box::new(e)), ty: exp, span: sp };
                }
                if act == Ty::Text {
                    self.err_help(sp, "the SQL must be written right here, as text in quotes", "put values in as parameters: `conn.query<User>(\"select * from users where id = ?\", [id])`");
                    return TExpr { ty: Ty::Err, ..e };
                }
            }
            (_, Ty::Adt(d, _)) if self.db_value_def(*d) && !matches!(&act, Ty::Adt(x, _) if x == d) => {
                let d = *d;
                if let Some(v) = self.to_db_value(e.clone(), &exp, d) {
                    return v;
                }
            }
            (Ty::Adt(d, _), Ty::Iface(ids)) => {
                if self.implements(*d, ids) {
                    let sp = e.span;
                    return TExpr { kind: TK::ToIface(Box::new(e)), ty: exp, span: sp };
                }
            }
            (Ty::Iface(have), Ty::Iface(want)) if have != want => {
                if want.iter().all(|w| have.contains(w)) {
                    let sp = e.span;
                    return TExpr { kind: TK::ToIface(Box::new(e)), ty: exp, span: sp };
                }
            }
            // a named function type (`type Check = fn(Int) -> Bool`) is only
            // a name for the signature: any function of that type fits
            (Ty::Func(_), Ty::Adt(d, _)) if matches!(&self.prog.defs[*d].kind, TypeKind::Newtype(Ty::Func(_))) => {
                let inner = match &self.prog.defs[*d].kind {
                    TypeKind::Newtype(t) => t.clone(),
                    _ => unreachable!(),
                };
                let inner = match &exp {
                    Ty::Adt(_, a) => inner.subst(a),
                    _ => inner,
                };
                let e = self.coerce(e, &inner);
                if self.try_unify(&e.ty, &inner) {
                    let sp = e.span;
                    return TExpr { kind: TK::Wrap(Box::new(e)), ty: exp, span: sp };
                }
                return e;
            }
            (Ty::Func(f), Ty::Func(g)) if !f.throws && g.throws => {
                let mut f2 = (**f).clone();
                f2.throws = true;
                if self.unify(&Ty::Func(Box::new(f2)), &exp) {
                    let sp = e.span;
                    // calling convention changes: lowered as an adapter
                    return TExpr { kind: TK::Wrap(Box::new(e)), ty: exp, span: sp };
                }
            }
            _ => {}
        }
        if !self.unify(&act, &exp) {
            self.mismatch(e.span, &exp, &act);
            return TExpr { ty: Ty::Err, ..e };
        }
        e
    }

    // `sql.Value`: database parameters are written as plain values
    fn db_value_def(&self, d: DefId) -> bool {
        self.sql_def(d, "Value")
    }

    fn sql_def(&self, d: DefId, name: &str) -> bool {
        let def = &self.prog.defs[d];
        def.name == name && self.prog.module_names.get(def.module).map(|m| m == "sql").unwrap_or(false)
    }

    fn to_db_value(&mut self, e: TExpr, exp: &Ty, d: DefId) -> Option<TExpr> {
        let act = self.resolve(&e.ty);
        let sp = e.span;
        if matches!(e.kind, TK::NoneLit) {
            return Some(TExpr { kind: TK::Variant { def: d, idx: 0, fields: vec![] }, ty: exp.clone(), span: sp });
        }
        let (inner, e) = match &act {
            Ty::Adt(nd, _) => match &self.prog.defs[*nd].kind {
                TypeKind::Newtype(i) => {
                    let i = i.clone();
                    (i.clone(), TExpr { kind: TK::Unwrap(Box::new(e)), ty: i, span: sp })
                }
                _ => (act.clone(), e),
            },
            _ => (act.clone(), e),
        };
        let idx = match &inner {
            Ty::Int => 1,
            Ty::Float => 2,
            Ty::Text => 3,
            Ty::Bool => 5,
            Ty::Adt(b, _) if *b == self.prog.b.bytes => 4,
            // a Decimal travels as text: exact
            Ty::Adt(b, _) if *b == self.prog.b.decimal => {
                let text = TExpr { kind: TK::ToText(Box::new(e)), ty: Ty::Text, span: sp };
                return Some(TExpr { kind: TK::Variant { def: d, idx: 3, fields: vec![text] }, ty: exp.clone(), span: sp });
            }
            _ => return None,
        };
        Some(TExpr { kind: TK::Variant { def: d, idx, fields: vec![e] }, ty: exp.clone(), span: sp })
    }

    fn mismatch(&mut self, span: Span, exp: &Ty, act: &Ty) {
        let exp = self.resolve(exp);
        let act = self.resolve(act);
        let es = self.prog.show(&exp);
        let as_ = self.prog.show(&act);
        let msg = format!("expected `{}`, found `{}`", es, as_);
        let help: Option<String> = match (&exp, &act) {
            (Ty::Float, Ty::Int) => Some("convert explicitly: `x.to_float()`".into()),
            (Ty::Int, Ty::Float) => Some("convert explicitly: `x.round()`, `x.floor()` or `x.ceil()`".into()),
            (Ty::Text, Ty::Int) | (Ty::Text, Ty::Float) | (Ty::Text, Ty::Bool) => Some("use interpolation: `\"${x}\"`, or `x.to_string()`".into()),
            (Ty::Int, Ty::Text) => Some("parse it: `try text.to_int()`".into()),
            (t, Ty::Opt(inner)) if **inner == *t => Some("the value may be missing: use `x ?? fallback`, or `if x is some(v) { ... }`".into()),
            (Ty::Adt(d, _), _) if matches!(self.prog.defs[*d].kind, TypeKind::Newtype(_)) => {
                Some(format!("wrap it: `{}(x)`", self.prog.defs[*d].name))
            }
            (_, Ty::Adt(d, _)) if matches!(self.prog.defs[*d].kind, TypeKind::Newtype(_)) => Some("unwrap it: `x.value`".into()),
            (Ty::Iface(ids), Ty::Adt(d, _)) => {
                let iname = self.prog.defs[ids[0]].name.clone();
                Some(format!("declare it: `type {} implements {} {{ ... }}`", self.prog.defs[*d].name, iname))
            }
            (Ty::Func(g), Ty::Func(f)) if f.throws && !g.throws => Some("this function uses `try`, but a function that can't fail is expected here".into()),
            _ => None,
        };
        match help {
            Some(h) => self.err_help(span, msg, h),
            None => self.err(span, msg),
        }
    }

    fn expr_coerce(&mut self, e: &Expr, expected: &Ty) -> TExpr {
        let t = self.expr(e, Some(expected));
        self.coerce(t, expected)
    }

    // ================= statements =================

    fn block(&mut self, b: &ast::Block, want_value: bool, expected: Option<&Ty>) -> TBlock {
        self.fcx().scopes.push(HashMap::new());
        let mut stmts = vec![];
        let mut tail = None;
        let mut diverges = false;
        let n = b.stmts.len();
        for (i, s) in b.stmts.iter().enumerate() {
            if diverges {
                self.err(stmt_span(s), "this line is never reached");
                break;
            }
            if want_value && i == n - 1 {
                if let Stmt::Expr(e) = s {
                    let te = self.expr(e, expected);
                    if te.ty == Ty::Never {
                        diverges = true;
                    }
                    tail = Some(Box::new(te));
                    continue;
                }
            }
            let ts = self.stmt(s);
            if stmt_diverges(&ts) {
                diverges = true;
            }
            // flow typing: `if x is none { <diverges> }` makes `x` a `T` below
            if let (Stmt::Expr(e), TStmt::Expr(te)) = (s, &ts) {
                if let Some((name, id, inner)) = self.narrowing(e, te) {
                    stmts.push(ts);
                    let new_id = self.rebind(&name, inner.clone());
                    let sp = e.span;
                    let src = TExpr { kind: TK::Local(id), ty: Ty::opt(inner.clone()), span: sp };
                    let un = TExpr { kind: TK::Unwrap(Box::new(src)), ty: inner, span: sp };
                    stmts.push(TStmt::Let(new_id, un));
                    continue;
                }
            }
            stmts.push(ts);
        }
        self.fcx().scopes.pop();
        let ty = if diverges {
            Ty::Never
        } else if let Some(t) = &tail {
            t.ty.clone()
        } else {
            Ty::Unit
        };
        if want_value && tail.is_none() && !diverges {
            if let Some(exp) = expected {
                let r = self.resolve(exp);
                if r != Ty::Unit && !matches!(r, Ty::Var(_)) {
                    self.err(b.span, format!("this block must end with a `{}` value", self.show(&r)));
                }
            }
        }
        TBlock { stmts, tail, ty }
    }

    fn narrowing(&mut self, e: &Expr, te: &TExpr) -> Option<(String, LocalId, Ty)> {
        if let ExprKind::If { cond, els: None, .. } = &e.kind {
            if let ExprKind::Is(scrut, Pattern::None(_)) = &cond.kind {
                if let ExprKind::Ident(name) = &scrut.kind {
                    if let TK::If { then, .. } = &te.kind {
                        if then.ty == Ty::Never {
                            let id = self.lookup_local(name)?;
                            if let Ty::Opt(inner) = self.resolve(&self.fc().locals[id].ty) {
                                return Some((name.clone(), id, *inner));
                            }
                        }
                    }
                    if let TK::Match { arms, .. } = &te.kind {
                        if arms.first().map(|a| a.body.ty == Ty::Never).unwrap_or(false) {
                            let id = self.lookup_local(name)?;
                            if let Ty::Opt(inner) = self.resolve(&self.fc().locals[id].ty) {
                                return Some((name.clone(), id, *inner));
                            }
                        }
                    }
                }
            }
        }
        None
    }

    fn check_unused(&mut self, e: &TExpr, src: &Expr) {
        let t = self.resolve(&e.ty);
        if matches!(t, Ty::Unit | Ty::Never | Ty::Err) {
            return;
        }
        if matches!(e.kind, TK::Spawn(_)) {
            if let Ty::Adt(_, a) = &t {
                if a[0] == Ty::Unit {
                    return;
                }
            }
            self.err_help(e.span, "the task's result is never waited for", "keep the task and `try task.wait()` for its result");
            return;
        }
        let mut help = "use the result, or write `let _ = ...` if you really don't need it".to_string();
        if let ExprKind::Call { callee, .. } = &src.kind {
            if let ExprKind::Field(_, name, _) = &callee.kind {
                let alt = match name.as_str() {
                    "sorted" => Some("sort()"),
                    "sorted_by" => Some("sort_by(...)"),
                    "reversed" => Some("reverse()"),
                    "concat" => Some("append_all(...)"),
                    _ => None,
                };
                if let Some(a) = alt {
                    help = format!("`{}` returns a new value; to change the list in place, write `{}`", name, a);
                }
            }
        }
        let ts = self.show(&t);
        self.err_help(e.span, format!("the result (`{}`) is not used", ts), help);
    }

    fn stmt(&mut self, s: &Stmt) -> TStmt {
        match s {
            Stmt::Let { name, mutable, ty, value, span } => {
                if name == "_" {
                    if *mutable {
                        self.err(*span, "write `let _ = ...`");
                    }
                    let v = self.expr(value, None);
                    return TStmt::Discard(v);
                }
                let md = self.fc().module;
                let generics = self.fc().generics.clone();
                let v = match ty {
                    Some(t) => {
                        let t = self.resolve_texpr(t, &generics, md);
                        self.expr_coerce(value, &t)
                    }
                    None => self.expr(value, None),
                };
                let vt = self.resolve(&v.ty);
                if vt == Ty::Unit {
                    self.err(value.span, "this produces no value");
                }
                if matches!(&vt, Ty::Adt(d, _) if *d == self.prog.b.locked) {
                    self.err_help(value.span, "a lock is taken with `with`", format!("write `with {} = ... {{ ... }}`: the lock is released at the end of the block", name));
                }
                if vt == Ty::Never {
                    self.err(value.span, "this never produces a value");
                }
                let id = self.declare(*span, name, v.ty.clone(), *mutable);
                TStmt::Let(id, v)
            }
            Stmt::Assign { target, op, value, span } => {
                let place = match self.place(target, true) {
                    Some(p) => p,
                    None => {
                        let _ = self.expr(value, None);
                        return TStmt::Expr(TExpr { kind: TK::Unit, ty: Ty::Unit, span: *span });
                    }
                };
                let pty = place.ty.clone();
                let v = match op {
                    None => self.expr_coerce(value, &pty),
                    Some(op) => {
                        let cur = self.place_read(&place, target.span);
                        let rhs = self.expr(value, Some(&pty));
                        self.binary(*op, cur, rhs, *span, Some(&pty))
                    }
                };
                let v = self.coerce(v, &pty);
                TStmt::Assign(place, v)
            }
            Stmt::Expr(e) => {
                self.fcx().stmt_pos = true;
                let te = self.expr(e, None);
                self.fcx().stmt_pos = false;
                let te = self.fix_stmt_expr(te);
                self.check_unused(&te, e);
                TStmt::Expr(te)
            }
            Stmt::Return(v, span) => {
                if !self.fc().lambdas.is_empty() {
                    self.err_help(*span, "no `return` inside a lambda", "the lambda's last line is its value");
                }
                let ret = self.fc().ret.clone();
                if ret == Ty::Never && self.fc().lambdas.is_empty() {
                    self.err(*span, "this function returns `Never`: it can't `return`");
                }
                match v {
                    Some(v) if ret == Ty::Never => {
                        let e = self.expr(v, None);
                        TStmt::Return(Some(e))
                    }
                    Some(v) => {
                        if ret == Ty::Unit {
                            self.err(v.span, "this function returns nothing (it has no `->`)");
                            let _ = self.expr(v, None);
                            TStmt::Return(None)
                        } else {
                            // a function may hand a new resource to its caller, who
                            // must then use `with`: `fn open_db() throws -> db.Connection`
                            let rt = self.resolve(&ret);
                            if self.resource_close(&rt).is_some() {
                                self.fcx().with_value = true;
                            }
                            let e = self.expr_coerce(v, &ret);
                            self.fcx().with_value = false;
                            TStmt::Return(Some(e))
                        }
                    }
                    None => {
                        if ret != Ty::Unit && ret != Ty::Never {
                            let r = self.show(&ret);
                            self.err(*span, format!("return a `{}` value", r));
                        }
                        TStmt::Return(None)
                    }
                }
            }
            Stmt::Break(span) | Stmt::Continue(span) => {
                let lvl = self.fc().lambdas.len();
                match self.fc().loops.last() {
                    Some(l) if *l == lvl => {}
                    Some(_) => self.err(*span, "no `break`/`continue` inside a lambda"),
                    None => self.err(*span, "`break`/`continue` outside a loop"),
                }
                if matches!(s, Stmt::Break(_)) {
                    TStmt::Break
                } else {
                    TStmt::Continue
                }
            }
            Stmt::Throw(e, span) => {
                let et = self.prog.error_ty();
                let v = self.expr(e, None);
                let vt = self.resolve(&v.ty);
                let ok = match &vt {
                    Ty::Adt(d, _) => self.prog.defs[*d].implements.contains(&self.prog.b.error),
                    Ty::Iface(ids) => ids.contains(&self.prog.b.error),
                    Ty::Err => true,
                    _ => false,
                };
                if !ok {
                    let s = self.show(&vt);
                    self.err_help(e.span, format!("`{}` is not an error", s), "throw `Failure(message: \"...\")`, or a type that `implements Error`");
                }
                self.note_throw(*span);
                let v = if ok { self.coerce(v, &et) } else { v };
                TStmt::Throw(v)
            }
            Stmt::While { cond, body, span } if matches!(&cond.kind, ExprKind::Is(_, p) if pattern_binds(p)) => {
                // `while x is some(v) { ... }`: loop { match x { some(v) => body, _ => break } }
                let (scrut, pat) = match &cond.kind {
                    ExprKind::Is(s, p) => (s, p),
                    _ => unreachable!(),
                };
                let lvl = self.fc().lambdas.len();
                self.fcx().loops.push(lvl);
                let s = self.expr(scrut, None);
                let sty = s.ty.clone();
                self.fcx().scopes.push(HashMap::new());
                let p = self.pattern(pat, &sty);
                let b = self.block(body, false, None);
                self.fcx().scopes.pop();
                self.fcx().loops.pop();
                let sp = *span;
                let body_e = TExpr { ty: Ty::Unit, kind: TK::Block(b), span: sp };
                let brk = TExpr { kind: TK::Diverge(Box::new(TStmt::Break)), ty: Ty::Never, span: sp };
                let m = TExpr {
                    kind: TK::Match { scrut: Box::new(s), arms: vec![TArm { pat: p, guard: None, body: body_e }, TArm { pat: TPat::Wild, guard: None, body: brk }] },
                    ty: Ty::Unit,
                    span: sp,
                };
                let loop_body = TBlock { stmts: vec![TStmt::Expr(m)], tail: None, ty: Ty::Unit };
                TStmt::While(TExpr { kind: TK::Bool(true), ty: Ty::Bool, span: sp }, loop_body)
            }
            Stmt::While { cond, body, .. } => {
                let c = self.expr_coerce(cond, &Ty::Bool);
                let lvl = self.fc().lambdas.len();
                self.fcx().loops.push(lvl);
                let b = self.block(body, false, None);
                self.fcx().loops.pop();
                TStmt::While(c, b)
            }
            Stmt::For { var, iter, body, span } => {
                if let ExprKind::Range { lo, hi, inclusive } = &iter.kind {
                    let lo = self.expr_coerce(lo, &Ty::Int);
                    let hi = self.expr_coerce(hi, &Ty::Int);
                    self.fcx().scopes.push(HashMap::new());
                    let v = self.declare(*span, var, Ty::Int, false);
                    let lvl = self.fc().lambdas.len();
                    self.fcx().loops.push(lvl);
                    let b = self.block(body, false, None);
                    self.fcx().loops.pop();
                    self.fcx().scopes.pop();
                    return TStmt::ForRange { var: v, lo, hi, inclusive: *inclusive, body: b };
                }
                let it = self.expr(iter, None);
                let ity = self.resolve(&it.ty);
                let (kind, elem) = match &ity {
                    Ty::Adt(d, a) if *d == self.prog.b.list => (0, a[0].clone()),
                    Ty::Adt(d, a) if *d == self.prog.b.map => (1, Ty::Adt(self.prog.b.entry, a.clone())),
                    Ty::Adt(d, a) if *d == self.prog.b.set => (2, a[0].clone()),
                    Ty::Adt(d, a) if *d == self.prog.b.channel => (3, a[0].clone()),
                    Ty::Err => (0, Ty::Err),
                    Ty::Text => {
                        self.err_help(iter.span, "can't loop over `String` directly", "use `text.chars()`, `text.words()` or `text.lines()`");
                        (0, Ty::Err)
                    }
                    t => {
                        let s = self.prog.show(t);
                        self.err(iter.span, format!("can't loop over a `{}`", s));
                        (0, Ty::Err)
                    }
                };
                self.fcx().scopes.push(HashMap::new());
                let v = self.declare(*span, var, elem, false);
                let lvl = self.fc().lambdas.len();
                self.fcx().loops.push(lvl);
                let b = self.block(body, false, None);
                self.fcx().loops.pop();
                self.fcx().scopes.pop();
                match kind {
                    0 => TStmt::ForList { var: v, list: it, body: b },
                    1 => TStmt::ForMap { var: v, map: it, body: b },
                    2 => TStmt::ForSet { var: v, set: it, body: b },
                    _ => TStmt::ForChannel { var: v, chan: it, body: b },
                }
            }
            Stmt::With { name, value, body, span } => {
                // `with x = shared.lock() { ... }`
                if let ExprKind::Call { callee, args, .. } = &value.kind {
                    if let ExprKind::Field(recv, m, _) = &callee.kind {
                        if m == "lock" && args.is_empty() {
                            let sh = self.expr(recv, None);
                            let st = self.resolve(&sh.ty);
                            if let Ty::Adt(d, a) = &st {
                                if *d == self.prog.b.shared {
                                    if self.fc().lock_depth > 0 {
                                        self.err_help(*span, "a lock inside another lock can deadlock", "take the locks one after another, or keep both values in one `Shared`");
                                    }
                                    self.fcx().scopes.push(HashMap::new());
                                    let v = self.declare(*span, name, a[0].clone(), true);
                                    self.fcx().lock_depth += 1;
                                    let b = self.block(body, false, None);
                                    self.fcx().lock_depth -= 1;
                                    self.fcx().scopes.pop();
                                    return TStmt::WithLock { var: v, shared: sh, body: b };
                                }
                            }
                        }
                    }
                }
                self.fcx().with_value = true;
                let v = self.expr(value, None);
                self.fcx().with_value = false;
                let vt = self.resolve(&v.ty);
                let close = match self.resource_close(&vt) {
                    Some(c) => c,
                    None => {
                        if vt != Ty::Err {
                            let s = self.show(&vt);
                            self.err_help(*span, format!("`with` is for things that must be closed, and `{}` isn't one", s), "use `let` instead");
                        }
                        return TStmt::Expr(TExpr { kind: TK::Unit, ty: Ty::Unit, span: *span });
                    }
                };
                self.fcx().scopes.push(HashMap::new());
                let var = self.declare(*span, name, vt, false);
                let b = self.block(body, false, None);
                self.fcx().scopes.pop();
                if self.prog.fns[close].throws {
                    self.note_throw(*span);
                }
                TStmt::With { var, value: v, body: b, close }
            }
            Stmt::Expect { cond, span } => {
                if !self.fc().in_test {
                    self.err_help(*span, "`expect` is only allowed in tests", "use `assert(cond)` for a condition that can only fail because of a bug");
                }
                let c = self.expr_coerce(cond, &Ty::Bool);
                TStmt::Expect { cond: c, text: String::new(), span: cond.span }
            }
        }
    }

    // A statement-level `if`/`match` has no value: its branches were checked
    // with want_value, which is fine; nothing to fix up for now.
    fn fix_stmt_expr(&mut self, e: TExpr) -> TExpr {
        e
    }

    fn note_throw(&mut self, span: Span) {
        let fc = self.fcx();
        if let Some(l) = fc.lambdas.last_mut() {
            l.throws = true;
            return;
        }
        fc.uses_throw = true;
        if !fc.throws_ok {
            self.err_help(span, "this function can't fail: it isn't declared `throws`", "add `throws` before `->`: `fn f() throws -> T`, or handle the error with `catch`");
        }
    }

    // ================= places =================

    fn place(&mut self, e: &Expr, write: bool) -> Option<TPlace> {
        match &e.kind {
            ExprKind::Ident(name) => {
                let id = match self.lookup_local(name) {
                    Some(id) => id,
                    None => {
                        let md = self.fc().module;
                        match self.lookup_global(name, md) {
                            Some(Global::Fn(f)) if self.prog.fns[f].is_const => self.err_help(e.span, format!("`{}` is a top-level `let`: it can't change", name), format!("copy it into a local: `var {n}_now = {n}`", n = name)),
                            _ => self.err(e.span, format!("unknown name `{}`", name)),
                        }
                        return None;
                    }
                };
                let l = self.fc().locals[id].clone();
                if write {
                    if self.is_captured(id) {
                        self.err_help(e.span, format!("can't change `{}` inside a lambda: lambdas capture a copy", name), "compute the value with the lambda's result (`map`, `fold`, `filter`), or share it through `Shared<T>` and change it in `with v = s.lock() { }`");
                        return None;
                    }
                    if !l.mutable {
                        self.err_help(e.span, format!("can't change `{}`: it's declared with `let`", name), format!("declare it with `var {} = ...`", name));
                        return None;
                    }
                }
                Some(TPlace { root: id, path: vec![], ty: l.ty })
            }
            ExprKind::SelfRef => {
                let id = match self.fc().self_local {
                    Some(id) => id,
                    None => {
                        self.err(e.span, "`self` is only available in methods");
                        return None;
                    }
                };
                self.note_use(id);
                if write && !self.fc().self_mutable {
                    self.err_help(e.span, "can't change `self` in a method that isn't `mutating`", "declare it `mutating fn ...`");
                    return None;
                }
                if write && self.is_captured(id) {
                    self.err(e.span, "can't change `self` inside a lambda");
                    return None;
                }
                let ty = self.fc().locals[id].ty.clone();
                Some(TPlace { root: id, path: vec![], ty })
            }
            ExprKind::Field(base, name, nsp) => {
                let mut p = self.place(base, write)?;
                let bt = self.resolve(&p.ty);
                match &bt {
                    Ty::Adt(d, args) => {
                        if let TypeKind::Record { fields } = &self.prog.defs[*d].kind {
                            if let Some(i) = fields.iter().position(|f| f.name == *name) {
                                let fty = fields[i].ty.subst(args);
                                p.path.push(PlaceElem::Field(i));
                                p.ty = fty;
                                return Some(p);
                            }
                        }
                        let s = self.prog.show(&bt);
                        self.err(*nsp, format!("`{}` has no field `{}` that can be changed", s, name));
                        None
                    }
                    Ty::Opt(_) => {
                        self.err_help(*nsp, "this value may be missing", "unwrap it first: `if x is some(v) { ... }`");
                        None
                    }
                    _ => {
                        let s = self.prog.show(&bt);
                        self.err(*nsp, format!("`{}` has no field `{}`", s, name));
                        None
                    }
                }
            }
            ExprKind::Index(base, idx) => {
                let mut p = self.place(base, write)?;
                let bt = self.resolve(&p.ty);
                match &bt {
                    Ty::Adt(d, args) if *d == self.prog.b.list => {
                        let i = self.expr_coerce(idx, &Ty::Int);
                        p.path.push(PlaceElem::Index(i));
                        p.ty = args[0].clone();
                        Some(p)
                    }
                    Ty::Adt(d, args) if *d == self.prog.b.map => {
                        let k = self.expr_coerce(idx, &args[0]);
                        p.path.push(PlaceElem::MapKey(k));
                        p.ty = args[1].clone();
                        Some(p)
                    }
                    _ => {
                        let s = self.prog.show(&bt);
                        self.err(e.span, format!("can't index a `{}`", s));
                        None
                    }
                }
            }
            _ => {
                if write {
                    self.err(e.span, "can't assign to this");
                } else {
                    self.err(e.span, "a mutating method needs a variable here");
                }
                None
            }
        }
    }

    fn place_read(&mut self, p: &TPlace, span: Span) -> TExpr {
        let root_ty = self.fc().locals[p.root].ty.clone();
        let mut e = TExpr { kind: TK::Local(p.root), ty: root_ty, span };
        for el in &p.path {
            let bt = self.resolve(&e.ty);
            e = match el {
                PlaceElem::Field(i) => {
                    let fty = match &bt {
                        Ty::Adt(d, args) => match &self.prog.defs[*d].kind {
                            TypeKind::Record { fields } => fields[*i].ty.subst(args),
                            _ => Ty::Err,
                        },
                        _ => Ty::Err,
                    };
                    TExpr { kind: TK::Field(Box::new(e), *i), ty: fty, span }
                }
                PlaceElem::Index(i) => {
                    let et = match &bt {
                        Ty::Adt(_, a) => a[0].clone(),
                        _ => Ty::Err,
                    };
                    TExpr { kind: TK::Index(Box::new(e), Box::new(i.clone())), ty: et, span }
                }
                PlaceElem::MapKey(k) => {
                    // reading `m[k]` inside a compound assignment: the key must exist
                    let vt = match &bt {
                        Ty::Adt(_, a) => a[1].clone(),
                        _ => Ty::Err,
                    };
                    let get = TExpr { kind: TK::MapGet(Box::new(e), Box::new(k.clone())), ty: Ty::opt(vt.clone()), span };
                    TExpr { kind: TK::Unwrap(Box::new(get)), ty: vt, span }
                }
            };
        }
        e
    }

    // ================= expressions =================

    fn is_decimal(&self, t: &Ty) -> bool {
        matches!(t, Ty::Adt(d, _) if *d == self.prog.b.decimal)
    }

    // `let price: Decimal = 19.99`: the literal's digits, exactly
    fn decimal_lit(&mut self, digits: String, span: Span) -> TExpr {
        let id = match self.lookup_global("__decimal", 0) {
            Some(Global::Fn(id)) => id,
            _ => panic!("__decimal missing"),
        };
        let arg = TExpr { kind: TK::Text(digits), ty: Ty::Text, span };
        TExpr { kind: TK::Call { callee: Callee::Fn(id, vec![]), args: vec![arg], throws: false }, ty: Ty::Adt(self.prog.b.decimal, vec![]), span }
    }

    fn lit_int(&mut self, v: i64, span: Span, expected: Option<&Ty>) -> TExpr {
        let exp = expected.map(|t| self.resolve(t));
        let exp_inner = match &exp {
            Some(Ty::Opt(i)) => Some((**i).clone()),
            e => e.clone(),
        };
        if exp_inner.as_ref().map(|t| self.is_decimal(t)).unwrap_or(false) {
            return self.decimal_lit(v.to_string(), span);
        }
        match exp {
            Some(Ty::Float) => TExpr { kind: TK::Float(v as f64), ty: Ty::Float, span },
            Some(Ty::Opt(inner)) if *inner == Ty::Float => TExpr { kind: TK::Float(v as f64), ty: Ty::Float, span },
            _ => TExpr { kind: TK::Int(v), ty: Ty::Int, span },
        }
    }

    pub fn expr(&mut self, e: &Expr, expected: Option<&Ty>) -> TExpr {
        let span = e.span;
        let mk = |kind, ty| TExpr { kind, ty, span };
        // `try` applies only to the call right below it
        let in_try = std::mem::replace(&mut self.fcx().in_try, false);
        let stmt_pos = std::mem::replace(&mut self.fcx().stmt_pos, false);
        let in_spawn = std::mem::replace(&mut self.fcx().in_spawn, false);
        let with_value = std::mem::replace(&mut self.fcx().with_value, false);
        match &e.kind {
            ExprKind::Try { .. } if with_value => self.fcx().with_value = true,
            ExprKind::Call { .. } if with_value => self.fcx().resource_ok = true,
            _ => {}
        }
        if in_spawn {
            self.fcx().spawn_call = true;
        }
        let _ = in_try;
        match &e.kind {
            ExprKind::Int(v) => self.lit_int(*v, span, expected),
            ExprKind::Float(v) => {
                let want = expected.map(|t| self.resolve(t));
                let want_inner = match &want {
                    Some(Ty::Opt(i)) => Some((**i).clone()),
                    w => w.clone(),
                };
                if want_inner.as_ref().map(|t| self.is_decimal(t)).unwrap_or(false) {
                    // the literal as written (1.50 keeps its two digits)
                    let written = self.source_texts.get(span.file as usize).and_then(|s| s.get(span.lo as usize..span.hi as usize)).map(|s| s.replace('_', "")).filter(|s| s.chars().all(|c| c.is_ascii_digit() || c == '.' || c == '-'));
                    return self.decimal_lit(written.unwrap_or_else(|| format!("{}", v)), span);
                }
                if let Some(Ty::Int) = expected.map(|t| self.resolve(t)) {
                    self.err_help(span, "expected an `Int`, found a `Float` literal", "write a whole number, or convert: `x.round()`");
                }
                mk(TK::Float(*v), Ty::Float)
            }
            ExprKind::Bool(b) => mk(TK::Bool(*b), Ty::Bool),
            ExprKind::None => {
                let ty = match expected.map(|t| self.resolve(t)) {
                    Some(t @ Ty::Opt(_)) => t,
                    _ => {
                        let v = self.fresh();
                        Ty::opt(v)
                    }
                };
                mk(TK::NoneLit, ty)
            }
            ExprKind::Str(segs) => {
                let mut parts = vec![];
                for s in segs {
                    match s {
                        ast::StrSeg::Lit(t) => parts.push(TExpr { kind: TK::Text(t.clone()), ty: Ty::Text, span }),
                        ast::StrSeg::Expr(x) => {
                            let te = self.expr(x, None);
                            let t = self.resolve(&te.ty);
                            match t {
                                Ty::Text => parts.push(te),
                                Ty::Unit | Ty::Func(_) | Ty::Never => {
                                    let s = self.prog.show(&t);
                                    self.err(x.span, format!("a `{}` can't be put into text", s));
                                }
                                _ => {
                                    let sp = te.span;
                                    parts.push(TExpr { kind: TK::ToText(Box::new(te)), ty: Ty::Text, span: sp });
                                }
                            }
                        }
                    }
                }
                if parts.len() == 1 && matches!(parts[0].kind, TK::Text(_)) {
                    return parts.pop().unwrap();
                }
                if parts.is_empty() {
                    return mk(TK::Text(String::new()), Ty::Text);
                }
                mk(TK::Interp(parts), Ty::Text)
            }
            ExprKind::Ident(name) => self.ident(name, span, expected),
            ExprKind::SelfRef => match self.fc().self_local {
                Some(id) => {
                    self.note_use(id);
                    let ty = self.fc().locals[id].ty.clone();
                    mk(TK::Local(id), ty)
                }
                None => {
                    self.err(span, "`self` is only available in methods");
                    mk(TK::Unit, Ty::Err)
                }
            },
            ExprKind::List(items) => {
                let elem = match expected.map(|t| self.resolve(t)) {
                    Some(Ty::Adt(d, a)) if d == self.prog.b.list => a[0].clone(),
                    _ => self.fresh(),
                };
                let mut out = vec![];
                for it in items {
                    let te = self.expr(it, Some(&elem));
                    let te = self.coerce(te, &elem);
                    out.push(te);
                }
                let ty = self.prog.list_of(elem);
                mk(TK::List(out), ty)
            }
            ExprKind::Map(entries) => {
                let exp = expected.map(|t| self.resolve(t));
                if let Some(Ty::Adt(d, a)) = &exp {
                    if *d == self.prog.b.set {
                        if !entries.is_empty() {
                            self.err_help(span, "a set has no literal with values", "start with `Set<T>()` and `add` items");
                        }
                        return mk(TK::EmptySet, Ty::Adt(*d, a.clone()));
                    }
                }
                let (k, v) = match &exp {
                    Some(Ty::Adt(d, a)) if *d == self.prog.b.map => (a[0].clone(), a[1].clone()),
                    _ => (self.fresh(), self.fresh()),
                };
                let mut out = vec![];
                for (ke, ve) in entries {
                    let kt = self.expr_coerce(ke, &k);
                    let vt = self.expr_coerce(ve, &v);
                    out.push((kt, vt));
                }
                mk(TK::Map(out), Ty::Adt(self.prog.b.map, vec![k, v]))
            }
            ExprKind::Field(base, name, nsp) => self.field(base, name, *nsp, span),
            ExprKind::Call { callee, type_args, args } => self.call(e, callee, type_args, args, expected, in_try || in_spawn),
            ExprKind::Index(base, idx) => {
                let b = self.expr(base, None);
                let bt = self.resolve(&b.ty);
                match &bt {
                    Ty::Adt(d, a) if *d == self.prog.b.list => {
                        let i = self.expr_coerce(idx, &Ty::Int);
                        mk(TK::Index(Box::new(b), Box::new(i)), a[0].clone())
                    }
                    Ty::Adt(d, a) if *d == self.prog.b.map => {
                        let k = self.expr_coerce(idx, &a[0]);
                        mk(TK::MapGet(Box::new(b), Box::new(k)), Ty::opt(a[1].clone()))
                    }
                    Ty::Adt(d, _) if *d == self.prog.b.bytes => {
                        let i = self.expr_coerce(idx, &Ty::Int);
                        mk(TK::Index(Box::new(b), Box::new(i)), Ty::Int)
                    }
                    Ty::Text => {
                        self.err_help(span, "text can't be indexed", "use `text.slice(from: i, to: i + 1)` or `text.chars()`");
                        mk(TK::Unit, Ty::Err)
                    }
                    Ty::Err => mk(TK::Unit, Ty::Err),
                    t => {
                        let s = self.prog.show(t);
                        self.err(span, format!("can't index a `{}`", s));
                        mk(TK::Unit, Ty::Err)
                    }
                }
            }
            ExprKind::Unary(op, x) => match op {
                UnOp::Not => {
                    let t = self.expr_coerce(x, &Ty::Bool);
                    mk(TK::Unary(UnOp::Not, Box::new(t)), Ty::Bool)
                }
                UnOp::Neg => {
                    let t = self.expr(x, expected);
                    let ty = self.resolve(&t.ty);
                    if !matches!(ty, Ty::Int | Ty::Float | Ty::Err) && !self.is_decimal(&ty) {
                        let s = self.prog.show(&ty);
                        self.err(span, format!("can't negate a `{}`", s));
                    }
                    mk(TK::Unary(UnOp::Neg, Box::new(t)), ty)
                }
            },
            ExprKind::Binary(op, l, r) => {
                let arith_op = matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem);
                let (lt, rt) = if is_literal(l) && !is_literal(r) {
                    let rt = self.expr(r, if arith_op { expected } else { None });
                    let rty = self.resolve(&rt.ty);
                    let lt = self.expr(l, Some(&rty));
                    (lt, rt)
                } else {
                    let arith = matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem);
                    let lt = self.expr(l, if arith { expected } else { None });
                    let lty = self.resolve(&lt.ty);
                    let rt = self.expr(r, Some(&lty));
                    (lt, rt)
                };
                self.binary(*op, lt, rt, span, expected)
            }
            ExprKind::Coalesce(l, r) => {
                let exp_opt = expected.map(|t| Ty::opt(self.resolve(t)));
                let lt = self.expr(l, exp_opt.as_ref());
                let lty = self.resolve(&lt.ty);
                let inner = match &lty {
                    Ty::Opt(i) => (**i).clone(),
                    Ty::Err => Ty::Err,
                    Ty::Var(_) => {
                        let v = self.fresh();
                        self.unify(&lty, &Ty::opt(v.clone()));
                        v
                    }
                    t => {
                        let s = self.prog.show(t);
                        self.err_help(l.span, format!("`??` needs a value that may be missing, but this is a `{}`", s), "remove `?? ...`");
                        t.clone()
                    }
                };
                let rt = self.expr(r, Some(&inner));
                let rty = self.resolve(&rt.ty);
                // `a ?? b` where b is itself optional keeps the optional
                let (rt, ty) = if let Ty::Opt(_) = rty {
                    let rt = self.coerce(rt, &Ty::opt(inner.clone()));
                    (rt, Ty::opt(inner))
                } else {
                    (self.coerce(rt, &inner), inner)
                };
                mk(TK::Coalesce(Box::new(lt), Box::new(rt)), ty)
            }
            ExprKind::Range { .. } => {
                self.err_help(span, "a range can only be used in `for`", "write `for i in 0..<n { ... }`");
                mk(TK::Unit, Ty::Err)
            }
            ExprKind::Lambda { params, body } => self.lambda(params, body, span, expected),
            ExprKind::Block(b) => {
                let tb = self.block(b, true, expected);
                let ty = tb.ty.clone();
                mk(TK::Block(tb), ty)
            }
            ExprKind::If { cond, then, els } => self.if_expr(cond, then, els.as_deref(), span, expected, !stmt_pos),
            ExprKind::Match { scrut, arms } => self.match_expr(scrut, arms, span, expected, !stmt_pos),
            ExprKind::Is(x, pat) => {
                let te = self.expr(x, None);
                let n_before = self.fc().locals.len();
                let ty = te.ty.clone();
                self.fcx().scopes.push(HashMap::new());
                let p = self.pattern(pat, &ty);
                self.fcx().scopes.pop();
                if self.fc().locals.len() != n_before {
                    self.err_help(pat.span(), "a name bound by `is` works only in an `if` condition, alone or joined with `and`", "with `or` (or outside `if`) the name may not be set: test without a name, `x is some(_)`, or write `if x is some(v) { ... }`");
                }
                mk(TK::Is(Box::new(te), p), Ty::Bool)
            }
            ExprKind::Try { expr, catch } => {
                // `try` covers every call in the expression after it
                self.fcx().try_hits.push(false);
                let call = self.expr(expr, expected);
                let hit = self.fcx().try_hits.pop().unwrap_or(false);
                if !hit && call.ty != Ty::Err {
                    self.err_help(span, "nothing here can fail", "remove `try`");
                }
                // `let x: Int? = try t.to_int() catch err { none }`: the value
                // becomes optional, so the catch block can give `none`
                let call = match (expected.map(|e| self.resolve(e)), self.resolve(&call.ty)) {
                    (Some(Ty::Opt(inner)), got) if catch.is_some() && !matches!(got, Ty::Opt(_) | Ty::Err | Ty::Never) && self.try_unify(&got, &inner) => {
                        let want = Ty::Opt(inner);
                        self.coerce(call, &want)
                    }
                    _ => call,
                };
                let ty = call.ty.clone();
                match catch {
                    None => {
                        if hit {
                            self.note_throw(span);
                        }
                        mk(TK::Try { call: Box::new(call), catch: None }, ty)
                    }
                    Some((name, blk)) => {
                        self.fcx().scopes.push(HashMap::new());
                        let et = self.prog.error_ty();
                        let err_local = self.declare(span, name, et, false);
                        let exp = self.resolve(&ty);
                        let b = self.block(blk, exp != Ty::Unit, if exp == Ty::Unit { None } else { Some(&exp) });
                        self.fcx().scopes.pop();
                        let mut b = b;
                        if let Some(t) = b.tail.take() {
                            let t = self.coerce(*t, &exp);
                            b.tail = Some(Box::new(t));
                        }
                        mk(TK::Try { call: Box::new(call), catch: Some((err_local, b)) }, ty)
                    }
                }
            }
            ExprKind::Spawn(x) => {
                if !matches!(x.kind, ExprKind::Call { .. }) {
                    self.err_help(x.span, "`spawn` goes right before a call", "write `spawn f(x)`");
                    return self.expr(x, None);
                }
                if !self.fc().lambdas.is_empty() {
                    // a task belongs to a function; a lambda would carry it away
                    self.err_help(span, "`spawn` can't be used inside a lambda", "for a list, use `try xs.parallel_map(limit: 8, transform: x => try f(x))`; or a loop: `for x in xs { tasks.append(spawn f(x)) }`");
                    return self.expr(x, None);
                }
                self.fcx().in_spawn = true;
                let call = self.expr(x, None);
                let throws = matches!(call.kind, TK::Call { throws: true, .. });
                if matches!(call.kind, TK::MutCall { .. } | TK::IfaceMutCall { .. }) {
                    self.err(span, "a method that changes its value can't be spawned");
                }
                if throws {
                    // an error nobody waits for comes out of the function
                    self.note_throw(span);
                }
                let rt = self.resolve(&call.ty);
                if rt == Ty::Never {
                    self.err(span, "this call never finishes");
                }
                let tty = Ty::Adt(self.prog.b.task, vec![rt]);
                mk(TK::Spawn(Box::new(call)), tty)
            }
            ExprKind::ExpectThrows(x) => {
                if !self.fc().in_test {
                    self.err(span, "`expect throws` is only allowed in tests");
                }
                self.fcx().try_hits.push(false);
                let call = self.expr(x, None);
                let hit = self.fcx().try_hits.pop().unwrap_or(false);
                if !hit && call.ty != Ty::Err {
                    self.err(x.span, "nothing here can fail, so `expect throws` can never pass");
                }
                let et = self.prog.error_ty();
                mk(TK::ExpectThrows(Box::new(call)), et)
            }
            ExprKind::Diverge(s) => {
                let ts = self.stmt(s);
                mk(TK::Diverge(Box::new(ts)), Ty::Never)
            }
        }
    }

    fn ident(&mut self, name: &str, span: Span, expected: Option<&Ty>) -> TExpr {
        let mk = |kind, ty| TExpr { kind, ty, span };
        if let Some(id) = self.lookup_local(name) {
            let ty = self.fc().locals[id].ty.clone();
            return mk(TK::Local(id), ty);
        }
        let md = self.fc().module;
        match self.lookup_global(name, md) {
            Some(Global::Fn(id)) if self.prog.fns[id].is_const => return self.const_use(id, span),
            Some(Global::Fn(id)) => {
                let f = self.prog.fns[id].clone();
                let targs: Vec<Ty> = f.generics.iter().map(|_| self.fresh()).collect();
                let ps = f.params.iter().map(|p| p.1.subst(&targs)).collect();
                let ty = Ty::func(ps, f.ret.subst(&targs), f.throws);
                return mk(TK::FnRef(id, targs), ty);
            }
            Some(Global::Type(d)) => {
                let n = self.prog.defs[d].name.clone();
                self.err_help(span, format!("`{}` is a type, not a value", n), format!("build a value with `{}(...)`", n));
                return mk(TK::Unit, Ty::Err);
            }
            None => {}
        }
        if self.module_named(name).is_some() {
            self.err_help(span, format!("`{}` is a module", name), format!("use something from it: `{}.name`", name));
            return mk(TK::Unit, Ty::Err);
        }
        // an enum variant without fields
        if let Some((d, idx)) = self.find_variant(name, expected, span) {
            let (fields, ng) = match &self.prog.defs[d].kind {
                TypeKind::Enum { variants } => (variants[idx].fields.len(), self.prog.defs[d].generics.len()),
                _ => (0, 0),
            };
            let targs: Vec<Ty> = (0..ng).map(|_| self.fresh()).collect();
            let ty = Ty::Adt(d, targs);
            if let Some(exp) = expected {
                self.unify(&ty, exp);
            }
            if fields > 0 {
                self.err_help(span, format!("`{}` needs its fields", name), format!("write `{}(...)`", name));
            }
            return mk(TK::Variant { def: d, idx, fields: vec![] }, ty);
        }
        let hint = match name {
            "files" | "json" | "http" | "db" | "time" | "env" | "cli" | "log" | "process" | "text" => Some(format!("add `import {}` at the top of the file", name)),
            "null" | "nil" | "None" | "undefined" => Some("use `none`".to_string()),
            "True" | "False" => Some(format!("use `{}`", name.to_lowercase())),
            "this" => Some("use `self`".into()),
            _ => {
                let cands: Vec<String> = self.fc().scopes.iter().flat_map(|s| s.keys().cloned()).collect();
                suggest(name, cands.iter()).map(|s| format!("did you mean `{}`?", s))
            }
        };
        match hint {
            Some(h) => self.err_help(span, format!("unknown name `{}`", name), h),
            None => self.err(span, format!("unknown name `{}`", name)),
        }
        mk(TK::Unit, Ty::Err)
    }

    fn find_variant(&mut self, name: &str, expected: Option<&Ty>, span: Span) -> Option<(DefId, usize)> {
        if let Some(Ty::Adt(d, _)) = expected.map(|t| self.resolve(t)) {
            if let TypeKind::Enum { variants } = &self.prog.defs[d].kind {
                if let Some(i) = variants.iter().position(|v| v.name == name) {
                    return Some((d, i));
                }
            }
        }
        let md = self.fc().module;
        let mut found = vec![];
        for (d, def) in self.prog.defs.iter().enumerate() {
            if def.module != md && def.module != 0 {
                continue;
            }
            if let TypeKind::Enum { variants } = &def.kind {
                if let Some(i) = variants.iter().position(|v| v.name == name) {
                    found.push((d, i));
                }
            }
        }
        match found.len() {
            0 => None,
            1 => Some(found[0]),
            _ => {
                let names: Vec<String> = found.iter().map(|(d, _)| format!("`{}.{}`", self.prog.defs[*d].name, name)).collect();
                self.err_help(span, format!("`{}` is a variant of several enums", name), format!("write {}", names.join(" or ")));
                Some(found[0])
            }
        }
    }

    // `module.Enum` written before a variant name
    fn imported_enum(&mut self, e: &Expr) -> Option<DefId> {
        if let ExprKind::Field(recv, tn, nsp) = &e.kind {
            if let ExprKind::Ident(alias) = &recv.kind {
                if self.lookup_local(alias).is_none() {
                    if let Some(target) = self.module_named(alias) {
                        if let Some(Global::Type(d)) = self.lookup_in_module(target, tn, *nsp) {
                            if matches!(self.prog.defs[d].kind, TypeKind::Enum { .. }) {
                                return Some(d);
                            }
                        }
                    }
                }
            }
        }
        None
    }

    fn field(&mut self, base: &Expr, name: &str, nsp: Span, span: Span) -> TExpr {
        let mk = |kind, ty| TExpr { kind, ty, span };
        if let ExprKind::Ident(alias) = &base.kind {
            if let Some(target) = self.module_named(alias) {
                match self.lookup_in_module(target, name, nsp) {
                    Some(Global::Fn(id)) if self.prog.fns[id].is_const => return self.const_use(id, span),
                    Some(Global::Fn(id)) => {
                        let f = self.prog.fns[id].clone();
                        let targs: Vec<Ty> = f.generics.iter().map(|_| self.fresh()).collect();
                        let ps = f.params.iter().map(|p| p.1.subst(&targs)).collect();
                        let ty = Ty::func(ps, f.ret.subst(&targs), f.throws);
                        return mk(TK::FnRef(id, targs), ty);
                    }
                    Some(Global::Type(d)) => {
                        let n = self.prog.defs[d].name.clone();
                        self.err_help(span, format!("`{}.{}` is a type, not a value", alias, n), format!("build a value with `{}.{}(...)`", alias, n));
                        return mk(TK::Unit, Ty::Err);
                    }
                    None => return mk(TK::Unit, Ty::Err),
                }
            }
        }
        // `json.Value.Null`
        if let Some(d) = self.imported_enum(base) {
            if let TypeKind::Enum { variants } = &self.prog.defs[d].kind {
                if let Some(i) = variants.iter().position(|v| v.name == name) {
                    let ng = self.prog.defs[d].generics.len();
                    let targs: Vec<Ty> = (0..ng).map(|_| self.fresh()).collect();
                    return mk(TK::Variant { def: d, idx: i, fields: vec![] }, Ty::Adt(d, targs));
                }
            }
            let n = self.prog.defs[d].name.clone();
            self.err(nsp, format!("`{}` has no variant `{}`", n, name));
            return mk(TK::Unit, Ty::Err);
        }
        // `Status.Draft`
        if let ExprKind::Ident(tn) = &base.kind {
            if self.lookup_local(tn).is_none() {
                let md = self.fc().module;
                if let Some(Global::Type(d)) = self.lookup_global(tn, md) {
                    if let TypeKind::Enum { variants } = &self.prog.defs[d].kind {
                        if let Some(i) = variants.iter().position(|v| v.name == name) {
                            let ng = self.prog.defs[d].generics.len();
                            let targs: Vec<Ty> = (0..ng).map(|_| self.fresh()).collect();
                            return mk(TK::Variant { def: d, idx: i, fields: vec![] }, Ty::Adt(d, targs));
                        }
                    }
                    let n = self.prog.defs[d].name.clone();
                    self.err(nsp, format!("`{}` has no variant `{}`", n, name));
                    return mk(TK::Unit, Ty::Err);
                }
            }
        }
        let b = self.expr(base, None);
        let bt = self.resolve(&b.ty);
        match &bt {
            Ty::Adt(d, args) => {
                let def = self.prog.defs[*d].clone();
                match &def.kind {
                    TypeKind::Record { fields } => {
                        if let Some(i) = fields.iter().position(|f| f.name == name) {
                            let fty = fields[i].ty.subst(args);
                            return mk(TK::Field(Box::new(b), i), fty);
                        }
                    }
                    TypeKind::Newtype(inner) if name == "value" => {
                        return mk(TK::Unwrap(Box::new(b)), inner.clone());
                    }
                    _ => {}
                }
                if let Some((_, pty)) = def.props.iter().find(|p| p.0 == name) {
                    return mk(TK::Prop(Box::new(b), name.to_string()), pty.subst(args));
                }
                if def.methods.contains_key(name) {
                    self.err_help(nsp, format!("`{}` is a method", name), format!("call it: `.{}()`", name));
                } else {
                    let mut cands: Vec<String> = def.props.iter().map(|p| p.0.clone()).collect();
                    if let TypeKind::Record { fields } = &def.kind {
                        cands.extend(fields.iter().map(|f| f.name.clone()));
                    }
                    let s = self.prog.show(&bt);
                    match suggest(name, cands.iter()) {
                        Some(c) => self.err_help(nsp, format!("`{}` has no field `{}`", s, name), format!("did you mean `{}`?", c)),
                        None => self.err(nsp, format!("`{}` has no field `{}`", s, name)),
                    }
                }
                mk(TK::Unit, Ty::Err)
            }
            Ty::Text => {
                if name == "length" {
                    return mk(TK::Prop(Box::new(b), "length".into()), Ty::Int);
                }
                let tdef = self.prog.b.text;
                if self.prog.defs[tdef].methods.contains_key(name) {
                    self.err_help(nsp, format!("`{}` is a method", name), format!("call it: `.{}()`", name));
                } else {
                    self.err(nsp, format!("`String` has no field `{}`", name));
                }
                mk(TK::Unit, Ty::Err)
            }
            Ty::Opt(_) => {
                self.err_help(nsp, "this value may be missing", format!("unwrap it first: `if x is some(v) {{ v.{} }}`, or give a fallback with `??`", name));
                mk(TK::Unit, Ty::Err)
            }
            Ty::Err => mk(TK::Unit, Ty::Err),
            Ty::Var(_) => {
                self.err_help(nsp, "the type isn't known here yet", "add a type annotation");
                mk(TK::Unit, Ty::Err)
            }
            t => {
                let s = self.prog.show(t);
                self.err(nsp, format!("`{}` has no field `{}`", s, name));
                mk(TK::Unit, Ty::Err)
            }
        }
    }

    // ---- calls ----

    fn check_args(
        &mut self,
        params: &[(String, Ty)],
        args: &[ast::Arg],
        span: Span,
        what: &str,
        named_rule: bool,
    ) -> Vec<TExpr> {
        if args.len() != params.len() {
            let names: Vec<&str> = params.iter().map(|p| p.0.as_str()).collect();
            self.err_help(span, format!("{} takes {} argument(s), but {} were given", what, params.len(), args.len()), format!("parameters: ({})", names.join(", ")));
        }
        // names
        for (i, a) in args.iter().enumerate() {
            if let Some(n) = &a.name {
                match params.get(i) {
                    Some(p) if p.0 == *n => {}
                    Some(p) => {
                        if params.iter().any(|q| q.0 == *n) {
                            self.err_help(a.span, format!("argument `{}` is out of order", n), format!("arguments go in the declared order; here `{}` is expected", p.0));
                        } else {
                            self.err_help(a.span, format!("there is no parameter `{}`", n), format!("the parameter here is `{}`", p.0));
                        }
                    }
                    None => {}
                }
            }
        }
        if named_rule && params.len() >= 3 {
            let missing: Vec<usize> = (1..args.len()).filter(|i| args[*i].name.is_none()).collect();
            if !missing.is_empty() && args.len() == params.len() {
                let fixed: Vec<String> = args
                    .iter()
                    .enumerate()
                    .map(|(i, _)| if i == 0 { "…".to_string() } else { format!("{}: …", params[i].0) })
                    .collect();
                self.err_help(args[missing[0]].span, format!("{} has 3 or more parameters: name every argument after the first", what), format!("write ({})", fixed.join(", ")));
            }
        } else if named_rule {
            for a in args {
                if a.name.is_some() && params.len() < 3 {
                    // allowed but not needed; `lang fmt` would remove it
                }
            }
        }
        // non-lambda arguments first, so lambdas see known types
        let mut out: Vec<Option<TExpr>> = vec![None; args.len()];
        for pass in 0..2 {
            for (i, a) in args.iter().enumerate() {
                let is_lambda = matches!(a.value.kind, ExprKind::Lambda { .. });
                if (pass == 0) == is_lambda {
                    continue;
                }
                let pty = params.get(i).map(|p| p.1.clone()).unwrap_or(Ty::Err);
                out[i] = Some(self.expr_coerce(&a.value, &pty));
            }
        }
        out.into_iter().map(|x| x.unwrap()).collect()
    }

    fn call(&mut self, e: &Expr, callee: &Expr, type_args: &[TypeExpr], args: &[ast::Arg], expected: Option<&Ty>, in_try: bool) -> TExpr {
        let span = e.span;
        let spawned_here = std::mem::replace(&mut self.fcx().spawn_call, false);
        let resource_ok = std::mem::replace(&mut self.fcx().resource_ok, false);
        let generics = self.fc().generics.clone();
        let md = self.fc().module;
        let explicit: Vec<Ty> = type_args.iter().map(|t| self.resolve_texpr(t, &generics, md)).collect();
        let result = match &callee.kind {
            // `files.read(...)`: a function or type from an imported module
            ExprKind::Field(recv, name, nsp) if matches!(&recv.kind, ExprKind::Ident(a) if self.module_named(a).is_some()) => {
                let alias = match &recv.kind {
                    ExprKind::Ident(a) => a.clone(),
                    _ => unreachable!(),
                };
                let target = self.module_named(&alias).unwrap();
                match self.lookup_in_module(target, name, *nsp) {
                    Some(Global::Fn(id)) => self.fn_call(id, None, &explicit, args, span, in_try, expected),
                    Some(Global::Type(d)) => self.ctor(d, &explicit, args, span, expected),
                    None => {
                        for a in args {
                            let _ = self.expr(&a.value, None);
                        }
                        TExpr { kind: TK::Unit, ty: Ty::Err, span }
                    }
                }
            }
            ExprKind::Field(recv, name, nsp) => {
                // `json.Value.String(...)`: a variant of an imported module's enum
                if let Some(d) = self.imported_enum(recv) {
                    if let TypeKind::Enum { variants } = &self.prog.defs[d].kind {
                        if let Some(i) = variants.iter().position(|v| v.name == *name) {
                            return self.variant_ctor(d, i, args, span, expected);
                        }
                    }
                    let n = self.prog.defs[d].name.clone();
                    self.err(*nsp, format!("`{}` has no variant `{}`", n, name));
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                // `Status.Circle(...)`
                if let ExprKind::Ident(tn) = &recv.kind {
                    if self.lookup_local(tn).is_none() {
                        if let Some(Global::Type(d)) = self.lookup_global(tn, md) {
                            if let TypeKind::Enum { variants } = &self.prog.defs[d].kind {
                                if let Some(i) = variants.iter().position(|v| v.name == *name) {
                                    return self.variant_ctor(d, i, args, span, expected);
                                }
                            }
                            let n = self.prog.defs[d].name.clone();
                            self.err_help(*nsp, format!("`{}` has no variant or function `{}`", n, name), "types have no static functions: use a plain function");
                            return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                        }
                    }
                }
                self.method_call(recv, name, *nsp, &explicit, args, span, in_try, expected)
            }
            ExprKind::Ident(name) => {
                if name == "some" && self.lookup_local(name).is_none() {
                    self.err_help(span, "`some(x)` is only a pattern", "a plain value is already accepted where `T?` is expected: write `x`");
                    for a in args {
                        let _ = self.expr(&a.value, None);
                    }
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                if let Some(id) = self.lookup_local(name) {
                    let ty = self.fc().locals[id].ty.clone();
                    let f = TExpr { kind: TK::Local(id), ty, span: callee.span };
                    self.value_call(f, args, span, in_try)
                } else {
                    match self.lookup_global(name, md) {
                        Some(Global::Fn(id)) => self.fn_call(id, None, &explicit, args, span, in_try, expected),
                        Some(Global::Type(d)) => self.ctor(d, &explicit, args, span, expected),
                        None => {
                            if let Some((d, i)) = self.find_variant(name, expected, span) {
                                return self.variant_ctor(d, i, args, span, expected);
                            }
                            let hint = match name.as_str() {
                                "len" => Some("use `xs.length`"),
                                "str" | "String" => Some("use interpolation: `\"${x}\"`"),
                                "int" | "parseInt" | "Int" => Some("use `try text.to_int()`"),
                                "float" | "Float" => Some("use `try text.to_float()` or `n.to_float()`"),
                                "println" | "console" | "puts" | "printf" => Some("use `print(text)`"),
                                "range" => Some("use `for i in 0..<n`"),
                                "sorted" => Some("use `xs.sorted()`"),
                                _ => None,
                            };
                            match hint {
                                Some(h) => self.err_help(callee.span, format!("unknown function `{}`", name), h),
                                None => {
                                    let cands: Vec<String> = self.modules[md].globals.keys().chain(self.modules[0].globals.keys()).cloned().collect();
                                    match suggest(name, cands.iter()) {
                                        Some(s) => self.err_help(callee.span, format!("unknown function `{}`", name), format!("did you mean `{}`?", s)),
                                        None => self.err(callee.span, format!("unknown function `{}`", name)),
                                    }
                                }
                            }
                            for a in args {
                                let _ = self.expr(&a.value, None);
                            }
                            TExpr { kind: TK::Unit, ty: Ty::Err, span }
                        }
                    }
                }
            }
            _ => {
                let f = self.expr(callee, None);
                self.value_call(f, args, span, in_try)
            }
        };
        if !resource_ok {
            let rt = self.resolve(&result.ty);
            if self.resource_close(&rt).is_some() {
                let s = self.show(&rt);
                self.err_help(span, format!("a `{}` must be closed: get it with `with`", s), "write `with f = try ... { ... }`: it's closed at the end of the block, even on an error");
            }
        }
        // `try` rules (a spawned call's errors arrive at `wait`)
        if spawned_here {
            return result;
        }
        let throws = match &result.kind {
            TK::Call { throws, .. } | TK::MutCall { throws, .. } | TK::IfaceMutCall { throws, .. } => *throws,
            _ => false,
        };
        let _ = in_try;
        if throws {
            match self.fcx().try_hits.last_mut() {
                Some(hit) => *hit = true,
                None => self.err_help(span, "this call can fail", "write `try` before it: `try f(x)`, or handle it: `try f(x) catch err { ... }`"),
            }
        }
        result
    }

    fn value_call(&mut self, f: TExpr, args: &[ast::Arg], span: Span, _in_try: bool) -> TExpr {
        let ft = self.resolve(&f.ty);
        let fty = match &ft {
            Ty::Func(ft) => (**ft).clone(),
            Ty::Adt(d, _) => match &self.prog.defs[*d].kind {
                TypeKind::Newtype(Ty::Func(ft)) => {
                    let inner = (**ft).clone();
                    let sp = f.span;
                    let unwrapped = TExpr { kind: TK::Unwrap(Box::new(f)), ty: Ty::Func(Box::new(inner.clone())), span: sp };
                    return self.value_call(unwrapped, args, span, _in_try);
                }
                _ => {
                    let s = self.prog.show(&ft);
                    self.err(span, format!("a `{}` can't be called", s));
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
            },
            Ty::Err => return TExpr { kind: TK::Unit, ty: Ty::Err, span },
            t => {
                let s = self.prog.show(t);
                self.err(span, format!("a `{}` can't be called", s));
                return TExpr { kind: TK::Unit, ty: Ty::Err, span };
            }
        };
        for a in args {
            if a.name.is_some() {
                self.err(a.span, "arguments of a function value are positional");
            }
        }
        let params: Vec<(String, Ty)> = fty.params.iter().enumerate().map(|(i, t)| (format!("#{}", i), t.clone())).collect();
        let targs = self.check_args(&params, args, span, "this function", false);
        TExpr { kind: TK::Call { callee: Callee::Value(Box::new(f)), args: targs, throws: fty.throws }, ty: fty.ret.clone(), span }
    }

    fn instantiate(&mut self, f: &FnDef, owner_args: &[Ty], explicit: &[Ty], span: Span) -> Vec<Ty> {
        let n_own = f.generics.len() - owner_args.len();
        let mut targs: Vec<Ty> = owner_args.to_vec();
        if !explicit.is_empty() {
            if explicit.len() != n_own {
                self.err(span, format!("`{}` takes {} type argument(s)", f.name, n_own));
            }
            for i in 0..n_own {
                targs.push(explicit.get(i).cloned().unwrap_or(Ty::Err));
            }
        } else {
            for _ in 0..n_own {
                let v = self.fresh();
                targs.push(v);
            }
        }
        targs
    }

    fn rethrow_result(&self, f: &FnDef, targs: &[TExpr], throws: bool) -> bool {
        if !f.rethrows {
            return throws;
        }
        targs.iter().any(|a| match self.resolve(&a.ty) {
            Ty::Func(ft) => ft.throws && !matches!(a.kind, TK::Wrap(_)),
            _ => false,
        })
    }

    fn fn_call(&mut self, id: FnId, recv: Option<TExpr>, explicit: &[Ty], args: &[ast::Arg], span: Span, _in_try: bool, expected: Option<&Ty>) -> TExpr {
        if self.prog.fns[id].is_const {
            let n = self.prog.fns[id].name.clone();
            self.err_help(span, format!("`{}` is a value, not a function", n), format!("write `{}` without `()`", n));
            for a in args {
                let _ = self.expr(&a.value, None);
            }
            return TExpr { kind: TK::Unit, ty: Ty::Err, span };
        }
        let f = self.prog.fns[id].clone();
        let targs = self.instantiate(&f, &[], explicit, span);
        let params: Vec<(String, Ty)> = f.params.iter().map(|(n, t)| (n.clone(), t.subst(&targs))).collect();
        let ret = f.ret.subst(&targs);
        if let Some(exp) = expected {
            // let the expected type flow into generic results (`let xs: List<Int> = f()`)
            if ret.has_params() || matches!(self.resolve(&ret), Ty::Var(_)) || f.generics.len() > 0 {
                let r = self.resolve(&ret);
                let e = self.resolve(exp);
                if !matches!(e, Ty::Opt(_) | Ty::Iface(_)) || matches!(r, Ty::Opt(_)) {
                    let _ = self.try_unify(&r, &e);
                }
            }
        }
        let mut targs_e = vec![];
        if let Some(r) = recv {
            targs_e.push(r);
        }
        let what = format!("`{}`", f.name);
        let checked = self.check_args(&params, args, span, &what, true);
        targs_e.extend(checked);
        let throws = self.rethrow_result(&f, &targs_e[if f.self_mode == SelfMode::None { 0 } else { 1 }..], f.throws);
        self.check_constraints(&f, &targs, span);
        TExpr { kind: TK::Call { callee: Callee::Fn(id, targs), args: targs_e, throws }, ty: ret, span }
    }

    fn try_unify(&mut self, a: &Ty, b: &Ty) -> bool {
        // unify without leaving partial bindings on failure
        let saved = self.fc().vars.clone();
        if self.unify(a, b) {
            true
        } else {
            self.fcx().vars = saved;
            false
        }
    }

    fn check_constraints(&mut self, f: &FnDef, targs: &[Ty], span: Span) {
        if !f.is_prelude {
            return;
        }
        let owner = f.owner.map(|d| self.prog.defs[d].name.clone()).unwrap_or_default();
        let t0 = targs.first().map(|t| self.resolve(t));
        let last = targs.last().map(|t| self.resolve(t));
        let need = |c: &mut Checker, t: Option<Ty>, ok: fn(&Ty) -> bool, what: &str| {
            if let Some(t) = t {
                if matches!(t, Ty::Var(_) | Ty::Param(_) | Ty::Err) || c.is_decimal(&t) {
                    return;
                }
                if !ok(&t) {
                    let s = c.prog.show(&t);
                    c.err(span, format!("`{}` needs {}, but this is `{}`", f.name, what, s));
                }
            }
        };
        match (owner.as_str(), f.name.as_str()) {
            ("List", "sum") => need(self, t0, |t| matches!(t, Ty::Int | Ty::Float), "numbers"),
            ("List", "min") | ("List", "max") | ("List", "sort") | ("List", "sorted") => {
                need(self, t0, orderable, "values that can be ordered (numbers or text); for other types use `sort_by(x => x.key)`")
            }
            ("List", "join") => need(self, t0, |t| *t == Ty::Text, "a list of `String`"),
            ("List", "sort_by") | ("List", "sorted_by") | ("List", "min_by") | ("List", "max_by") => {
                need(self, last, orderable, "a key that can be ordered (a number or text)")
            }
            ("", "min") | ("", "max") | ("", "__less") => need(self, t0, orderable, "values that can be ordered (numbers or text)"),
            _ => {}
        }
    }

    // The type of a place expression, with `m[k]` giving the value type (no
    // errors, no captures recorded).
    fn peek_place_ty(&mut self, e: &Expr) -> Option<Ty> {
        match &e.kind {
            ExprKind::Ident(n) => {
                let fc = self.fc();
                for s in fc.scopes.iter().rev() {
                    if let Some(id) = s.get(n) {
                        return Some(self.resolve(&fc.locals[*id].ty));
                    }
                }
                None
            }
            ExprKind::SelfRef => self.fc().self_local.map(|l| self.fc().locals[l].ty.clone()),
            ExprKind::Field(b, name, _) => {
                let bt = self.peek_place_ty(b)?;
                match &bt {
                    Ty::Adt(d, args) => match &self.prog.defs[*d].kind {
                        TypeKind::Record { fields } => fields.iter().find(|f| f.name == *name).map(|f| f.ty.subst(args)),
                        _ => None,
                    },
                    _ => None,
                }
            }
            ExprKind::Index(b, _) => {
                let bt = self.peek_place_ty(b)?;
                match &bt {
                    Ty::Adt(d, a) if *d == self.prog.b.list => Some(a[0].clone()),
                    Ty::Adt(d, a) if *d == self.prog.b.map => Some(a[1].clone()),
                    _ => None,
                }
            }
            _ => None,
        }
    }

    fn is_mutating_on(&self, t: &Ty, name: &str) -> bool {
        let d = match t {
            Ty::Adt(d, _) => *d,
            Ty::Int => self.prog.b.int,
            Ty::Float => self.prog.b.float,
            Ty::Text => self.prog.b.text,
            Ty::Iface(ids) => {
                return ids.iter().any(|i| self.prog.defs[*i].methods.get(name).map(|m| self.prog.fns[*m].self_mode == SelfMode::Mutating).unwrap_or(false));
            }
            _ => return false,
        };
        self.prog.defs[d].methods.get(name).map(|m| self.prog.fns[*m].self_mode == SelfMode::Mutating).unwrap_or(false)
    }

    fn method_call(&mut self, recv: &Expr, name: &str, nsp: Span, explicit: &[Ty], args: &[ast::Arg], span: Span, in_try: bool, expected: Option<&Ty>) -> TExpr {
        // a mutating method on a path through maps: check it as a place
        if matches!(recv.kind, ExprKind::Field(..) | ExprKind::Index(..)) {
            if let Some(pt) = self.peek_place_ty(recv) {
                if self.is_mutating_on(&pt, name) {
                    let fake = TExpr { kind: TK::Unit, ty: pt, span: recv.span };
                    return self.method_call_typed(fake, recv, name, nsp, explicit, args, span, in_try, expected);
                }
            }
        }
        let r = self.expr(recv, None);
        self.method_call_typed(r, recv, name, nsp, explicit, args, span, in_try, expected)
    }

    fn method_call_typed(&mut self, r: TExpr, recv: &Expr, name: &str, nsp: Span, explicit: &[Ty], args: &[ast::Arg], span: Span, in_try: bool, expected: Option<&Ty>) -> TExpr {
        let mut rt = self.resolve(&r.ty);
        // `m[k].append(x)`: a mutating call through a map key sees the value type
        let mut via_map_key = false;
        if let (TK::MapGet(..), Ty::Opt(inner)) = (&r.kind, &rt) {
            let inner = (**inner).clone();
            let def = match &inner {
                Ty::Adt(d, _) => Some(*d),
                Ty::Int => Some(self.prog.b.int),
                Ty::Float => Some(self.prog.b.float),
                Ty::Text => Some(self.prog.b.text),
                _ => None,
            };
            if let Some(d) = def {
                if let Some(m) = self.prog.defs[d].methods.get(name) {
                    if self.prog.fns[*m].self_mode == SelfMode::Mutating {
                        rt = inner;
                        via_map_key = true;
                    }
                }
            }
        }
        let _ = via_map_key;
        let err = |c: &mut Checker, msg: String, help: Option<String>| {
            match help {
                Some(h) => c.err_help(nsp, msg, h),
                None => c.err(nsp, msg),
            }
            for a in args {
                let _ = c.expr(&a.value, None);
            }
            TExpr { kind: TK::Unit, ty: Ty::Err, span }
        };
        let (def, owner_args) = match &rt {
            Ty::Adt(d, a) => (*d, a.clone()),
            Ty::Int => (self.prog.b.int, vec![]),
            Ty::Float => (self.prog.b.float, vec![]),
            Ty::Bool => (self.prog.b.bool_, vec![]),
            Ty::Text => (self.prog.b.text, vec![]),
            Ty::Iface(ids) => {
                let ids = ids.clone();
                return self.iface_call(r, &ids, name, nsp, args, span);
            }
            Ty::Opt(_) => {
                return err(self, "this value may be missing".into(), Some(format!("unwrap it first: `if x is some(v) {{ v.{}(...) }}`, or use `??`", name)));
            }
            Ty::Err => {
                for a in args {
                    let _ = self.expr(&a.value, None);
                }
                return TExpr { kind: TK::Unit, ty: Ty::Err, span };
            }
            Ty::Var(_) => return err(self, "the type isn't known here yet".into(), Some("add a type annotation".into())),
            Ty::Param(_) => return err(self, format!("can't call `{}` on a generic value", name), Some("generic values can only be compared, used as keys and put into text".into())),
            t => {
                let s = self.prog.show(t);
                return err(self, format!("a `{}` has no methods", s), None);
            }
        };
        let id = match self.prog.defs[def].methods.get(name).copied() {
            Some(id) => id,
            None => {
                let d = self.prog.defs[def].clone();
                let s = self.prog.show(&rt);
                // a field holding a function: `rule.check(x)` calls it
                if let TypeKind::Record { fields } = &d.kind {
                    if let Some(i) = fields.iter().position(|f| f.name == name) {
                        let fty = fields[i].ty.subst(&owner_args);
                        let callable = match self.resolve(&fty) {
                            Ty::Func(_) => true,
                            Ty::Adt(nd, _) => matches!(self.prog.defs[nd].kind, TypeKind::Newtype(Ty::Func(_))),
                            _ => false,
                        };
                        if callable {
                            let field = TExpr { kind: TK::Field(Box::new(r), i), ty: fty, span };
                            return self.value_call(field, args, span, false);
                        }
                    }
                }
                let cands: Vec<String> = d.methods.keys().cloned().collect();
                let help = suggest(name, cands.iter()).map(|c| format!("did you mean `{}`?", c));
                return err(self, format!("`{}` has no method `{}`", s, name), help);
            }
        };
        let f = self.prog.fns[id].clone();
        let md = self.fc().module;
        if f.module != md && !f.is_pub {
            let tn = self.prog.defs[def].name.clone();
            self.err_help(nsp, format!("`{}.{}` is private to its module", tn, name), "mark it `pub fn` in its module to use it here");
        }
        let n_owner = self.prog.defs[def].generics.len();
        let targs = self.instantiate(&f, &owner_args[..n_owner.min(owner_args.len())], explicit, span);
        let params: Vec<(String, Ty)> = f.params.iter().map(|(n, t)| (n.clone(), t.subst(&targs))).collect();
        let ret = f.ret.subst(&targs);
        if let Some(exp) = expected {
            if f.generics.len() > n_owner {
                let (r2, e2) = (self.resolve(&ret), self.resolve(exp));
                let _ = self.try_unify(&r2, &e2);
            }
        }
        let what = format!("`{}`", f.name);
        let checked = self.check_args(&params, args, span, &what, true);
        let throws = self.rethrow_result(&f, &checked, f.throws);
        self.check_constraints(&f, &targs, span);
        if f.self_mode == SelfMode::Mutating {
            let place = match self.place(recv, true) {
                Some(p) => p,
                None => return TExpr { kind: TK::Unit, ty: Ty::Err, span },
            };
            let _ = in_try;
            return TExpr { kind: TK::MutCall { fn_id: id, type_args: targs, place, args: checked, throws }, ty: ret, span };
        }
        let mut all = vec![r];
        all.extend(checked);
        TExpr { kind: TK::Call { callee: Callee::Fn(id, targs), args: all, throws }, ty: ret, span }
    }

    fn iface_call(&mut self, r: TExpr, ids: &[DefId], name: &str, nsp: Span, args: &[ast::Arg], span: Span) -> TExpr {
        let mut found = None;
        for i in ids {
            if let Some(m) = self.prog.defs[*i].methods.get(name) {
                found = Some((*i, *m));
            }
        }
        let (iface, mid) = match found {
            Some(x) => x,
            None => {
                let s = self.prog.show(&Ty::Iface(ids.to_vec()));
                self.err(nsp, format!("`{}` has no method `{}`", s, name));
                return TExpr { kind: TK::Unit, ty: Ty::Err, span };
            }
        };
        let f = self.prog.fns[mid].clone();
        let what = format!("`{}`", f.name);
        let checked = self.check_args(&f.params, args, span, &what, true);
        if f.self_mode == SelfMode::Mutating {
            // need a place: re-derive from the receiver expression is not
            // possible here; interface mutation goes through a variable
            if let TK::Local(l) = r.kind {
                if !self.fc().locals[l].mutable {
                    let n = self.fc().locals[l].name.clone();
                    self.err_help(r.span, format!("can't change `{}`: it's declared with `let`", n), format!("declare it with `var {} = ...`", n));
                }
                let place = TPlace { root: l, path: vec![], ty: r.ty.clone() };
                return TExpr { kind: TK::IfaceMutCall { iface, method: mid, place, args: checked, throws: f.throws }, ty: f.ret.clone(), span };
            }
            self.err(r.span, "a mutating method needs a variable here");
            return TExpr { kind: TK::Unit, ty: Ty::Err, span };
        }
        let mut all = vec![r];
        all.extend(checked);
        TExpr { kind: TK::Call { callee: Callee::Iface { iface, method: mid }, args: all, throws: f.throws }, ty: f.ret.clone(), span }
    }

    fn ctor(&mut self, d: DefId, explicit: &[Ty], args: &[ast::Arg], span: Span, expected: Option<&Ty>) -> TExpr {
        let def = self.prog.defs[d].clone();
        let targs: Vec<Ty> = if !explicit.is_empty() {
            if explicit.len() != def.generics.len() {
                self.err(span, format!("`{}` takes {} type argument(s)", def.name, def.generics.len()));
            }
            (0..def.generics.len()).map(|i| explicit.get(i).cloned().unwrap_or(Ty::Err)).collect()
        } else {
            let v: Vec<Ty> = def.generics.iter().map(|_| self.fresh()).collect();
            if let Some(exp) = expected {
                let e = self.resolve(exp);
                let e = match e {
                    Ty::Opt(i) => *i,
                    t => t,
                };
                let _ = self.try_unify(&Ty::Adt(d, v.clone()), &e);
            }
            v
        };
        let ty = Ty::Adt(d, targs.clone());
        match &def.kind {
            TypeKind::Record { fields } => {
                let mut out: Vec<Option<TExpr>> = vec![None; fields.len()];
                for a in args {
                    let name = match &a.name {
                        Some(n) => n.clone(),
                        None => {
                            let hint = fields.iter().map(|f| format!("{}: …", f.name)).collect::<Vec<_>>().join(", ");
                            self.err_help(a.span, format!("fields of `{}` are passed by name", def.name), format!("write `{}({})`", def.name, hint));
                            let _ = self.expr(&a.value, None);
                            continue;
                        }
                    };
                    match fields.iter().position(|f| f.name == name) {
                        Some(i) => {
                            if out[i].is_some() {
                                self.err(a.span, format!("field `{}` is given twice", name));
                            }
                            let fty = fields[i].ty.subst(&targs);
                            // a resource's fields may take new resources: it owns and closes them
                            let owner_closes = self.resource_close(&Ty::Adt(d, targs.clone())).is_some();
                            let rf = self.resolve(&fty);
                            if owner_closes && self.resource_close(&rf).is_some() {
                                self.fcx().with_value = true;
                            }
                            out[i] = Some(self.expr_coerce(&a.value, &fty));
                            self.fcx().with_value = false;
                        }
                        None => {
                            let cands: Vec<String> = fields.iter().map(|f| f.name.clone()).collect();
                            match suggest(&name, cands.iter()) {
                                Some(c) => self.err_help(a.span, format!("`{}` has no field `{}`", def.name, name), format!("did you mean `{}`?", c)),
                                None => self.err(a.span, format!("`{}` has no field `{}`", def.name, name)),
                            }
                            let _ = self.expr(&a.value, None);
                        }
                    }
                }
                let mut fs = vec![];
                let mut missing = vec![];
                for (i, f) in fields.iter().enumerate() {
                    match out[i].take() {
                        Some(e) => fs.push(e),
                        None => match &f.default {
                            Some(dv) => {
                                let mut dv = dv.clone();
                                dv.ty = dv.ty.subst(&targs);
                                fs.push(dv);
                            }
                            None => {
                                missing.push(f.name.clone());
                                fs.push(TExpr { kind: TK::Unit, ty: Ty::Err, span });
                            }
                        },
                    }
                }
                if !missing.is_empty() {
                    self.err(span, format!("missing field(s) of `{}`: {}", def.name, missing.join(", ")));
                }
                TExpr { kind: TK::Record { def: d, fields: fs }, ty, span }
            }
            TypeKind::Newtype(inner) => {
                if self.sql_def(d, "Query") && self.prog.module_names.get(self.fc().module).map(|m| m != "sql").unwrap_or(true) {
                    self.err_help(span, "SQL can't be made from text at run time", "write the SQL right in the call, and put values in as parameters");
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                if args.len() != 1 || args[0].name.is_some() {
                    self.err_help(span, format!("`{}` wraps one value", def.name), format!("write `{}(x)`", def.name));
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                let v = self.expr_coerce(&args[0].value, inner);
                TExpr { kind: TK::Wrap(Box::new(v)), ty, span }
            }
            TypeKind::Builtin if d == self.prog.b.bytes => {
                if args.len() > 1 || args.iter().any(|a| a.name.is_some()) {
                    self.err_help(span, "`Bytes` takes nothing or a list of numbers", "write `Bytes()` or `Bytes([104, 105])`");
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                let fields = match args.first() {
                    Some(a) => {
                        let lt = self.prog.list_of(Ty::Int);
                        vec![self.expr_coerce(&a.value, &lt)]
                    }
                    None => vec![],
                };
                TExpr { kind: TK::Record { def: d, fields }, ty, span }
            }
            TypeKind::Builtin if d == self.prog.b.shared => {
                if args.len() != 1 || args[0].name.is_some() {
                    self.err_help(span, "`Shared` wraps one value", "write `Shared(value)`");
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                let v = self.expr_coerce(&args[0].value, &targs[0]);
                TExpr { kind: TK::Record { def: d, fields: vec![v] }, ty, span }
            }
            TypeKind::Builtin if d == self.prog.b.channel => {
                if args.len() != 1 || args[0].name.as_deref() != Some("capacity") {
                    self.err_help(span, "a channel needs a capacity", "write `Channel<T>(capacity: 100)`");
                    return TExpr { kind: TK::Unit, ty: Ty::Err, span };
                }
                let v = self.expr_coerce(&args[0].value, &Ty::Int);
                TExpr { kind: TK::Record { def: d, fields: vec![v] }, ty, span }
            }
            TypeKind::Builtin if d == self.prog.b.set || d == self.prog.b.map || d == self.prog.b.list => {
                if !args.is_empty() {
                    self.err(span, format!("`{}<...>()` takes no arguments", def.name));
                }
                let kind = if d == self.prog.b.set {
                    TK::EmptySet
                } else if d == self.prog.b.map {
                    TK::Map(vec![])
                } else {
                    TK::List(vec![])
                };
                TExpr { kind, ty, span }
            }
            TypeKind::Interface { .. } => {
                self.err(span, format!("`{}` is an interface: build a value of a type that implements it", def.name));
                TExpr { kind: TK::Unit, ty: Ty::Err, span }
            }
            TypeKind::Enum { .. } => {
                self.err_help(span, format!("`{}` is an enum", def.name), format!("build one of its variants: `{}.Variant(...)`", def.name));
                TExpr { kind: TK::Unit, ty: Ty::Err, span }
            }
            _ => {
                self.err(span, format!("`{}` can't be built this way", def.name));
                TExpr { kind: TK::Unit, ty: Ty::Err, span }
            }
        }
    }

    fn variant_ctor(&mut self, d: DefId, idx: usize, args: &[ast::Arg], span: Span, expected: Option<&Ty>) -> TExpr {
        let def = self.prog.defs[d].clone();
        let v = match &def.kind {
            TypeKind::Enum { variants } => variants[idx].clone(),
            _ => unreachable!(),
        };
        let targs: Vec<Ty> = def.generics.iter().map(|_| self.fresh()).collect();
        if let Some(exp) = expected {
            let _ = self.try_unify(&Ty::Adt(d, targs.clone()), exp);
        }
        let mut out = vec![];
        if args.len() != v.fields.len() {
            let hint = v.fields.iter().map(|f| format!("{}: …", f.name)).collect::<Vec<_>>().join(", ");
            self.err_help(span, format!("`{}` has {} field(s)", v.name, v.fields.len()), format!("write `{}({})`", v.name, hint));
        }
        for (i, a) in args.iter().enumerate() {
            let f = match v.fields.get(i) {
                Some(f) => f,
                None => {
                    let _ = self.expr(&a.value, None);
                    continue;
                }
            };
            match &a.name {
                Some(n) if *n == f.name => {}
                Some(n) => self.err_help(a.span, format!("expected field `{}`, found `{}`", f.name, n), "fields go in the declared order"),
                None => self.err_help(a.span, format!("fields of `{}` are passed by name", v.name), format!("write `{}: …`", f.name)),
            }
            let fty = f.ty.subst(&targs);
            out.push(self.expr_coerce(&a.value, &fty));
        }
        TExpr { kind: TK::Variant { def: d, idx, fields: out }, ty: Ty::Adt(d, targs), span }
    }

    // ---- operators ----

    fn binary(&mut self, op: BinOp, l: TExpr, r: TExpr, span: Span, _expected: Option<&Ty>) -> TExpr {
        let lt = self.resolve(&l.ty);
        let rt = self.resolve(&r.ty);
        let mk = |kind, ty| TExpr { kind, ty, span };
        let bad = |c: &mut Checker, msg: String, help: Option<&str>| {
            match help {
                Some(h) => c.err_help(span, msg, h),
                None => c.err(span, msg),
            }
            TExpr { kind: TK::Unit, ty: Ty::Err, span }
        };
        if lt == Ty::Err || rt == Ty::Err {
            return mk(TK::Binary(op, Box::new(l), Box::new(r)), if matches!(op, BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem) { Ty::Err } else { Ty::Bool });
        }
        match op {
            BinOp::And | BinOp::Or => {
                let l = self.coerce(l, &Ty::Bool);
                let r = self.coerce(r, &Ty::Bool);
                mk(TK::Binary(op, Box::new(l), Box::new(r)), Ty::Bool)
            }
            BinOp::Add | BinOp::Sub | BinOp::Mul | BinOp::Div | BinOp::Rem => {
                if lt == Ty::Text || rt == Ty::Text {
                    return bad(self, "there is no `+` on text".into(), Some("use interpolation: `\"${a}${b}\"`, or `parts.join(\"\")`"));
                }
                if matches!(lt, Ty::Adt(d, _) if d == self.prog.b.list) {
                    return bad(self, "there is no `+` on lists".into(), Some("use `a.concat(b)`, or `xs.append_all(ys)` on a `var`"));
                }
                if self.is_decimal(&lt) || self.is_decimal(&rt) {
                    if !(self.is_decimal(&lt) && self.is_decimal(&rt)) {
                        let (a, b) = (self.prog.show(&lt), self.prog.show(&rt));
                        return bad(self, format!("can't combine `{}` and `{}`", a, b), Some("convert the other number: `n.to_decimal()`"));
                    }
                    if op == BinOp::Div {
                        return bad(self, "`/` on `Decimal`s is not allowed: say how many digits to keep".into(), Some("write `a.div(b, places: 2)`"));
                    }
                    if op == BinOp::Rem {
                        return bad(self, "`%` works on `Int` only".into(), None);
                    }
                    let t = lt.clone();
                    return mk(TK::Binary(op, Box::new(l), Box::new(r)), t);
                }
                if !matches!(lt, Ty::Int | Ty::Float | Ty::Var(_)) {
                    let s = self.prog.show(&lt);
                    return bad(self, format!("arithmetic needs numbers, but this is `{}`", s), None);
                }
                if !self.try_unify(&lt, &rt) {
                    let (a, b) = (self.prog.show(&lt), self.prog.show(&rt));
                    let help = if (lt == Ty::Int && rt == Ty::Float) || (lt == Ty::Float && rt == Ty::Int) {
                        Some("numbers don't convert on their own: use `n.to_float()`")
                    } else {
                        None
                    };
                    return bad(self, format!("can't combine `{}` and `{}`", a, b), help);
                }
                let t = self.resolve(&lt);
                if op == BinOp::Div && t == Ty::Int {
                    return bad(self, "`/` on two `Int`s is not allowed".into(), Some("write `a.div(b)` for whole-number division, or `a.to_float() / b.to_float()`"));
                }
                if op == BinOp::Rem && t != Ty::Int {
                    return bad(self, "`%` works on `Int` only".into(), None);
                }
                mk(TK::Binary(op, Box::new(l), Box::new(r)), t)
            }
            BinOp::Eq | BinOp::Ne => {
                // T? == T: wrap the plain side
                let (l, r) = match (&lt, &rt) {
                    (Ty::Opt(_), Ty::Opt(_)) => (l, r),
                    (Ty::Opt(_), _) => {
                        let lt2 = lt.clone();
                        let r = self.coerce(r, &lt2);
                        (l, r)
                    }
                    (_, Ty::Opt(_)) => {
                        let rt2 = rt.clone();
                        let l = self.coerce(l, &rt2);
                        (l, r)
                    }
                    _ => {
                        let r = self.coerce(r, &lt);
                        (l, r)
                    }
                };
                let t = self.resolve(&l.ty);
                if contains_fn(&t, &self.prog) {
                    return bad(self, "functions can't be compared".into(), None);
                }
                mk(TK::Binary(op, Box::new(l), Box::new(r)), Ty::Bool)
            }
            BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => {
                if !self.try_unify(&lt, &rt) {
                    let (a, b) = (self.prog.show(&lt), self.prog.show(&rt));
                    return bad(self, format!("can't compare `{}` and `{}`", a, b), None);
                }
                let t = self.resolve(&lt);
                if !orderable(&t) && !matches!(t, Ty::Var(_)) && !self.is_decimal(&t) {
                    let s = self.prog.show(&t);
                    let help = if matches!(t, Ty::Opt(_)) { Some("the value may be missing: unwrap it first") } else { Some("only numbers and text can be ordered; compare a field instead") };
                    return bad(self, format!("`{}` values can't be ordered", s), help);
                }
                mk(TK::Binary(op, Box::new(l), Box::new(r)), Ty::Bool)
            }
        }
    }

    // ---- lambdas ----

    fn lambda(&mut self, params: &[(String, Span)], body: &Expr, span: Span, expected: Option<&Ty>) -> TExpr {
        let exp = expected.map(|t| self.resolve(t));
        // a named function type wraps the lambda
        if let Some(Ty::Adt(d, _)) = &exp {
            if let TypeKind::Newtype(inner @ Ty::Func(_)) = self.prog.defs[*d].kind.clone() {
                let l = self.lambda(params, body, span, Some(&inner));
                let l = self.coerce(l, &inner);
                return TExpr { kind: TK::Wrap(Box::new(l)), ty: exp.clone().unwrap(), span };
            }
        }
        let (ptys, ret, throws_allowed) = match &exp {
            Some(Ty::Func(f)) => {
                if f.params.len() != params.len() {
                    self.err(span, format!("this lambda takes {} parameter(s), but {} are expected here", params.len(), f.params.len()));
                }
                let ps: Vec<Ty> = (0..params.len()).map(|i| f.params.get(i).cloned().unwrap_or(Ty::Err)).collect();
                (ps, f.ret.clone(), f.throws)
            }
            _ => {
                let ps: Vec<Ty> = params.iter().map(|_| self.fresh()).collect();
                let r = self.fresh();
                (ps, r, true)
            }
        };
        self.fcx().scopes.push(HashMap::new());
        let level = self.fc().lambdas.len() + 1;
        self.fcx().lambdas.push(LambdaCtx { level, captures: vec![], throws: false, ret: ret.clone() });
        let mut pids = vec![];
        for (i, (n, sp)) in params.iter().enumerate() {
            pids.push(self.declare(*sp, n, ptys[i].clone(), false));
        }
        let saved_loops = std::mem::take(&mut self.fcx().loops);
        let saved_hits = std::mem::take(&mut self.fcx().try_hits);
        let b = match &body.kind {
            ExprKind::Block(blk) => {
                let tb = self.block(blk, true, Some(&ret));
                let ty = tb.ty.clone();
                TExpr { kind: TK::Block(tb), ty, span: body.span }
            }
            _ => self.expr(body, Some(&ret)),
        };
        self.fcx().loops = saved_loops;
        self.fcx().try_hits = saved_hits;
        let lc = self.fcx().lambdas.pop().unwrap();
        self.fcx().scopes.pop();
        let rty = self.resolve(&ret);
        let _ = rty;
        let b = self.coerce(b, &ret);
        if lc.throws && !throws_allowed {
            self.err_help(span, "this lambda uses `try`, but a function that can't fail is expected here", "handle the error inside: `try f(x) catch err { fallback }`");
        }
        // captures that belong to this lambda (declared outside of it)
        let captures: Vec<LocalId> = lc.captures.iter().copied().filter(|c| self.fc().level[*c] < level).collect();
        for c in &captures {
            let ct = self.resolve(&self.fc().locals[*c].ty);
            if self.contains_task(&ct) {
                let n = self.fc().locals[*c].name.clone();
                self.err_help(span, format!("a lambda can't capture the task `{}`", n), "a task belongs to the function that started it: wait for it there");
            }
        }
        let fty = Ty::func(ptys, ret, lc.throws);
        TExpr { kind: TK::Lambda { params: pids, captures, body: Box::new(b) }, ty: fty, span }
    }

    // ---- if / match ----

    fn if_expr(&mut self, cond: &Expr, then: &ast::Block, els: Option<&Expr>, span: Span, expected: Option<&Ty>, want: bool) -> TExpr {
        // `if ready and x is some(v) { }`: test `ready`, then `x is some(v)`;
        // the else part is reached from either test
        if let ExprKind::Binary(BinOp::And, l, r) = &cond.kind {
            if matches!(&r.kind, ExprKind::Is(_, p) if pattern_binds(p)) && !matches!(l.kind, ExprKind::Is(..)) {
                let c = self.expr_coerce(l, &Ty::Bool);
                let inner = self.if_expr(r, then, els, span, expected, want);
                let ty = inner.ty.clone();
                let els_e = els.map(|e| {
                    let e2 = self.expr(e, expected);
                    Box::new(self.coerce_branch(e2, &ty))
                });
                let tb = TBlock { stmts: vec![], tail: Some(Box::new(inner)), ty: ty.clone() };
                return TExpr { kind: TK::If { cond: Box::new(c), then: tb, els: els_e }, ty, span };
            }
        }
        // `if x is P { } else { }` and `if x is P and c { }` become a match
        let (is_part, guard) = match &cond.kind {
            ExprKind::Is(..) => (Some(cond), None),
            ExprKind::Binary(BinOp::And, l, r) if matches!(l.kind, ExprKind::Is(..)) => (Some(&**l), Some(&**r)),
            _ => (None, None),
        };
        let want = want && !(els.is_none() && expected.is_none());
        if let Some(Expr { kind: ExprKind::Is(scrut, pat), .. }) = is_part {
            if pattern_binds(pat) || guard.is_some() || is_type_test(pat) {
                let s = self.expr(scrut, None);
                let sty = s.ty.clone();
                self.fcx().scopes.push(HashMap::new());
                let p = self.pattern_narrow(pat, &sty, scrut);
                let g = guard.map(|g| self.expr_coerce(g, &Ty::Bool));
                let tb = self.block(then, want, expected);
                self.fcx().scopes.pop();
                let then_e = TExpr { ty: tb.ty.clone(), kind: TK::Block(tb), span: then.span };
                if !want {
                    self.fcx().stmt_pos = true;
                }
                let els_e = match els {
                    Some(e) => self.expr(e, expected),
                    None => TExpr { kind: TK::Unit, ty: Ty::Unit, span },
                };
                self.fcx().stmt_pos = false;
                let ty = self.join_types(&then_e, &els_e, els.is_some(), want, span, expected);
                let then_e = self.coerce_branch(then_e, &ty);
                let els_e = self.coerce_branch(els_e, &ty);
                let arms = vec![
                    TArm { pat: p, guard: g, body: then_e },
                    TArm { pat: TPat::Wild, guard: None, body: els_e },
                ];
                return TExpr { kind: TK::Match { scrut: Box::new(s), arms }, ty, span };
            }
        }
        let c = self.expr_coerce(cond, &Ty::Bool);
        let tb = self.block(then, want, expected);
        let then_ty = tb.ty.clone();
        if !want {
            self.fcx().stmt_pos = true;
        }
        let els_e = els.map(|e| self.expr(e, expected));
        self.fcx().stmt_pos = false;
        let then_e = TExpr { kind: TK::Unit, ty: then_ty, span: then.span };
        let unit = TExpr { kind: TK::Unit, ty: Ty::Unit, span };
        let ty = self.join_types(&then_e, els_e.as_ref().unwrap_or(&unit), els.is_some(), want, span, expected);
        let mut tb = tb;
        if let Some(t) = tb.tail.take() {
            tb.tail = Some(Box::new(self.coerce_branch(*t, &ty)));
        }
        let els_e = els_e.map(|e| Box::new(self.coerce_branch(e, &ty)));
        TExpr { kind: TK::If { cond: Box::new(c), then: tb, els: els_e }, ty, span }
    }

    fn coerce_branch(&mut self, e: TExpr, ty: &Ty) -> TExpr {
        if *ty == Ty::Unit || e.ty == Ty::Never {
            return e;
        }
        self.coerce(e, ty)
    }

    fn join_types(&mut self, a: &TExpr, b: &TExpr, has_else: bool, want: bool, span: Span, expected: Option<&Ty>) -> Ty {
        if !want {
            return Ty::Unit;
        }
        if !has_else {
            if a.ty != Ty::Unit && a.ty != Ty::Never {
                self.err_help(span, "an `if` used as a value needs an `else`", "add `else { ... }`");
            }
            return Ty::Unit;
        }
        let (at, bt) = (self.resolve(&a.ty), self.resolve(&b.ty));
        if at == Ty::Never {
            return expected.map(|t| self.resolve(t)).filter(|t| !matches!(t, Ty::Var(_))).unwrap_or(bt);
        }
        if bt == Ty::Never {
            return expected.map(|t| self.resolve(t)).filter(|t| !matches!(t, Ty::Var(_))).unwrap_or(at);
        }
        if let Some(e) = expected {
            let e = self.resolve(e);
            if !matches!(e, Ty::Var(_)) {
                return e;
            }
        }
        if !self.try_unify(&at, &bt) {
            // T and T? join to T?
            match (&at, &bt) {
                (Ty::Opt(x), y) | (y, Ty::Opt(x)) if **x == *y => return Ty::opt(y.clone()),
                _ => {}
            }
            let (x, y) = (self.prog.show(&at), self.prog.show(&bt));
            self.err(span, format!("the branches have different types: `{}` and `{}`", x, y));
            return Ty::Err;
        }
        self.resolve(&at)
    }

    fn match_expr(&mut self, scrut: &Expr, arms: &[ast::MatchArm], span: Span, expected: Option<&Ty>, want: bool) -> TExpr {
        let s = self.expr(scrut, None);
        let sty = self.resolve(&s.ty);
        let mut tarms = vec![];
        let mut result: Option<Ty> = expected.map(|t| self.resolve(t)).filter(|t| !matches!(t, Ty::Var(_)));
        for a in arms {
            self.fcx().scopes.push(HashMap::new());
            let p = self.pattern(&a.pat, &sty);
            let g = a.guard.as_ref().map(|g| self.expr_coerce(g, &Ty::Bool));
            if !want {
                self.fcx().stmt_pos = true;
            }
            let body = self.expr(&a.body, if want { result.as_ref().or(expected) } else { None });
            self.fcx().stmt_pos = false;
            self.fcx().scopes.pop();
            let bt = self.resolve(&body.ty);
            if want && bt != Ty::Never {
                match &result {
                    None => result = Some(bt),
                    Some(r) => {
                        let r = r.clone();
                        if !self.try_unify(&r, &bt) {
                            match (&r, &bt) {
                                (Ty::Opt(x), y) if **x == *y => {}
                                (y, Ty::Opt(x)) if **x == *y => result = Some(Ty::opt(y.clone())),
                                (Ty::Iface(_), _) => {}
                                _ => {
                                    let (x, y) = (self.prog.show(&r), self.prog.show(&bt));
                                    self.err(a.body.span, format!("the arms have different types: `{}` and `{}`", x, y));
                                }
                            }
                        }
                    }
                }
            }
            tarms.push(TArm { pat: p, guard: g, body });
        }
        let ty = if want { result.unwrap_or(Ty::Never) } else { Ty::Unit };
        if want {
            for a in tarms.iter_mut() {
                let b = std::mem::replace(&mut a.body, TExpr { kind: TK::Unit, ty: Ty::Unit, span });
                a.body = self.coerce_branch(b, &ty);
            }
        }
        self.exhaustive(&sty, &tarms, span);
        let ty = if !want && tarms.iter().all(|a| a.body.ty == Ty::Never) && !tarms.is_empty() { Ty::Never } else { ty };
        TExpr { kind: TK::Match { scrut: Box::new(s), arms: tarms }, ty, span }
    }

    fn exhaustive(&mut self, ty: &Ty, arms: &[TArm], span: Span) {
        // `a | b => x` covers what `a => x` and `b => x` would
        if arms.iter().any(|a| matches!(a.pat, TPat::Or(_))) {
            let mut flat = vec![];
            for a in arms {
                match &a.pat {
                    TPat::Or(alts) => {
                        for p in alts {
                            flat.push(TArm { pat: p.clone(), guard: a.guard.clone(), body: a.body.clone() });
                        }
                    }
                    _ => flat.push(a.clone()),
                }
            }
            return self.exhaustive(ty, &flat, span);
        }
        let total = |p: &TPat| matches!(p, TPat::Wild | TPat::Bind(_)) || matches!(p, TPat::Record { args } if args.iter().all(|a| matches!(a, TPat::Wild | TPat::Bind(_))));
        if arms.iter().any(|a| a.guard.is_none() && total(&a.pat)) {
            return;
        }
        match ty {
            Ty::Adt(d, _) => {
                if let TypeKind::Enum { variants } = &self.prog.defs[*d].kind {
                    let mut covered = vec![false; variants.len()];
                    for a in arms {
                        if a.guard.is_some() {
                            continue;
                        }
                        if let TPat::Variant { idx, args } = &a.pat {
                            if args.iter().all(|x| matches!(x, TPat::Wild | TPat::Bind(_))) {
                                covered[*idx] = true;
                            }
                        }
                    }
                    let missing: Vec<String> = variants.iter().zip(&covered).filter(|(_, c)| !**c).map(|(v, _)| v.name.clone()).collect();
                    if !missing.is_empty() {
                        self.err_help(span, format!("`match` doesn't cover: {}", missing.join(", ")), "add the missing cases, or `_ => ...`");
                    }
                    return;
                }
                self.err_help(span, "`match` doesn't cover every value", "add `_ => ...`");
            }
            Ty::Bool => {
                let t = arms.iter().any(|a| a.guard.is_none() && matches!(a.pat, TPat::Bool(true)));
                let f = arms.iter().any(|a| a.guard.is_none() && matches!(a.pat, TPat::Bool(false)));
                if !(t && f) {
                    self.err_help(span, "`match` doesn't cover every value", "add `_ => ...`");
                }
            }
            Ty::Opt(_) => {
                let s = arms.iter().any(|a| a.guard.is_none() && matches!(&a.pat, TPat::Some(p) if total(p)));
                let n = arms.iter().any(|a| a.guard.is_none() && matches!(a.pat, TPat::None));
                if !(s && n) {
                    self.err_help(span, "`match` doesn't cover every value", if !n { "add `none => ...`" } else { "add `some(v) => ...`" });
                }
            }
            Ty::Err | Ty::Never => {}
            _ => self.err_help(span, "`match` doesn't cover every value", "add `_ => ...`"),
        }
    }

    // Like `pattern`, but `if err is NotFound { ... }` rebinds `err` as the
    // record inside the block (flow typing).
    fn pattern_narrow(&mut self, pat: &Pattern, ty: &Ty, scrut: &Expr) -> TPat {
        if let (Pattern::Ctor { path, args: None, span }, ExprKind::Ident(name)) = (pat, &scrut.kind) {
            if let Ty::Iface(_) = self.resolve(ty) {
                let p = self.pattern(pat, ty);
                if let TPat::Type { def, targs, args, .. } = p {
                    let rt = Ty::Adt(def, targs.clone());
                    let _ = (path, span);
                    let id = self.rebind(name, rt);
                    return TPat::Type { def, targs, args, bind: Some(id) };
                }
                return p;
            }
        }
        self.pattern(pat, ty)
    }

    fn pattern(&mut self, pat: &Pattern, ty: &Ty) -> TPat {
        let ty = self.resolve(ty);
        match pat {
            Pattern::Wild(_) => TPat::Wild,
            Pattern::Or(alts, sp) => {
                if alts.iter().any(pattern_binds) {
                    self.err_help(*sp, "patterns joined with `|` can't bind names", "match the cases separately, or bind nothing: `Circle(_) | Square(_) => ...`");
                }
                TPat::Or(alts.iter().map(|a| self.pattern(a, &ty)).collect())
            }
            Pattern::Bind(name, sp) => TPat::Bind(self.declare(*sp, name, ty, false)),
            Pattern::Int(v, sp) => {
                if !matches!(ty, Ty::Int | Ty::Err) {
                    if ty == Ty::Float {
                        return TPat::Float(*v as f64);
                    }
                    let s = self.prog.show(&ty);
                    self.err(*sp, format!("a number pattern can't match a `{}`", s));
                }
                TPat::Int(*v)
            }
            Pattern::Float(v, sp) => {
                if !matches!(ty, Ty::Float | Ty::Err) {
                    let s = self.prog.show(&ty);
                    self.err(*sp, format!("a number pattern can't match a `{}`", s));
                }
                TPat::Float(*v)
            }
            Pattern::Str(s, sp) => {
                if !matches!(ty, Ty::Text | Ty::Err) {
                    let t = self.prog.show(&ty);
                    self.err(*sp, format!("a text pattern can't match a `{}`", t));
                }
                TPat::Text(s.clone())
            }
            Pattern::Bool(b, sp) => {
                if !matches!(ty, Ty::Bool | Ty::Err) {
                    let t = self.prog.show(&ty);
                    self.err(*sp, format!("`{}` can't match a `{}`", b, t));
                }
                TPat::Bool(*b)
            }
            Pattern::None(sp) => {
                if !matches!(ty, Ty::Opt(_) | Ty::Err) {
                    let t = self.prog.show(&ty);
                    self.err(*sp, format!("`none` can't match a `{}`: it's never missing", t));
                }
                TPat::None
            }
            Pattern::Some(inner, sp) => match &ty {
                Ty::Opt(t) => {
                    let t = (**t).clone();
                    TPat::Some(Box::new(self.pattern(inner, &t)))
                }
                Ty::Err => TPat::Wild,
                t => {
                    let s = self.prog.show(t);
                    self.err(*sp, format!("`some(...)` can't match a `{}`: it's never missing", s));
                    TPat::Wild
                }
            },
            Pattern::Ctor { path, args, span } => {
                let name = path.last().unwrap();
                match &ty {
                    Ty::Adt(d, targs) => {
                        let def = self.prog.defs[*d].clone();
                        match &def.kind {
                            TypeKind::Enum { variants } => {
                                if path.len() == 2 && path[0] != def.name {
                                    self.err(*span, format!("expected a variant of `{}`", def.name));
                                }
                                let idx = match variants.iter().position(|v| v.name == *name) {
                                    Some(i) => i,
                                    None => {
                                        let names: Vec<String> = variants.iter().map(|v| v.name.clone()).collect();
                                        let help = suggest(name, names.iter()).map(|s| format!("did you mean `{}`?", s)).unwrap_or_else(|| format!("variants: {}", names.join(", ")));
                                        self.err_help(*span, format!("`{}` has no variant `{}`", def.name, name), help);
                                        return TPat::Wild;
                                    }
                                };
                                let ftys: Vec<Ty> = variants[idx].fields.iter().map(|f| f.ty.subst(targs)).collect();
                                let args = self.ctor_args(args.as_deref(), &ftys, name, *span);
                                TPat::Variant { idx, args }
                            }
                            TypeKind::Record { fields } => {
                                if def.name != *name {
                                    self.err(*span, format!("expected `{}`", def.name));
                                }
                                let ftys: Vec<Ty> = fields.iter().map(|f| f.ty.subst(targs)).collect();
                                let args = self.ctor_args(args.as_deref(), &ftys, name, *span);
                                TPat::Record { args }
                            }
                            _ => {
                                self.err(*span, format!("`{}` can't be matched by a pattern", def.name));
                                TPat::Wild
                            }
                        }
                    }
                    Ty::Iface(ids) => {
                        let md = self.fc().module;
                        // `NotFound`, or `time.TimedOut` from another module
                        let found = if path.len() == 2 && self.lookup_local(&path[0]).is_none() && self.module_named(&path[0]).is_some() {
                            let target = self.module_named(&path[0]).unwrap();
                            self.lookup_in_module(target, &path[1], *span)
                        } else {
                            self.lookup_global(name, md)
                        };
                        let d = match found {
                            Some(Global::Type(d)) => d,
                            _ => {
                                self.err(*span, format!("unknown type `{}`", path.join(".")));
                                return TPat::Wild;
                            }
                        };
                        if !self.implements(d, ids) {
                            let (a, b) = (self.prog.defs[d].name.clone(), self.prog.show(&ty));
                            self.err(*span, format!("`{}` doesn't implement `{}`, so it can never match", a, b));
                        }
                        let fields = match &self.prog.defs[d].kind {
                            TypeKind::Record { fields } => fields.clone(),
                            _ => vec![],
                        };
                        let ftys: Vec<Ty> = fields.iter().map(|f| f.ty.clone()).collect();
                        let args = match args {
                            None => vec![],
                            Some(_) => self.ctor_args(args.as_deref(), &ftys, name, *span),
                        };
                        TPat::Type { def: d, targs: vec![], args, bind: None }
                    }
                    Ty::Err => TPat::Wild,
                    t => {
                        let s = self.prog.show(t);
                        self.err(*span, format!("`{}` can't match a `{}`", name, s));
                        TPat::Wild
                    }
                }
            }
        }
    }

    fn ctor_args(&mut self, args: Option<&[Pattern]>, ftys: &[Ty], name: &str, span: Span) -> Vec<TPat> {
        match args {
            None => ftys.iter().map(|_| TPat::Wild).collect(),
            Some(ps) => {
                if ps.len() == 1 && matches!(ps[0], Pattern::Wild(_)) {
                    return ftys.iter().map(|_| TPat::Wild).collect();
                }
                if ps.len() != ftys.len() {
                    self.err_help(span, format!("`{}` has {} field(s), but the pattern has {}", name, ftys.len(), ps.len()), "bind every field by position, or write `(_)` to ignore them all");
                }
                ps.iter().zip(ftys).map(|(p, t)| self.pattern(p, t)).collect()
            }
        }
    }

    // ================= resolving inference variables =================

    fn zonk_ty(&self, t: &Ty, unresolved: &mut Option<bool>) -> Ty {
        let r = self.resolve(t);
        if has_var(&r) {
            if let Some(u) = unresolved {
                *u = true;
            }
            return subst_vars_err(&r);
        }
        r
    }

    fn zonk_block(&self, b: &mut TBlock, un: &mut Option<Span>) {
        for s in &mut b.stmts {
            self.zonk_stmt(s, un);
        }
        if let Some(t) = &mut b.tail {
            self.zonk_expr(t, un);
        }
        b.ty = self.zonk_ty(&b.ty, &mut None);
    }

    fn zonk_stmt(&self, s: &mut TStmt, un: &mut Option<Span>) {
        match s {
            TStmt::Let(_, e) | TStmt::Discard(e) | TStmt::Expr(e) | TStmt::Throw(e) => self.zonk_expr(e, un),
            TStmt::Assign(p, e) => {
                self.zonk_place(p, un);
                self.zonk_expr(e, un);
            }
            TStmt::Return(e) => {
                if let Some(e) = e {
                    self.zonk_expr(e, un)
                }
            }
            TStmt::Break | TStmt::Continue => {}
            TStmt::While(c, b) => {
                self.zonk_expr(c, un);
                self.zonk_block(b, un);
            }
            TStmt::ForList { list: e, body, .. } | TStmt::ForMap { map: e, body, .. } | TStmt::ForSet { set: e, body, .. } | TStmt::ForChannel { chan: e, body, .. } | TStmt::WithLock { shared: e, body, .. } | TStmt::With { value: e, body, .. } => {
                self.zonk_expr(e, un);
                self.zonk_block(body, un);
            }
            TStmt::ForRange { lo, hi, body, .. } => {
                self.zonk_expr(lo, un);
                self.zonk_expr(hi, un);
                self.zonk_block(body, un);
            }
            TStmt::Expect { cond, .. } => self.zonk_expr(cond, un),
        }
    }

    fn zonk_place(&self, p: &mut TPlace, un: &mut Option<Span>) {
        p.ty = self.zonk_ty(&p.ty, &mut None);
        for el in &mut p.path {
            match el {
                PlaceElem::Index(e) | PlaceElem::MapKey(e) => self.zonk_expr(e, un),
                PlaceElem::Field(_) => {}
            }
        }
    }

    fn zonk_expr(&self, e: &mut TExpr, un: &mut Option<Span>) {
        let mut u = Some(false);
        e.ty = self.zonk_ty(&e.ty, &mut u);
        if u == Some(true) && un.is_none() {
            *un = Some(e.span);
        }
        let zl = |c: &Checker, v: &mut Vec<Ty>| {
            for t in v.iter_mut() {
                *t = c.zonk_ty(t, &mut None);
            }
        };
        match &mut e.kind {
            TK::FnRef(_, targs) => zl(self, targs),
            TK::Call { callee, args, .. } => {
                match callee {
                    Callee::Fn(_, targs) => zl(self, targs),
                    Callee::Value(f) => self.zonk_expr(f, un),
                    Callee::Iface { .. } => {}
                }
                for a in args {
                    self.zonk_expr(a, un);
                }
            }
            TK::MutCall { type_args, place, args, .. } => {
                zl(self, type_args);
                self.zonk_place(place, un);
                for a in args {
                    self.zonk_expr(a, un);
                }
            }
            TK::IfaceMutCall { place, args, .. } => {
                self.zonk_place(place, un);
                for a in args {
                    self.zonk_expr(a, un);
                }
            }
            TK::Record { fields, .. } | TK::Variant { fields, .. } | TK::Interp(fields) | TK::List(fields) => {
                for f in fields {
                    self.zonk_expr(f, un);
                }
            }
            TK::Map(entries) => {
                for (k, v) in entries {
                    self.zonk_expr(k, un);
                    self.zonk_expr(v, un);
                }
            }
            TK::Wrap(x) | TK::Unwrap(x) | TK::Field(x, _) | TK::Prop(x, _) | TK::Unary(_, x) | TK::Some(x) | TK::ToIface(x) | TK::ToText(x) | TK::ExpectThrows(x) | TK::Spawn(x) => {
                self.zonk_expr(x, un)
            }
            TK::Index(a, b) | TK::MapGet(a, b) | TK::Binary(_, a, b) | TK::Coalesce(a, b) => {
                self.zonk_expr(a, un);
                self.zonk_expr(b, un);
            }
            TK::Lambda { body, .. } => self.zonk_expr(body, un),
            TK::Block(b) => self.zonk_block(b, un),
            TK::If { cond, then, els } => {
                self.zonk_expr(cond, un);
                self.zonk_block(then, un);
                if let Some(x) = els {
                    self.zonk_expr(x, un);
                }
            }
            TK::Match { scrut, arms } => {
                self.zonk_expr(scrut, un);
                for a in arms {
                    self.zonk_pat(&mut a.pat);
                    if let Some(g) = &mut a.guard {
                        self.zonk_expr(g, un);
                    }
                    self.zonk_expr(&mut a.body, un);
                }
            }
            TK::Is(x, p) => {
                self.zonk_expr(x, un);
                self.zonk_pat(p);
            }
            TK::Try { call, catch } => {
                self.zonk_expr(call, un);
                if let Some((_, b)) = catch {
                    self.zonk_block(b, un);
                }
            }
            TK::Diverge(s) => self.zonk_stmt(s, un),
            _ => {}
        }
    }

    fn zonk_pat(&self, p: &mut TPat) {
        match p {
            TPat::Some(x) => self.zonk_pat(x),
            TPat::Variant { args, .. } | TPat::Record { args } => {
                for a in args {
                    self.zonk_pat(a)
                }
            }
            TPat::Type { targs, args, .. } => {
                for t in targs.iter_mut() {
                    *t = self.zonk_ty(t, &mut None);
                }
                for a in args {
                    self.zonk_pat(a)
                }
            }
            _ => {}
        }
    }
}

fn has_var(t: &Ty) -> bool {
    match t {
        Ty::Var(_) => true,
        Ty::Adt(_, a) => a.iter().any(has_var),
        Ty::Opt(x) => has_var(x),
        Ty::Func(f) => f.params.iter().any(has_var) || has_var(&f.ret),
        _ => false,
    }
}

fn subst_vars_err(t: &Ty) -> Ty {
    match t {
        Ty::Var(_) => Ty::Err,
        Ty::Adt(d, a) => Ty::Adt(*d, a.iter().map(subst_vars_err).collect()),
        Ty::Opt(x) => Ty::opt(subst_vars_err(x)),
        Ty::Func(f) => Ty::func(f.params.iter().map(subst_vars_err).collect(), subst_vars_err(&f.ret), f.throws),
        t => t.clone(),
    }
}

fn contains_fn(t: &Ty, p: &Program) -> bool {
    match t {
        Ty::Func(_) => true,
        Ty::Opt(x) => contains_fn(x, p),
        Ty::Adt(d, a) => {
            if a.iter().any(|x| contains_fn(x, p)) {
                return true;
            }
            match &p.defs[*d].kind {
                TypeKind::Record { fields } => fields.iter().any(|f| matches!(f.ty, Ty::Func(_))),
                TypeKind::Newtype(Ty::Func(_)) => true,
                _ => false,
            }
        }
        _ => false,
    }
}

fn is_literal(e: &Expr) -> bool {
    matches!(e.kind, ExprKind::Int(_) | ExprKind::Float(_))
}

fn pattern_binds(p: &Pattern) -> bool {
    match p {
        Pattern::Bind(..) => true,
        Pattern::Or(alts, _) => alts.iter().any(pattern_binds),
        Pattern::Some(x, _) => pattern_binds(x),
        Pattern::Ctor { args: Some(a), .. } => a.iter().any(pattern_binds),
        _ => false,
    }
}

fn is_type_test(p: &Pattern) -> bool {
    matches!(p, Pattern::Ctor { args: None, .. })
}

fn stmt_span(s: &Stmt) -> Span {
    match s {
        Stmt::Let { span, .. } | Stmt::Assign { span, .. } | Stmt::While { span, .. } | Stmt::For { span, .. } | Stmt::With { span, .. } | Stmt::Expect { span, .. } => *span,
        Stmt::Return(_, s) | Stmt::Break(s) | Stmt::Continue(s) | Stmt::Throw(_, s) => *s,
        Stmt::Expr(e) => e.span,
    }
}

fn stmt_diverges(s: &TStmt) -> bool {
    match s {
        TStmt::Return(_) | TStmt::Break | TStmt::Continue | TStmt::Throw(_) => true,
        TStmt::Expr(e) => e.ty == Ty::Never,
        TStmt::Let(_, e) => e.ty == Ty::Never,
        _ => false,
    }
}

// The first part of a top-level `let`'s value that isn't fixed: a call of a
// function, `try`, a lambda, ... Types and variants (capitalized) may be
// called: `Point(x: 1, y: 2)`.
fn not_fixed(e: &Expr) -> Option<Span> {
    match &e.kind {
        ExprKind::Int(_) | ExprKind::Float(_) | ExprKind::Bool(_) | ExprKind::None | ExprKind::Ident(_) => None,
        ExprKind::Str(segs) => segs.iter().find_map(|s| match s {
            ast::StrSeg::Expr(x) => not_fixed(x),
            _ => None,
        }),
        ExprKind::List(xs) => xs.iter().find_map(not_fixed),
        ExprKind::Map(kvs) => kvs.iter().find_map(|(k, v)| not_fixed(k).or_else(|| not_fixed(v))),
        // `module.NAME` or a field of another fixed value
        ExprKind::Field(b, _, _) => not_fixed(b),
        ExprKind::Unary(_, x) => not_fixed(x),
        ExprKind::Binary(_, a, b) | ExprKind::Coalesce(a, b) => not_fixed(a).or_else(|| not_fixed(b)),
        ExprKind::Call { callee, args, .. } => {
            let name = match &callee.kind {
                ExprKind::Ident(n) => Some(n),
                ExprKind::Field(b, n, _) if matches!(b.kind, ExprKind::Ident(_)) => Some(n),
                _ => None,
            };
            match name {
                Some(n) if n.chars().next().map(|c| c.is_uppercase()).unwrap_or(false) => args.iter().find_map(|a| not_fixed(&a.value)),
                _ => Some(e.span),
            }
        }
        _ => Some(e.span),
    }
}
