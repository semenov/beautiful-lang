// Lowering: the typed tree -> MIR, instantiating generic functions for the
// concrete types they are used with.

use crate::ast::{BinOp, UnOp};
use crate::diag::{Sources, Span};
use crate::mir::{self, Callee as MCallee, FnKind, Func, Op, Place, Proj, Rv, Stmt, Term, B, L};
use crate::types::*;
use std::collections::HashMap;

struct Fb<'a> {
    f: Func,
    cur: B,
    map: HashMap<LocalId, L>,
    subst: Vec<Ty>,
    // for rethrows instances called with non-throwing functions
    no_throw_fns: bool,
    body_locals: &'a [LocalDef],
    // (break target, continue target, cleanup depth)
    loops: Vec<(B, B, usize)>,
    line: u32,
    closed: Vec<bool>,
    // things to undo on every way out: locks, the function's tasks
    cleanups: Vec<Cleanup>,
    scope: Option<L>,
    // errors inside `try ... catch`: where they go
    catch_targets: Vec<(B, L)>,
}

#[derive(Clone)]
enum Cleanup {
    Unlock { shared: L, var: L, ty: Ty },
    Scope(L),
    Close { res: L, close: FnId },
}

// Does the code start tasks (not counting nested lambdas)?
fn expr_spawns(e: &TExpr) -> bool {
    match &e.kind {
        TK::Spawn(_) => true,
        TK::Lambda { .. } => false,
        TK::Call { callee, args, .. } => {
            (match callee {
                Callee::Value(f) => expr_spawns(f),
                _ => false,
            }) || args.iter().any(expr_spawns)
        }
        TK::MutCall { place, args, .. } | TK::IfaceMutCall { place, args, .. } => place_spawns(place) || args.iter().any(expr_spawns),
        TK::Record { fields, .. } | TK::Variant { fields, .. } | TK::Interp(fields) | TK::List(fields) => fields.iter().any(expr_spawns),
        TK::Map(entries) => entries.iter().any(|(k, v)| expr_spawns(k) || expr_spawns(v)),
        TK::Wrap(x) | TK::Unwrap(x) | TK::Field(x, _) | TK::Prop(x, _) | TK::Unary(_, x) | TK::Some(x) | TK::ToIface(x) | TK::ToText(x) | TK::ExpectThrows(x) | TK::Is(x, _) => expr_spawns(x),
        TK::Index(a, b) | TK::MapGet(a, b) | TK::Binary(_, a, b) | TK::Coalesce(a, b) => expr_spawns(a) || expr_spawns(b),
        TK::Block(b) => block_spawns(b),
        TK::If { cond, then, els } => expr_spawns(cond) || block_spawns(then) || els.as_ref().map(|x| expr_spawns(x)).unwrap_or(false),
        TK::Match { scrut, arms } => expr_spawns(scrut) || arms.iter().any(|a| expr_spawns(&a.body) || a.guard.as_ref().map(expr_spawns).unwrap_or(false)),
        TK::Try { call, catch } => expr_spawns(call) || catch.as_ref().map(|(_, b)| block_spawns(b)).unwrap_or(false),
        TK::Diverge(s) => stmt_spawns(s),
        _ => false,
    }
}
fn place_spawns(p: &TPlace) -> bool {
    p.path.iter().any(|e| match e {
        PlaceElem::Index(x) | PlaceElem::MapKey(x) => expr_spawns(x),
        _ => false,
    })
}
fn block_spawns(b: &TBlock) -> bool {
    b.stmts.iter().any(stmt_spawns) || b.tail.as_ref().map(|x| expr_spawns(x)).unwrap_or(false)
}
fn stmt_spawns(s: &TStmt) -> bool {
    match s {
        TStmt::Let(_, e) | TStmt::Discard(e) | TStmt::Expr(e) | TStmt::Throw(e) => expr_spawns(e),
        TStmt::Assign(p, e) => place_spawns(p) || expr_spawns(e),
        TStmt::Return(e) => e.as_ref().map(expr_spawns).unwrap_or(false),
        TStmt::While(c, b) => expr_spawns(c) || block_spawns(b),
        TStmt::ForList { list: e, body, .. } | TStmt::ForMap { map: e, body, .. } | TStmt::ForSet { set: e, body, .. } | TStmt::ForChannel { chan: e, body, .. } | TStmt::WithLock { shared: e, body, .. } | TStmt::With { value: e, body, .. } => expr_spawns(e) || block_spawns(body),
        TStmt::ForRange { lo, hi, body, .. } => expr_spawns(lo) || expr_spawns(hi) || block_spawns(body),
        TStmt::Expect { cond, .. } => expr_spawns(cond),
        TStmt::Break | TStmt::Continue => false,
    }
}

pub struct Lowerer<'a> {
    prog: &'a Program,
    src: &'a Sources,
    pub funcs: Vec<Func>,
    pub vtables: Vec<mir::Vtable>,
    vt_map: HashMap<(Ty, Vec<DefId>), usize>,
    // conversions between interface combinations: (from, to)
    upcasts: Vec<(Vec<DefId>, Vec<DefId>)>,
    instances: HashMap<(FnId, Vec<Ty>, bool), usize>,
    queue: Vec<(usize, FnId, Vec<Ty>, bool)>,
    thunks: HashMap<(FnId, Vec<Ty>), usize>,
    fbs: Vec<Fb<'a>>,
    user_file: u32,
    default_fns: Vec<(Ty, usize)>,
    decoded: std::collections::HashSet<Ty>,
}

pub fn iface_methods(prog: &Program, ids: &[DefId]) -> Vec<FnId> {
    let mut v = vec![];
    for i in ids {
        if let TypeKind::Interface { methods } = &prog.defs[*i].kind {
            v.extend(methods.iter().copied());
        }
    }
    v
}

impl<'a> Lowerer<'a> {
    pub fn new(prog: &'a Program, src: &'a Sources, user_file: u32) -> Lowerer<'a> {
        Lowerer {
            prog,
            src,
            funcs: vec![],
            vtables: vec![],
            vt_map: HashMap::new(),
            upcasts: vec![],
            instances: HashMap::new(),
            queue: vec![],
            thunks: HashMap::new(),
            fbs: vec![],
            user_file,
            default_fns: vec![],
            decoded: std::collections::HashSet::new(),
        }
    }

    pub fn run(mut self, with_tests: bool) -> mir::Module {
        let main = self.prog.main.map(|m| self.instance(m, vec![], false));
        let mut tests = vec![];
        if with_tests {
            for (i, t) in self.prog.tests.iter().enumerate() {
                let idx = self.lower_body(&t.body, vec![], false, format!("test_{}", i), FnKind::Test(t.name.clone()), Ty::Unit, true, false, t.name.clone());
                tests.push(idx);
            }
        }
        // the runtime builds `Failure` errors (e.g. for `to_int`)
        let failure = Ty::Adt(self.prog.b.failure, vec![]);
        let failure_vtable = self.vtable(failure, vec![self.prog.b.error]);
        let cancelled_vtable = self.vtable(Ty::Adt(self.prog.b.cancelled, vec![]), vec![self.prog.b.error]);
        let closed_vtable = self.vtable(Ty::Adt(self.prog.b.channel_closed, vec![]), vec![self.prog.b.error]);
        self.drain();
        // every type that reaches a combination converted to another needs
        // a vtable for the other one too (and its methods may add more)
        loop {
            let before = self.vtables.len();
            for (from, to) in self.upcasts.clone() {
                let concretes: Vec<Ty> = self.vtables.iter().filter(|v| v.iface == from).map(|v| v.concrete.clone()).collect();
                for c in concretes {
                    self.vtable(c, to.clone());
                }
            }
            self.drain();
            if self.vtables.len() == before {
                break;
            }
        }
        mir::Module { funcs: self.funcs, vtables: self.vtables, main, tests, failure_vtable, cancelled_vtable, closed_vtable, default_fns: self.default_fns }
    }

    fn drain(&mut self) {
        while let Some((idx, id, targs, rethrow)) = self.queue.pop() {
            let f = &self.prog.fns[id];
            let body = f.body.as_ref().expect("function without body");
            let ret = f.ret.subst(&targs);
            let no_throw = f.rethrows && !rethrow;
            let throws = if f.rethrows { rethrow } else { f.throws };
            let ret = if no_throw { strip_throws(&ret) } else { ret };
            let name = self.funcs[idx].name.clone();
            let src_name = self.funcs[idx].source_name.clone();
            let lowered = self.build(body, targs, no_throw, name, FnKind::Normal, ret, throws, f.self_mode == SelfMode::Mutating, src_name);
            self.funcs[idx] = lowered;
        }
    }

    // Returns the index of an instance, queueing it for lowering.
    fn instance(&mut self, id: FnId, targs: Vec<Ty>, rethrow: bool) -> usize {
        let key = (id, targs.clone(), rethrow);
        if let Some(i) = self.instances.get(&key) {
            return *i;
        }
        let f = &self.prog.fns[id];
        let idx = self.funcs.len();
        let owner = f.owner.map(|d| format!("{}_", sanitize(&self.prog.defs[d].name))).unwrap_or_default();
        let src_name = match f.owner {
            Some(d) => format!("{}.{}", self.prog.defs[d].name, f.name),
            None => f.name.clone(),
        };
        self.funcs.push(Func {
            name: format!("f{}_{}{}", idx, owner, sanitize(&f.name)),
            kind: FnKind::Normal,
            params: vec![],
            ret: Ty::Unit,
            throws: false,
            mutating: false,
            locals: vec![],
            blocks: vec![],
            source_name: src_name,
        });
        self.instances.insert(key, idx);
        self.queue.push((idx, id, targs, rethrow));
        idx
    }

    fn lower_body(&mut self, body: &'a TBody, subst: Vec<Ty>, no_throw: bool, name: String, kind: FnKind, ret: Ty, throws: bool, mutating: bool, src_name: String) -> usize {
        let f = self.build(body, subst, no_throw, name, kind, ret, throws, mutating, src_name);
        self.funcs.push(f);
        self.funcs.len() - 1
    }

    fn build(&mut self, body: &'a TBody, subst: Vec<Ty>, no_throw: bool, name: String, kind: FnKind, ret: Ty, throws: bool, mutating: bool, src_name: String) -> Func {
        let func = Func { name, kind, params: vec![], ret: ret.clone(), throws, mutating, locals: vec![], blocks: vec![], source_name: src_name };
        self.fbs.push(Fb { f: func, cur: 0, map: HashMap::new(), subst, no_throw_fns: no_throw, body_locals: &body.locals, loops: vec![], line: 0, closed: vec![], cleanups: vec![], scope: None, catch_targets: vec![] });
        self.new_block();
        for (i, p) in body.params.iter().enumerate() {
            let ty = self.local_ty(*p);
            let l = self.new_local(ty, &body.locals[*p].name.clone());
            if i == 0 && mutating {
                self.fb().f.locals[l].self_ptr = true;
            }
            self.fb().map.insert(*p, l);
            self.fb().f.params.push(l);
        }
        if block_spawns(&body.block) {
            self.open_scope();
        }
        let v = self.block(&body.block);
        if !self.terminated() {
            let r = self.fb().f.ret.clone();
            if r == Ty::Unit || r == Ty::Never {
                self.ret(Op::Unit);
            } else {
                // a value block (lambdas): return the tail
                self.ret(v);
            }
        }
        self.fbs.pop().unwrap().f
    }

    // ---- builder helpers ----

    fn fb(&mut self) -> &mut Fb<'a> {
        self.fbs.last_mut().unwrap()
    }
    fn fbr(&self) -> &Fb<'a> {
        self.fbs.last().unwrap()
    }
    fn ty(&self, t: &Ty) -> Ty {
        let fb = self.fbr();
        let t = t.subst(&fb.subst);
        if fb.no_throw_fns {
            strip_throws(&t)
        } else {
            t
        }
    }
    fn local_ty(&self, id: LocalId) -> Ty {
        let t = self.fbr().body_locals[id].ty.clone();
        self.ty(&t)
    }
    fn new_local(&mut self, ty: Ty, name: &str) -> L {
        let f = &mut self.fb().f;
        f.locals.push(mir::Local { ty, name: name.to_string(), self_ptr: false });
        f.locals.len() - 1
    }
    fn tmp(&mut self, ty: Ty) -> L {
        self.new_local(ty, "")
    }
    fn new_block(&mut self) -> B {
        let line = self.fbr().line;
        let fb = self.fb();
        fb.f.blocks.push(mir::Block { stmts: vec![], term: Term::Unreachable, line });
        fb.closed.push(false);
        fb.f.blocks.len() - 1
    }
    fn switch_to(&mut self, b: B) {
        let line = self.fbr().line;
        let fb = self.fb();
        fb.cur = b;
        if fb.f.blocks[b].stmts.is_empty() {
            fb.f.blocks[b].line = line;
        }
    }
    fn terminated(&self) -> bool {
        let fb = self.fbr();
        fb.closed[fb.cur]
    }
    fn reopen(&mut self, b: B) {
        let fb = self.fb();
        fb.closed[b] = false;
        fb.f.blocks[b].term = Term::Unreachable;
    }
    fn emit(&mut self, s: Stmt) {
        if self.terminated() {
            // code after a return/throw: put it in a dead block
            let b = self.new_block();
            self.switch_to(b);
        }
        let cur = self.fbr().cur;
        self.fb().f.blocks[cur].stmts.push(s);
    }
    fn term(&mut self, t: Term) {
        if self.terminated() {
            return;
        }
        let fb = self.fb();
        let cur = fb.cur;
        fb.f.blocks[cur].term = t;
        fb.closed[cur] = true;
    }
    fn assign(&mut self, ty: Ty, rv: Rv) -> Op {
        let l = self.tmp(ty);
        self.emit(Stmt::Assign(l, rv));
        Op::Local(l)
    }
    fn set_line(&mut self, span: Span) {
        if span.file != self.user_file {
            return;
        }
        let (line, _) = self.src.line_col(span);
        let line = line as u32;
        if self.fbr().line != line {
            self.fb().line = line;
            self.emit(Stmt::Line(line));
        }
    }
    fn to_local(&mut self, op: Op, ty: Ty) -> L {
        match op {
            Op::Local(l) => l,
            o => {
                let l = self.tmp(ty);
                self.emit(Stmt::Assign(l, Rv::Use(o)));
                l
            }
        }
    }

    // ---- cleanups: locks and the function's tasks ----

    fn open_scope(&mut self) {
        let s = self.new_local(Ty::Int, "scope");
        self.emit(Stmt::Assign(s, Rv::Call(MCallee::Intrinsic("scope_new".into(), vec![]), vec![])));
        self.fb().scope = Some(s);
        self.fb().cleanups.push(Cleanup::Scope(s));
    }

    // Undo cleanups down to `depth`. On the error path, tasks are cancelled
    // and their errors ignored (the function already has one).
    fn run_cleanups(&mut self, depth: usize, error: bool) {
        let n = self.fbr().cleanups.len();
        for i in (depth..n).rev() {
            let c = self.fbr().cleanups[i].clone();
            match c {
                Cleanup::Unlock { shared, var, ty } => {
                    let _ = self.assign(Ty::Unit, Rv::Call(MCallee::Intrinsic("Shared.release".into(), vec![ty]), vec![Op::Local(shared), Op::Local(var)]));
                }
                Cleanup::Scope(s) => {
                    if error {
                        let _ = self.assign(Ty::Unit, Rv::Call(MCallee::Intrinsic("scope_cancel".into(), vec![]), vec![Op::Local(s)]));
                    } else {
                        let err = self.tmp(self.prog.error_ty());
                        self.emit(Stmt::CallT { dst: None, err, callee: MCallee::Intrinsic("scope_end".into(), vec![]), args: vec![Op::Local(s)] });
                        self.fail_below(err, i);
                    }
                }
                Cleanup::Close { res, close } => {
                    let rty = self.fbr().f.locals[res].ty.clone();
                    let (callee, throws) = self.method_callee(close, &rty);
                    if throws {
                        let err = self.tmp(self.prog.error_ty());
                        self.emit(Stmt::CallT { dst: None, err, callee, args: vec![Op::Local(res)] });
                        if !error {
                            self.fail_below(err, i);
                        }
                    } else {
                        let _ = self.assign(Ty::Unit, Rv::Call(callee, vec![Op::Local(res)]));
                    }
                }
            }
        }
    }

    // After a cleanup at level `level` failed with `err`: undo the levels
    // below it on the error path and leave the function.
    fn fail_below(&mut self, err: L, level: usize) {
        let bad = self.new_block();
        let ok = self.new_block();
        self.term(Term::IfErr(err, bad, ok));
        self.switch_to(bad);
        let saved = self.fbr().cleanups.clone();
        self.fb().cleanups.truncate(level);
        self.run_cleanups(0, true);
        self.fb().cleanups = saved;
        self.term(Term::Throw(Op::Local(err)));
        self.switch_to(ok);
    }

    // How to call a method of the standard library on a value of type `recv`.
    fn method_callee(&mut self, id: FnId, recv: &Ty) -> (MCallee, bool) {
        let f = &self.prog.fns[id];
        if f.intrinsic {
            (MCallee::Intrinsic(intrinsic_name(self.prog, f), vec![recv.clone()]), f.throws)
        } else {
            let targs = match recv {
                Ty::Adt(_, a) => a.clone(),
                _ => vec![],
            };
            (MCallee::Fn(self.instance(id, targs, false)), f.throws)
        }
    }

    fn ret(&mut self, v: Op) {
        if self.terminated() {
            return;
        }
        let v = match v {
            Op::Local(l) if !self.fbr().cleanups.is_empty() => Op::Local(l),
            o if !self.fbr().cleanups.is_empty() && !matches!(o, Op::Unit) => {
                let ty = self.fbr().f.ret.clone();
                Op::Local(self.to_local(o, ty))
            }
            o => o,
        };
        // `with r = ... { return r }`: the resource goes to the caller unclosed
        let saved = self.fbr().cleanups.clone();
        if let Op::Local(l) = v {
            self.fb().cleanups.retain(|c| !matches!(c, Cleanup::Close { res, .. } if *res == l));
        }
        self.run_cleanups(0, false);
        self.fb().cleanups = saved;
        self.term(Term::Return(v));
    }

    fn throw(&mut self, v: Op) {
        if self.terminated() {
            return;
        }
        let v = match v {
            Op::Local(l) => Op::Local(l),
            o => {
                let et = self.prog.error_ty();
                Op::Local(self.to_local(o, et))
            }
        };
        self.run_cleanups(0, true);
        self.term(Term::Throw(v));
    }

    // ---- vtables ----

    fn vtable(&mut self, concrete: Ty, ids: Vec<DefId>) -> usize {
        let key = (concrete.clone(), ids.clone());
        if let Some(v) = self.vt_map.get(&key) {
            return *v;
        }
        let d = match &concrete {
            Ty::Adt(d, _) => *d,
            _ => panic!("vtable for a non-record type"),
        };
        let mut methods = vec![];
        for m in iface_methods(self.prog, &ids) {
            let name = &self.prog.fns[m].name;
            let impl_id = self.prog.defs[d].methods[name];
            methods.push(self.instance(impl_id, vec![], false));
        }
        let idx = self.vtables.len();
        self.vtables.push(mir::Vtable { concrete, iface: ids, methods });
        self.vt_map.insert(key, idx);
        idx
    }

    // ---- statements ----

    fn block(&mut self, b: &'a TBlock) -> Op {
        for s in &b.stmts {
            self.stmt(s);
        }
        match &b.tail {
            Some(t) => self.expr(t),
            None => Op::Unit,
        }
    }

    fn stmt(&mut self, s: &'a TStmt) {
        match s {
            TStmt::Let(id, e) => {
                self.set_line(e.span);
                let v = self.expr(e);
                let ty = self.local_ty(*id);
                let name = self.fbr().body_locals[*id].name.clone();
                let l = self.new_local(ty, &name);
                self.fb().map.insert(*id, l);
                self.emit(Stmt::Assign(l, Rv::Use(v)));
            }
            TStmt::Discard(e) | TStmt::Expr(e) => {
                self.set_line(e.span);
                let _ = self.expr(e);
            }
            TStmt::Assign(place, e) => {
                self.set_line(e.span);
                let v = self.expr(e);
                let root = self.fbr().map[&place.root];
                if place.path.is_empty() && !self.fbr().f.locals[root].self_ptr {
                    self.emit(Stmt::Assign(root, Rv::Use(v)));
                } else {
                    let p = self.place(place, true);
                    self.emit(Stmt::Store(p, v));
                }
            }
            TStmt::Return(e) => {
                let v = match e {
                    Some(e) => {
                        self.set_line(e.span);
                        self.expr(e)
                    }
                    None => Op::Unit,
                };
                self.ret(v);
            }
            TStmt::Break => {
                let (t, _, depth) = *self.fbr().loops.last().unwrap();
                self.run_cleanups(depth, false);
                self.term(Term::Goto(t));
            }
            TStmt::Continue => {
                let (_, t, depth) = *self.fbr().loops.last().unwrap();
                self.run_cleanups(depth, false);
                self.term(Term::Goto(t));
            }
            TStmt::Throw(e) => {
                self.set_line(e.span);
                let v = self.expr(e);
                self.throw(v);
            }
            TStmt::WithLock { var, shared, body } => {
                self.set_line(shared.span);
                let sty = self.ty(&shared.ty);
                let vty = match &sty {
                    Ty::Adt(_, a) => a[0].clone(),
                    _ => unreachable!(),
                };
                let sv = self.expr(shared);
                let sl = self.new_local(sty.clone(), "shared");
                self.emit(Stmt::Assign(sl, Rv::Use(sv)));
                let name = self.fbr().body_locals[*var].name.clone();
                let x = self.new_local(vty.clone(), &name);
                self.fb().map.insert(*var, x);
                self.emit(Stmt::Assign(x, Rv::Call(MCallee::Intrinsic("Shared.acquire".into(), vec![vty.clone()]), vec![Op::Local(sl)])));
                self.fb().cleanups.push(Cleanup::Unlock { shared: sl, var: x, ty: vty.clone() });
                self.block(body);
                self.fb().cleanups.pop();
                if !self.terminated() {
                    let _ = self.assign(Ty::Unit, Rv::Call(MCallee::Intrinsic("Shared.release".into(), vec![vty]), vec![Op::Local(sl), Op::Local(x)]));
                }
            }
            TStmt::With { var, value, body, close } => {
                self.set_line(value.span);
                let rty = self.ty(&value.ty);
                let v = self.expr(value);
                let name = self.fbr().body_locals[*var].name.clone();
                let r = self.new_local(rty, &name);
                self.fb().map.insert(*var, r);
                self.emit(Stmt::Assign(r, Rv::Use(v)));
                let level = self.fbr().cleanups.len();
                self.fb().cleanups.push(Cleanup::Close { res: r, close: *close });
                self.block(body);
                if !self.terminated() {
                    self.run_cleanups(level, false);
                }
                self.fb().cleanups.pop();
            }
            TStmt::ForChannel { var, chan, body } => {
                self.set_line(chan.span);
                let cty = self.ty(&chan.ty);
                let elem = match &cty {
                    Ty::Adt(_, a) => a[0].clone(),
                    _ => unreachable!(),
                };
                let cv = self.expr(chan);
                let cl = self.new_local(cty.clone(), "chan");
                self.emit(Stmt::Assign(cl, Rv::Use(cv)));
                let head = self.new_block();
                let bodyb = self.new_block();
                let exit = self.new_block();
                self.term(Term::Goto(head));
                self.switch_to(head);
                let item = self.assign(Ty::opt(elem.clone()), Rv::Call(MCallee::Intrinsic("Channel.next".into(), vec![cty.clone()]), vec![Op::Local(cl)]));
                let c = self.assign(Ty::Bool, Rv::OptIsSome(item.clone()));
                self.term(Term::If(c, bodyb, exit));
                self.switch_to(bodyb);
                let name = self.fbr().body_locals[*var].name.clone();
                let v = self.new_local(elem, &name);
                self.fb().map.insert(*var, v);
                self.emit(Stmt::Assign(v, Rv::OptGet(item, false)));
                let depth = self.fbr().cleanups.len();
                self.fb().loops.push((exit, head, depth));
                self.block(body);
                self.fb().loops.pop();
                self.term(Term::Goto(head));
                self.switch_to(exit);
            }
            TStmt::While(c, body) => {
                let head = self.new_block();
                let bodyb = self.new_block();
                let exit = self.new_block();
                self.term(Term::Goto(head));
                self.switch_to(head);
                self.set_line(c.span);
                let cv = self.expr(c);
                self.term(Term::If(cv, bodyb, exit));
                self.switch_to(bodyb);
                let depth = self.fbr().cleanups.len();
                self.fb().loops.push((exit, head, depth));
                self.block(body);
                self.fb().loops.pop();
                self.term(Term::Goto(head));
                self.switch_to(exit);
            }
            TStmt::ForRange { var, lo, hi, inclusive, body } => {
                self.set_line(lo.span);
                let lo = self.expr(lo);
                let hi = self.expr(hi);
                let hi = self.to_local(hi, Ty::Int);
                let i = self.new_local(Ty::Int, "i");
                self.emit(Stmt::Assign(i, Rv::Use(lo)));
                let head = self.new_block();
                let bodyb = self.new_block();
                let step = self.new_block();
                let exit = self.new_block();
                self.term(Term::Goto(head));
                self.switch_to(head);
                let op = if *inclusive { BinOp::Le } else { BinOp::Lt };
                let c = self.assign(Ty::Bool, Rv::Bin(op, Op::Local(i), Op::Local(hi)));
                self.term(Term::If(c, bodyb, exit));
                self.switch_to(bodyb);
                let v = self.new_local(Ty::Int, &self.fbr().body_locals[*var].name.clone());
                self.fb().map.insert(*var, v);
                self.emit(Stmt::Assign(v, Rv::Use(Op::Local(i))));
                let depth = self.fbr().cleanups.len();
                self.fb().loops.push((exit, step, depth));
                self.block(body);
                self.fb().loops.pop();
                self.term(Term::Goto(step));
                self.switch_to(step);
                self.emit(Stmt::Assign(i, Rv::Call(MCallee::Intrinsic("int_inc".into(), vec![]), vec![Op::Local(i)])));
                self.term(Term::Goto(head));
                self.switch_to(exit);
            }
            TStmt::ForList { var, list, body } => {
                self.set_line(list.span);
                let lv = self.expr(list);
                let lty = self.ty(&list.ty);
                self.for_list(lv, lty, *var, body);
            }
            TStmt::ForMap { var, map, body } => {
                self.set_line(map.span);
                let mv = self.expr(map);
                let mty = self.ty(&map.ty);
                let (k, v) = match &mty {
                    Ty::Adt(_, a) => (a[0].clone(), a[1].clone()),
                    _ => unreachable!(),
                };
                let ety = self.prog.list_of(Ty::Adt(self.prog.b.entry, vec![k, v]));
                let entries = self.assign(ety.clone(), Rv::Call(MCallee::Intrinsic("Map.entries".into(), vec![mty]), vec![mv]));
                self.for_list(entries, ety, *var, body);
            }
            TStmt::ForSet { var, set, body } => {
                self.set_line(set.span);
                let sv = self.expr(set);
                let sty = self.ty(&set.ty);
                let elem = match &sty {
                    Ty::Adt(_, a) => a[0].clone(),
                    _ => unreachable!(),
                };
                let lty = self.prog.list_of(elem);
                let items = self.assign(lty.clone(), Rv::Call(MCallee::Intrinsic("Set.to_list".into(), vec![sty]), vec![sv]));
                self.for_list(items, lty, *var, body);
            }
            TStmt::Expect { cond, span, .. } => {
                self.set_line(*span);
                self.expect(cond, *span);
            }
        }
    }

    fn for_list(&mut self, lv: Op, lty: Ty, var: LocalId, body: &'a TBlock) {
        let elem = match &lty {
            Ty::Adt(_, a) => a[0].clone(),
            _ => unreachable!(),
        };
        let it = self.new_local(lty.clone(), "it");
        self.emit(Stmt::Assign(it, Rv::Use(lv)));
        let n = self.new_local(Ty::Int, "n");
        self.emit(Stmt::Assign(n, Rv::Call(MCallee::Intrinsic("List.length".into(), vec![lty.clone()]), vec![Op::Local(it)])));
        let i = self.new_local(Ty::Int, "i");
        self.emit(Stmt::Assign(i, Rv::Use(Op::Int(0))));
        let head = self.new_block();
        let bodyb = self.new_block();
        let step = self.new_block();
        let exit = self.new_block();
        self.term(Term::Goto(head));
        self.switch_to(head);
        let c = self.assign(Ty::Bool, Rv::Bin(BinOp::Lt, Op::Local(i), Op::Local(n)));
        self.term(Term::If(c, bodyb, exit));
        self.switch_to(bodyb);
        let name = self.fbr().body_locals[var].name.clone();
        let v = self.new_local(elem.clone(), &name);
        self.fb().map.insert(var, v);
        self.emit(Stmt::Assign(v, Rv::Call(MCallee::Intrinsic("List.get_unchecked".into(), vec![lty.clone()]), vec![Op::Local(it), Op::Local(i)])));
        let depth = self.fbr().cleanups.len();
        self.fb().loops.push((exit, step, depth));
        self.block(body);
        self.fb().loops.pop();
        self.term(Term::Goto(step));
        self.switch_to(step);
        self.emit(Stmt::Assign(i, Rv::Call(MCallee::Intrinsic("int_inc".into(), vec![]), vec![Op::Local(i)])));
        self.term(Term::Goto(head));
        self.switch_to(exit);
    }

    fn expect(&mut self, cond: &'a TExpr, span: Span) {
        let text = self.src_text(cond.span);
        let (line, _) = self.src.line_col(span);
        let fail = self.new_block();
        let ok = self.new_block();
        let mut details: Vec<Op> = vec![];
        if let TK::Binary(op, a, b) = &cond.kind {
            if matches!(op, BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge) {
                let av = self.expr(a);
                let bv = self.expr(b);
                let aty = self.ty(&a.ty);
                let bty = self.ty(&b.ty);
                let av = Op::Local(self.to_local(av, aty.clone()));
                let bv = Op::Local(self.to_local(bv, bty.clone()));
                let c = self.compare(*op, av.clone(), bv.clone(), &aty);
                self.term(Term::If(c, ok, fail));
                self.switch_to(fail);
                let at = self.assign(Ty::Text, Rv::Call(MCallee::Intrinsic("debug_text".into(), vec![aty]), vec![av]));
                let bt = self.assign(Ty::Text, Rv::Call(MCallee::Intrinsic("debug_text".into(), vec![bty]), vec![bv]));
                details = vec![at, bt];
            }
        }
        if details.is_empty() {
            let c = self.expr(cond);
            self.term(Term::If(c, ok, fail));
            self.switch_to(fail);
        }
        let mut args = vec![Op::Text(text), Op::Int(line as i64)];
        args.extend(details);
        let err = self.assign(self.prog.error_ty(), Rv::Call(MCallee::Intrinsic("expect_failed".into(), vec![]), args));
        self.throw(err);
        self.switch_to(ok);
    }

    fn src_text(&self, span: Span) -> String {
        let f = &self.src.files[span.file as usize];
        f.text.get(span.lo as usize..span.hi as usize).unwrap_or("").to_string()
    }

    // ---- places ----

    fn place(&mut self, p: &'a TPlace, write: bool) -> Place {
        let root = self.fbr().map[&p.root];
        let mut ty = self.fbr().f.locals[root].ty.clone();
        let mut path = vec![];
        let n = p.path.len();
        for (i, el) in p.path.iter().enumerate() {
            let (proj, next) = match el {
                PlaceElem::Field(f) => {
                    let fty = field_ty(self.prog, &ty, *f);
                    (Proj::Field(*f), fty)
                }
                PlaceElem::Index(e) => {
                    let v = self.expr(e);
                    let v = Op::Local(self.to_local(v, Ty::Int));
                    let et = match &ty {
                        Ty::Adt(_, a) => a[0].clone(),
                        _ => unreachable!(),
                    };
                    (Proj::Index(v), et)
                }
                PlaceElem::MapKey(e) => {
                    let v = self.expr(e);
                    let kty = self.ty(&e.ty);
                    let v = Op::Local(self.to_local(v, kty));
                    let vt = match &ty {
                        Ty::Adt(_, a) => a[1].clone(),
                        _ => unreachable!(),
                    };
                    (Proj::MapKey(v, write && i == n - 1), vt)
                }
            };
            path.push((proj, next.clone()));
            ty = next;
        }
        Place { root, path }
    }

    // ---- expressions ----

    fn expr(&mut self, e: &'a TExpr) -> Op {
        let ty = self.ty(&e.ty);
        match &e.kind {
            TK::Int(v) => Op::Int(*v),
            TK::Float(v) => Op::Float(*v),
            TK::Bool(b) => Op::Bool(*b),
            TK::Text(s) => Op::Text(s.clone()),
            TK::Unit => Op::Unit,
            TK::NoneLit => self.assign(ty, Rv::None),
            TK::Local(id) => match self.fbr().map.get(id) {
                Some(l) => {
                    let l = *l;
                    if self.fbr().f.locals[l].self_ptr {
                        // reading `self` in a mutating method copies the value
                        let p = Place { root: l, path: vec![] };
                        return self.assign(ty, Rv::Read(p));
                    }
                    Op::Local(l)
                }
                None => panic!("unmapped local {} ({})", id, self.fbr().body_locals[*id].name),
            },
            TK::FnRef(id, targs) => {
                let targs: Vec<Ty> = targs.iter().map(|t| self.ty(t)).collect();
                let thunk = self.fn_thunk(*id, targs, &ty);
                self.assign(ty, Rv::Closure(thunk, vec![]))
            }
            TK::Call { callee, args, throws } => self.call(callee, args, *throws, ty, None),
            TK::MutCall { fn_id, type_args, place, args, throws } => {
                let targs: Vec<Ty> = type_args.iter().map(|t| self.ty(t)).collect();
                let f = &self.prog.fns[*fn_id];
                let mut argv = vec![];
                for a in args {
                    let a = self.strip_adapter_if(f.rethrows, a);
                    let v = self.expr(a);
                    argv.push(v);
                }
                let rethrow = f.rethrows && *throws && !self.fbr().no_throw_fns;
                let throws = if f.rethrows { rethrow } else { *throws };
                let p = self.place(place, false);
                let callee = if f.intrinsic {
                    let pty = self.ty(&place.ty);
                    let mut it = vec![pty];
                    it.extend(targs.iter().skip(1).cloned());
                    MCallee::Intrinsic(intrinsic_name(self.prog, f), it)
                } else {
                    MCallee::Fn(self.instance(*fn_id, targs, rethrow))
                };
                let dst = if ty == Ty::Unit { None } else { Some(self.tmp(ty.clone())) };
                if throws {
                    let err = self.tmp(self.prog.error_ty());
                    self.emit(Stmt::MutCall { dst, err: Some(err), place: p, callee, args: argv });
                    self.propagate(err);
                } else {
                    self.emit(Stmt::MutCall { dst, err: None, place: p, callee, args: argv });
                }
                dst.map(Op::Local).unwrap_or(Op::Unit)
            }
            TK::IfaceMutCall { iface, method, place, args, throws } => {
                let recv_ty = self.ty(&place.ty);
                let ids = match &recv_ty {
                    Ty::Iface(ids) => ids.clone(),
                    _ => vec![*iface],
                };
                let slot = iface_methods(self.prog, &ids).iter().position(|m| m == method).unwrap();
                let mut argv = vec![];
                for a in args {
                    argv.push(self.expr(a));
                }
                let p = self.place(place, false);
                let dst = if ty == Ty::Unit { None } else { Some(self.tmp(ty.clone())) };
                if *throws {
                    let err = self.tmp(self.prog.error_ty());
                    self.emit(Stmt::MutCall { dst, err: Some(err), place: p, callee: MCallee::Iface(slot), args: argv });
                    self.propagate(err);
                } else {
                    self.emit(Stmt::MutCall { dst, err: None, place: p, callee: MCallee::Iface(slot), args: argv });
                }
                dst.map(Op::Local).unwrap_or(Op::Unit)
            }
            TK::Record { fields, .. } => {
                let ops: Vec<Op> = fields.iter().map(|f| self.expr(f)).collect();
                self.assign(ty, Rv::Record(ops))
            }
            TK::Variant { idx, fields, .. } => {
                let ops: Vec<Op> = fields.iter().map(|f| self.expr(f)).collect();
                self.assign(ty, Rv::Variant(*idx, ops))
            }
            TK::Wrap(inner) => {
                let ity = self.ty(&inner.ty);
                if let (Ty::Func(from), Ty::Func(to)) = (&ity, &ty) {
                    if !from.throws && to.throws {
                        let v = self.expr(inner);
                        let adapter = self.throws_adapter(&ity, &ty);
                        return self.assign(ty, Rv::Closure(adapter, vec![v]));
                    }
                }
                let v = self.expr(inner);
                self.assign(ty, Rv::Use(v))
            }
            TK::Unwrap(inner) => {
                let ity = self.ty(&inner.ty);
                let v = self.expr(inner);
                if let Ty::Opt(_) = ity {
                    self.set_line(e.span);
                    self.assign(ty, Rv::OptGet(v, true))
                } else {
                    self.assign(ty, Rv::Use(v))
                }
            }
            TK::Field(b, i) => {
                let v = self.expr(b);
                let bty = self.ty(&b.ty);
                let v = Op::Local(self.to_local(v, bty));
                self.assign(ty, Rv::Field(v, *i))
            }
            TK::Prop(b, name) => {
                let bty = self.ty(&b.ty);
                let v = self.expr(b);
                let owner = match &bty {
                    Ty::Text => "String".to_string(),
                    Ty::Adt(d, _) => {
                        let def = &self.prog.defs[*d];
                        if def.module != 0 {
                            format!("{}.{}", self.prog.module_names[def.module], def.name)
                        } else {
                            def.name.clone()
                        }
                    }
                    _ => unreachable!(),
                };
                self.assign(ty, Rv::Call(MCallee::Intrinsic(format!("{}.{}", owner, name), vec![bty]), vec![v]))
            }
            TK::Index(b, i) => {
                let bty = self.ty(&b.ty);
                let bv = self.expr(b);
                let iv = self.expr(i);
                self.set_line(e.span);
                let name = if matches!(&bty, Ty::Adt(d, _) if *d == self.prog.b.bytes) { "Bytes.get" } else { "List.get" };
                self.assign(ty, Rv::Call(MCallee::Intrinsic(name.into(), vec![bty]), vec![bv, iv]))
            }
            TK::MapGet(m, k) => {
                let mty = self.ty(&m.ty);
                let mv = self.expr(m);
                let kv = self.expr(k);
                self.assign(ty, Rv::Call(MCallee::Intrinsic("Map.get".into(), vec![mty]), vec![mv, kv]))
            }
            TK::Unary(op, x) => {
                let v = self.expr(x);
                self.set_line(e.span);
                self.assign(ty, Rv::Un(*op, v))
            }
            TK::Binary(op, a, b) => match op {
                BinOp::And | BinOp::Or => {
                    let res = self.tmp(Ty::Bool);
                    let av = self.expr(a);
                    let rhs = self.new_block();
                    let short = self.new_block();
                    let join = self.new_block();
                    if *op == BinOp::And {
                        self.term(Term::If(av, rhs, short));
                    } else {
                        self.term(Term::If(av, short, rhs));
                    }
                    self.switch_to(short);
                    self.emit(Stmt::Assign(res, Rv::Use(Op::Bool(*op == BinOp::Or))));
                    self.term(Term::Goto(join));
                    self.switch_to(rhs);
                    let bv = self.expr(b);
                    self.emit(Stmt::Assign(res, Rv::Use(bv)));
                    self.term(Term::Goto(join));
                    self.switch_to(join);
                    Op::Local(res)
                }
                _ => {
                    let aty = self.ty(&a.ty);
                    let av = self.expr(a);
                    let bv = self.expr(b);
                    self.set_line(e.span);
                    match op {
                        BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge => self.compare(*op, av, bv, &aty),
                        _ => self.assign(ty, Rv::Bin(*op, av, bv)),
                    }
                }
            },
            TK::Coalesce(a, b) => {
                let aty = self.ty(&a.ty);
                let av = self.expr(a);
                let al = self.to_local(av, aty);
                let res = self.tmp(ty.clone());
                let c = self.assign(Ty::Bool, Rv::OptIsSome(Op::Local(al)));
                let yes = self.new_block();
                let no = self.new_block();
                let join = self.new_block();
                self.term(Term::If(c, yes, no));
                self.switch_to(yes);
                if let Ty::Opt(_) = ty {
                    self.emit(Stmt::Assign(res, Rv::Use(Op::Local(al))));
                } else {
                    self.emit(Stmt::Assign(res, Rv::OptGet(Op::Local(al), false)));
                }
                self.term(Term::Goto(join));
                self.switch_to(no);
                let bv = self.expr(b);
                if !self.terminated() {
                    self.emit(Stmt::Assign(res, Rv::Use(bv)));
                    self.term(Term::Goto(join));
                }
                self.switch_to(join);
                Op::Local(res)
            }
            TK::Some(x) => {
                let v = self.expr(x);
                self.assign(ty, Rv::Some(v))
            }
            TK::ToIface(x) => {
                let xty = self.ty(&x.ty);
                let v = self.expr(x);
                let ids = match &ty {
                    Ty::Iface(ids) => ids.clone(),
                    _ => unreachable!(),
                };
                match &xty {
                    Ty::Iface(from) => {
                        // between interface combinations: the object stays,
                        // the vtable is looked up by its type
                        if !self.upcasts.contains(&(from.clone(), ids.clone())) {
                            self.upcasts.push((from.clone(), ids.clone()));
                        }
                        self.assign(ty.clone(), Rv::Call(MCallee::Intrinsic("iface_upcast".into(), vec![xty.clone(), ty.clone()]), vec![v]))
                    }
                    _ => {
                        self.vtable(xty.clone(), ids);
                        self.assign(ty, Rv::ToIface(v, xty))
                    }
                }
            }
            TK::Lambda { params, captures, body } => self.lambda(params, captures, body, &ty, e.span),
            TK::Block(b) => self.block(b),
            TK::If { cond, then, els } => {
                self.set_line(cond.span);
                let c = self.expr(cond);
                let tb = self.new_block();
                let eb = self.new_block();
                let join = self.new_block();
                let res = if ty != Ty::Unit && ty != Ty::Never { Some(self.tmp(ty.clone())) } else { None };
                self.term(Term::If(c, tb, eb));
                self.switch_to(tb);
                let v = self.block(then);
                if !self.terminated() {
                    if let Some(r) = res {
                        self.emit(Stmt::Assign(r, Rv::Use(v)));
                    }
                    self.term(Term::Goto(join));
                }
                self.switch_to(eb);
                if let Some(x) = els {
                    let v = self.expr(x);
                    if !self.terminated() {
                        if let Some(r) = res {
                            self.emit(Stmt::Assign(r, Rv::Use(v)));
                        }
                    }
                }
                self.term(Term::Goto(join));
                self.switch_to(join);
                res.map(Op::Local).unwrap_or(Op::Unit)
            }
            TK::Match { scrut, arms } => {
                self.set_line(scrut.span);
                let sty = self.ty(&scrut.ty);
                let sv = self.expr(scrut);
                let sl = Op::Local(self.to_local(sv, sty.clone()));
                let res = if ty != Ty::Unit && ty != Ty::Never { Some(self.tmp(ty.clone())) } else { None };
                let join = self.new_block();
                for a in arms {
                    let next = self.new_block();
                    self.test_pat(&a.pat, sl.clone(), &sty, next);
                    if let Some(g) = &a.guard {
                        let gv = self.expr(g);
                        let yes = self.new_block();
                        self.term(Term::If(gv, yes, next));
                        self.switch_to(yes);
                    }
                    self.set_line(a.body.span);
                    let v = self.expr(&a.body);
                    if !self.terminated() {
                        if let Some(r) = res {
                            self.emit(Stmt::Assign(r, Rv::Use(v)));
                        }
                        self.term(Term::Goto(join));
                    }
                    self.switch_to(next);
                }
                // no arm matched: the checker proved this can't happen
                self.term(Term::Unreachable);
                self.switch_to(join);
                res.map(Op::Local).unwrap_or(Op::Unit)
            }
            TK::Is(x, pat) => {
                let xty = self.ty(&x.ty);
                let v = self.expr(x);
                let xl = Op::Local(self.to_local(v, xty.clone()));
                let res = self.tmp(Ty::Bool);
                let fail = self.new_block();
                let join = self.new_block();
                self.test_pat(pat, xl, &xty, fail);
                self.emit(Stmt::Assign(res, Rv::Use(Op::Bool(true))));
                self.term(Term::Goto(join));
                self.switch_to(fail);
                self.emit(Stmt::Assign(res, Rv::Use(Op::Bool(false))));
                self.term(Term::Goto(join));
                self.switch_to(join);
                Op::Local(res)
            }
            TK::Try { call, catch } => match catch {
                None => self.expr(call),
                Some((err_id, blk)) => self.try_catch(call, *err_id, blk, ty),
            },
            TK::ExpectThrows(call) => self.expect_throws(call, e.span),
            TK::Spawn(call) => self.spawn(call, ty),
            TK::Interp(parts) => {
                let ops: Vec<Op> = parts.iter().map(|p| self.expr(p)).collect();
                self.assign(Ty::Text, Rv::Call(MCallee::Intrinsic("String.concat".into(), vec![]), ops))
            }
            TK::ToText(x) => {
                let xty = self.ty(&x.ty);
                let v = self.expr(x);
                self.assign(Ty::Text, Rv::Call(MCallee::Intrinsic("to_text".into(), vec![xty]), vec![v]))
            }
            TK::List(items) => {
                let ops: Vec<Op> = items.iter().map(|i| self.expr(i)).collect();
                self.assign(ty, Rv::List(ops))
            }
            TK::Map(entries) => {
                let ops: Vec<(Op, Op)> = entries.iter().map(|(k, v)| (self.expr(k), self.expr(v))).collect();
                self.assign(ty, Rv::Map(ops))
            }
            TK::EmptySet => self.assign(ty, Rv::EmptySet),
            TK::Diverge(s) => {
                self.stmt(s);
                Op::Unit
            }
        }
    }

    // `a op b` for comparisons; ==/!= work on every type, ordering on
    // numbers and text.
    fn compare(&mut self, op: BinOp, a: Op, b: Op, ty: &Ty) -> Op {
        match ty {
            Ty::Int | Ty::Float | Ty::Bool => self.assign(Ty::Bool, Rv::Bin(op, a, b)),
            _ => match op {
                BinOp::Eq => self.assign(Ty::Bool, Rv::Eq(a, b)),
                BinOp::Ne => {
                    let e = self.assign(Ty::Bool, Rv::Eq(a, b));
                    self.assign(Ty::Bool, Rv::Un(UnOp::Not, e))
                }
                _ => {
                    let c = self.assign(Ty::Int, Rv::Cmp(a, b));
                    self.assign(Ty::Bool, Rv::Bin(op, c, Op::Int(0)))
                }
            },
        }
    }

    // Jumps to `fail` when `v` doesn't match; binds names on success.
    fn test_pat(&mut self, pat: &TPat, v: Op, ty: &Ty, fail: B) {
        let next = |s: &mut Self, c: Op| {
            let ok = s.new_block();
            s.term(Term::If(c, ok, fail));
            s.switch_to(ok);
        };
        match pat {
            TPat::Wild => {}
            TPat::Or(alts) => {
                // try each; the first that fits goes on, the last one's
                // failure is the pattern's
                let ok = self.new_block();
                for (i, alt) in alts.iter().enumerate() {
                    let miss = if i + 1 < alts.len() { self.new_block() } else { fail };
                    self.test_pat(alt, v.clone(), ty, miss);
                    self.term(Term::Goto(ok));
                    if i + 1 < alts.len() {
                        self.switch_to(miss);
                    }
                }
                self.switch_to(ok);
            }
            TPat::Bind(id) => {
                let lty = self.local_ty(*id);
                let name = self.fbr().body_locals[*id].name.clone();
                let l = self.new_local(lty, &name);
                self.fb().map.insert(*id, l);
                let rv = match v {
                    Op::Local(src) => Rv::Read(Place { root: src, path: vec![] }),
                    o => Rv::Use(o),
                };
                self.emit(Stmt::Assign(l, rv));
            }
            TPat::Int(i) => {
                let c = self.assign(Ty::Bool, Rv::Bin(BinOp::Eq, v, Op::Int(*i)));
                next(self, c);
            }
            TPat::Float(f) => {
                let c = self.assign(Ty::Bool, Rv::Bin(BinOp::Eq, v, Op::Float(*f)));
                next(self, c);
            }
            TPat::Bool(b) => {
                let c = self.assign(Ty::Bool, Rv::Bin(BinOp::Eq, v, Op::Bool(*b)));
                next(self, c);
            }
            TPat::Text(s) => {
                let c = self.assign(Ty::Bool, Rv::Eq(v, Op::Text(s.clone())));
                next(self, c);
            }
            TPat::None => {
                let c = self.assign(Ty::Bool, Rv::OptIsSome(v));
                let ok = self.new_block();
                self.term(Term::If(c, fail, ok));
                self.switch_to(ok);
            }
            TPat::Some(p) => {
                let c = self.assign(Ty::Bool, Rv::OptIsSome(v.clone()));
                next(self, c);
                if !matches!(**p, TPat::Wild) {
                    let inner = match ty {
                        Ty::Opt(t) => (**t).clone(),
                        _ => unreachable!(),
                    };
                    let x = self.assign(inner.clone(), Rv::OptGet(v, false));
                    self.test_pat(p, x, &inner, fail);
                }
            }
            TPat::Variant { idx, args } => {
                let (d, targs) = match ty {
                    Ty::Adt(d, a) => (*d, a.clone()),
                    _ => unreachable!(),
                };
                let variants = match &self.prog.defs[d].kind {
                    TypeKind::Enum { variants } => variants,
                    _ => unreachable!(),
                };
                if variants.len() > 1 {
                    let t = self.assign(Ty::Int, Rv::Tag(v.clone()));
                    let c = self.assign(Ty::Bool, Rv::Bin(BinOp::Eq, t, Op::Int(*idx as i64)));
                    next(self, c);
                }
                for (i, a) in args.iter().enumerate() {
                    if matches!(a, TPat::Wild) {
                        continue;
                    }
                    let fty = variants[*idx].fields[i].ty.subst(&targs);
                    let x = self.assign(fty.clone(), Rv::VariantField(v.clone(), *idx, i));
                    self.test_pat(a, x, &fty, fail);
                }
            }
            TPat::Record { args } => {
                for (i, a) in args.iter().enumerate() {
                    if matches!(a, TPat::Wild) {
                        continue;
                    }
                    let fty = field_ty(self.prog, ty, i);
                    let x = self.assign(fty.clone(), Rv::Field(v.clone(), i));
                    self.test_pat(a, x, &fty, fail);
                }
            }
            TPat::Type { def, targs, args, bind } => {
                let rt = Ty::Adt(*def, targs.clone());
                let c = self.assign(Ty::Bool, Rv::IfaceIs(v.clone(), rt.clone()));
                next(self, c);
                let needs = bind.is_some() || args.iter().any(|a| !matches!(a, TPat::Wild));
                if needs {
                    let x = match bind {
                        Some(id) => {
                            let name = self.fbr().body_locals[*id].name.clone();
                            let l = self.new_local(rt.clone(), &name);
                            self.fb().map.insert(*id, l);
                            self.emit(Stmt::Assign(l, Rv::IfaceGet(v, rt.clone())));
                            Op::Local(l)
                        }
                        None => self.assign(rt.clone(), Rv::IfaceGet(v, rt.clone())),
                    };
                    for (i, a) in args.iter().enumerate() {
                        if matches!(a, TPat::Wild) {
                            continue;
                        }
                        let fty = field_ty(self.prog, &rt, i);
                        let y = self.assign(fty.clone(), Rv::Field(x.clone(), i));
                        self.test_pat(a, y, &fty, fail);
                    }
                }
            }
        }
    }

    // ---- calls ----

    fn strip_adapter_if(&self, rethrows: bool, a: &'a TExpr) -> &'a TExpr {
        if rethrows {
            if let TK::Wrap(inner) = &a.kind {
                if matches!(inner.ty, Ty::Func(_)) && matches!(a.ty, Ty::Func(_)) {
                    return inner;
                }
            }
        }
        a
    }

    fn call(&mut self, callee: &'a Callee, args: &'a [TExpr], throws: bool, ty: Ty, _hint: Option<()>) -> Op {
        match callee {
            Callee::Fn(id, targs) => {
                let f = &self.prog.fns[*id];
                // a call that never returns keeps that, even where the checker
                // gave it the type of its surroundings (`catch err { process.exit(2) }`)
                let ty = if f.ret == Ty::Never { Ty::Never } else { ty };
                let targs: Vec<Ty> = targs.iter().map(|t| self.ty(t)).collect();
                let mut argv = vec![];
                let mut arg_tys = vec![];
                for a in args {
                    let a = self.strip_adapter_if(f.rethrows, a);
                    arg_tys.push(self.ty(&a.ty));
                    argv.push(self.expr(a));
                }
                if f.intrinsic {
                    return self.intrinsic(*id, targs, argv, arg_tys, ty, throws);
                }
                let rethrow = f.rethrows && arg_tys.iter().any(|t| matches!(t, Ty::Func(ft) if ft.throws));
                let throws = if f.rethrows { rethrow } else { throws };
                let idx = self.instance(*id, targs, rethrow);
                self.emit_call(MCallee::Fn(idx), argv, ty, throws)
            }
            Callee::Value(fv) => {
                let fty = self.ty(&fv.ty);
                let f = self.expr(fv);
                let f = Op::Local(self.to_local(f, fty.clone()));
                let argv: Vec<Op> = args.iter().map(|a| self.expr(a)).collect();
                let throws = match &fty {
                    Ty::Func(ft) => ft.throws,
                    _ => throws,
                };
                self.emit_call(MCallee::Closure(f), argv, ty, throws)
            }
            Callee::Iface { method, .. } => {
                let rty = self.ty(&args[0].ty);
                let ids = match &rty {
                    Ty::Iface(ids) => ids.clone(),
                    _ => unreachable!(),
                };
                let slot = iface_methods(self.prog, &ids).iter().position(|m| m == method).unwrap();
                let mut argv = vec![];
                for a in args {
                    argv.push(self.expr(a));
                }
                argv[0] = Op::Local(self.to_local(argv[0].clone(), rty));
                let throws = self.prog.fns[*method].throws;
                self.emit_call(MCallee::Iface(slot), argv, ty, throws)
            }
        }
    }

    fn emit_call(&mut self, callee: MCallee, args: Vec<Op>, ty: Ty, throws: bool) -> Op {
        if throws {
            let dst = if ty == Ty::Unit || ty == Ty::Never { None } else { Some(self.tmp(ty.clone())) };
            let err = self.tmp(self.prog.error_ty());
            self.emit(Stmt::CallT { dst, err, callee, args });
            self.propagate(err);
            if ty == Ty::Never {
                self.term(Term::Unreachable);
            }
            dst.map(Op::Local).unwrap_or(Op::Unit)
        } else {
            let r = self.assign(ty.clone(), Rv::Call(callee, args));
            if ty == Ty::Never {
                self.term(Term::Unreachable);
                return Op::Unit;
            }
            if ty == Ty::Unit {
                return Op::Unit;
            }
            r
        }
    }

    // After a throwing call: on error, leave the function with the error.
    // `try ... catch` redirects this through `catch_target`.
    fn propagate(&mut self, err: L) {
        let bad = self.new_block();
        let ok = self.new_block();
        self.term(Term::IfErr(err, bad, ok));
        self.switch_to(bad);
        match self.fbr().catch_targets.last().copied() {
            Some((target, cerr)) => {
                self.emit(Stmt::Assign(cerr, Rv::Use(Op::Local(err))));
                self.term(Term::Goto(target));
            }
            None => self.throw(Op::Local(err)),
        }
        self.switch_to(ok);
    }

    fn try_catch(&mut self, call: &'a TExpr, err_id: LocalId, blk: &'a TBlock, ty: Ty) -> Op {
        let catch_b = self.new_block();
        let name = self.fbr().body_locals[err_id].name.clone();
        let el = self.new_local(self.prog.error_ty(), &name);
        self.fb().map.insert(err_id, el);
        self.fb().catch_targets.push((catch_b, el));
        let v = self.expr(call);
        self.fb().catch_targets.pop();
        let res = if ty != Ty::Unit && ty != Ty::Never { Some(self.tmp(ty.clone())) } else { None };
        let join = self.new_block();
        if !self.terminated() {
            if let Some(r) = res {
                self.emit(Stmt::Assign(r, Rv::Use(v)));
            }
            self.term(Term::Goto(join));
        }
        self.switch_to(catch_b);
        let cv = self.block(blk);
        if !self.terminated() {
            if let Some(r) = res {
                self.emit(Stmt::Assign(r, Rv::Use(cv)));
            }
            self.term(Term::Goto(join));
        }
        self.switch_to(join);
        res.map(Op::Local).unwrap_or(Op::Unit)
    }

    fn expect_throws(&mut self, call: &'a TExpr, span: Span) -> Op {
        let catch_b = self.new_block();
        let res = self.tmp(self.prog.error_ty());
        self.fb().catch_targets.push((catch_b, res));
        let _ = self.expr(call);
        self.fb().catch_targets.pop();
        let join = self.new_block();
        // no error: the expectation fails
        let text = self.src_text(call.span);
        let (line, _) = self.src.line_col(span);
        let e = self.assign(self.prog.error_ty(), Rv::Call(MCallee::Intrinsic("expect_throws_failed".into(), vec![]), vec![Op::Text(text), Op::Int(line as i64)]));
        self.throw(e);
        self.switch_to(catch_b);
        self.term(Term::Goto(join));
        self.switch_to(join);
        Op::Local(res)
    }

    fn intrinsic(&mut self, id: FnId, targs: Vec<Ty>, args: Vec<Op>, arg_tys: Vec<Ty>, ty: Ty, throws: bool) -> Op {
        let f = &self.prog.fns[id];
        let name = intrinsic_name(self.prog, f);
        // the receiver's type first, then the function's own type arguments
        let mut itys = vec![];
        if f.owner.is_some() {
            itys.push(arg_tys[0].clone());
            let n_owner = self.prog.defs[f.owner.unwrap()].generics.len();
            itys.extend(targs.iter().skip(n_owner).cloned());
        } else {
            itys.extend(targs.iter().cloned());
            if targs.is_empty() {
                itys.extend(arg_tys.iter().cloned());
            }
        }
        if matches!(name.as_str(), "json.decode" | "env.decode" | "cli.decode" | "cli.decode_from" | "db.Connection.query" | "csv.decode" | "sql.decode") {
            if let Some(t) = itys.last().cloned() {
                self.ensure_defaults(&t);
            }
        }
        let panics = matches!(name.as_str(), "assert" | "Int.div" | "panic" | "String.slice" | "List.get");
        if panics {
            // the panic message needs the line; `Line` markers carry it
        }
        if name == "panic" {
            let _ = self.emit_call(MCallee::Intrinsic(name, itys), args, Ty::Unit, false);
            self.term(Term::Unreachable);
            return Op::Unit;
        }
        let throws = throws && f.throws && !f.rethrows || (f.rethrows && arg_tys.iter().any(|t| matches!(t, Ty::Func(ft) if ft.throws)));
        self.emit_call(MCallee::Intrinsic(name, itys), args, ty, throws)
    }

    // ---- field defaults for decoders ----

    // Every record reachable from `t` that has field defaults gets a function
    // that builds it with those defaults (other fields zero).
    fn ensure_defaults(&mut self, t: &Ty) {
        if !self.decoded.insert(t.clone()) {
            return;
        }
        match t {
            Ty::Opt(x) => self.ensure_defaults(x),
            Ty::Adt(d, args) => {
                let def = &self.prog.defs[*d];
                match &def.kind {
                    TypeKind::Builtin | TypeKind::Newtype(_) => {
                        let inner: Vec<Ty> = match &def.kind {
                            TypeKind::Newtype(i) => vec![i.clone()],
                            _ => args.clone(),
                        };
                        for a in inner {
                            self.ensure_defaults(&a);
                        }
                    }
                    TypeKind::Enum { variants } => {
                        let fts: Vec<Ty> = variants.iter().flat_map(|v| v.fields.iter().map(|f| f.ty.subst(args))).collect();
                        for f in fts {
                            self.ensure_defaults(&f);
                        }
                    }
                    TypeKind::Record { fields } => {
                        let fts: Vec<Ty> = fields.iter().map(|f| f.ty.subst(args)).collect();
                        let has_defaults = fields.iter().any(|f| f.default.is_some());
                        for f in fts {
                            self.ensure_defaults(&f);
                        }
                        if has_defaults {
                            let idx = self.build_defaults(*d, args.clone());
                            self.default_fns.push((t.clone(), idx));
                        }
                    }
                    TypeKind::Interface { .. } => {}
                }
            }
            _ => {}
        }
    }

    fn build_defaults(&mut self, d: DefId, args: Vec<Ty>) -> usize {
        let prog = self.prog;
        let fields = match &prog.defs[d].kind {
            TypeKind::Record { fields } => fields,
            _ => unreachable!(),
        };
        let ty = Ty::Adt(d, args.clone());
        let func = Func { name: format!("defaults{}", self.funcs.len()), kind: FnKind::Normal, params: vec![], ret: ty.clone(), throws: false, mutating: false, locals: vec![], blocks: vec![], source_name: format!("defaults of {}", prog.defs[d].name) };
        static NO_LOCALS: [LocalDef; 0] = [];
        self.fbs.push(Fb { f: func, cur: 0, map: HashMap::new(), subst: args, no_throw_fns: false, body_locals: &NO_LOCALS, loops: vec![], line: 0, closed: vec![], cleanups: vec![], scope: None, catch_targets: vec![] });
        self.new_block();
        let mut ops = vec![];
        for f in fields {
            let fty = self.ty(&f.ty);
            match &f.default {
                Some(e) => ops.push(self.expr(e)),
                None => ops.push(self.assign(fty, Rv::Zero)),
            }
        }
        let r = self.assign(ty, Rv::Record(ops));
        self.term(Term::Return(r));
        let f = self.fbs.pop().unwrap().f;
        self.funcs.push(f);
        self.funcs.len() - 1
    }

    // ---- closures ----

    fn lambda(&mut self, params: &'a [LocalId], captures: &'a [LocalId], body: &'a TExpr, fty: &Ty, _span: Span) -> Op {
        let ft = match fty {
            Ty::Func(f) => (**f).clone(),
            _ => unreachable!(),
        };
        // capture values in the parent
        let mut cap_ops = vec![];
        let mut cap_tys = vec![];
        for c in captures {
            let l = self.fbr().map[c];
            let lt = self.fbr().f.locals[l].ty.clone();
            if self.fbr().f.locals[l].self_ptr {
                let v = self.assign(lt.clone(), Rv::Read(Place { root: l, path: vec![] }));
                cap_ops.push(v);
            } else {
                cap_ops.push(Op::Local(l));
            }
            cap_tys.push(lt);
        }
        let parent = self.fbr();
        let subst = parent.subst.clone();
        let no_throw = parent.no_throw_fns;
        let body_locals = parent.body_locals;
        let pname = parent.f.name.clone();
        let src_name = parent.f.source_name.clone();
        let idx = self.funcs.len();
        let name = format!("{}_lambda{}", pname, idx);
        self.funcs.push(Func { name: name.clone(), kind: FnKind::Normal, params: vec![], ret: Ty::Unit, throws: false, mutating: false, locals: vec![], blocks: vec![], source_name: src_name.clone() });
        let func = Func { name, kind: FnKind::Closure(cap_tys.clone()), params: vec![], ret: ft.ret.clone(), throws: ft.throws, mutating: false, locals: vec![], blocks: vec![], source_name: format!("lambda in {}", src_name) };
        let line = self.fbr().line;
        self.fbs.push(Fb { f: func, cur: 0, map: HashMap::new(), subst, no_throw_fns: no_throw, body_locals, loops: vec![], line, closed: vec![], cleanups: vec![], scope: None, catch_targets: vec![] });
        self.new_block();
        self.switch_to(0);
        for p in params {
            let ty = self.local_ty(*p);
            let name = body_locals[*p].name.clone();
            let l = self.new_local(ty, &name);
            self.fb().map.insert(*p, l);
            self.fb().f.params.push(l);
        }
        for (i, c) in captures.iter().enumerate() {
            let name = body_locals[*c].name.clone();
            let l = self.new_local(cap_tys[i].clone(), &name);
            self.fb().map.insert(*c, l);
            self.emit(Stmt::Assign(l, Rv::EnvField(i)));
        }
        let v = self.expr(body);
        if !self.terminated() {
            self.term(Term::Return(v));
        }
        let f = self.fbs.pop().unwrap().f;
        self.funcs[idx] = f;
        self.assign(fty.clone(), Rv::Closure(idx, cap_ops))
    }

    // `spawn f(args)`: the arguments are evaluated here, the call runs in a
    // new task. The call is wrapped in a closure over the argument values.
    fn spawn(&mut self, call: &'a TExpr, task_ty: Ty) -> Op {
        let rty = self.ty(&call.ty);
        let (callee, args, throws) = match &call.kind {
            TK::Call { callee, args, throws } => (callee, args, *throws),
            _ => unreachable!(),
        };
        // what the thunk calls, and the values it captures
        let mut caps: Vec<Op> = vec![];
        let mut cap_tys: Vec<Ty> = vec![];
        let (mcallee, n_fixed, throws) = match callee {
            Callee::Fn(id, targs) => {
                let f = &self.prog.fns[*id];
                let targs: Vec<Ty> = targs.iter().map(|x| self.ty(x)).collect();
                let mut arg_tys = vec![];
                for a in args {
                    let a = self.strip_adapter_if(f.rethrows, a);
                    let at = self.ty(&a.ty);
                    let v = self.expr(a);
                    caps.push(v);
                    cap_tys.push(at.clone());
                    arg_tys.push(at);
                }
                if f.intrinsic {
                    let name = intrinsic_name(self.prog, f);
                    let mut itys = vec![];
                    if f.owner.is_some() {
                        itys.push(arg_tys[0].clone());
                        let n_owner = self.prog.defs[f.owner.unwrap()].generics.len();
                        itys.extend(targs.iter().skip(n_owner).cloned());
                    } else {
                        itys.extend(targs.iter().cloned());
                        if targs.is_empty() {
                            itys.extend(arg_tys.iter().cloned());
                        }
                    }
                    (MCallee::Intrinsic(name, itys), 0, throws && f.throws)
                } else {
                    let rethrow = f.rethrows && arg_tys.iter().any(|t| matches!(t, Ty::Func(ft) if ft.throws));
                    let throws = if f.rethrows { rethrow } else { throws };
                    (MCallee::Fn(self.instance(*id, targs, rethrow)), 0, throws)
                }
            }
            Callee::Value(fv) => {
                let fty = self.ty(&fv.ty);
                let v = self.expr(fv);
                caps.push(v);
                cap_tys.push(fty.clone());
                for a in args {
                    let at = self.ty(&a.ty);
                    caps.push(self.expr(a));
                    cap_tys.push(at);
                }
                let throws = matches!(&fty, Ty::Func(ft) if ft.throws);
                (MCallee::Closure(Op::Local(0)), 1, throws)
            }
            Callee::Iface { method, .. } => {
                let rty0 = self.ty(&args[0].ty);
                let ids = match &rty0 {
                    Ty::Iface(ids) => ids.clone(),
                    _ => unreachable!(),
                };
                let slot = iface_methods(self.prog, &ids).iter().position(|m| m == method).unwrap();
                for a in args {
                    let at = self.ty(&a.ty);
                    caps.push(self.expr(a));
                    cap_tys.push(at);
                }
                (MCallee::Iface(slot), 0, self.prog.fns[*method].throws)
            }
        };
        // the thunk: load the captures, make the call, return its result
        let mut func = Func {
            name: format!("spawn{}", self.funcs.len()),
            kind: FnKind::Closure(cap_tys.clone()),
            params: vec![],
            ret: rty.clone(),
            throws,
            mutating: false,
            locals: vec![],
            blocks: vec![],
            source_name: format!("task in {}", self.fbr().f.source_name),
        };
        let mut stmts = vec![];
        for (i, ct) in cap_tys.iter().enumerate() {
            func.locals.push(mir::Local { ty: ct.clone(), name: format!("c{}", i), self_ptr: false });
            stmts.push(Stmt::Assign(i, Rv::EnvField(i)));
        }
        let call_args: Vec<Op> = (n_fixed..cap_tys.len()).map(Op::Local).collect();
        let mut blocks = vec![];
        if throws {
            let dst = if rty == Ty::Unit { None } else { Some(func.locals.len()) };
            if dst.is_some() {
                func.locals.push(mir::Local { ty: rty.clone(), name: "r".into(), self_ptr: false });
            }
            let err = func.locals.len();
            func.locals.push(mir::Local { ty: self.prog.error_ty(), name: "e".into(), self_ptr: false });
            stmts.push(Stmt::CallT { dst, err, callee: mcallee, args: call_args });
            blocks.push(mir::Block { stmts, term: Term::IfErr(err, 1, 2), line: 0 });
            blocks.push(mir::Block { stmts: vec![], term: Term::Throw(Op::Local(err)), line: 0 });
            blocks.push(mir::Block { stmts: vec![], term: Term::Return(dst.map(Op::Local).unwrap_or(Op::Unit)), line: 0 });
        } else {
            let r = func.locals.len();
            func.locals.push(mir::Local { ty: rty.clone(), name: "r".into(), self_ptr: false });
            stmts.push(Stmt::Assign(r, Rv::Call(mcallee, call_args)));
            blocks.push(mir::Block { stmts, term: Term::Return(if rty == Ty::Unit { Op::Unit } else { Op::Local(r) }), line: 0 });
        }
        func.blocks = blocks;
        self.funcs.push(func);
        let thunk = self.funcs.len() - 1;
        let fn_ty = Ty::func(vec![], rty.clone(), throws);
        let clo = self.assign(fn_ty, Rv::Closure(thunk, caps));
        let scope = self.fbr().scope.expect("spawn outside a scope");
        self.assign(task_ty, Rv::Call(MCallee::Intrinsic("spawn".into(), vec![rty, Ty::Bool]), vec![Op::Local(scope), clo, Op::Bool(throws)]))
    }

    // A named function used as a value.
    fn fn_thunk(&mut self, id: FnId, targs: Vec<Ty>, fty: &Ty) -> usize {
        if let Some(t) = self.thunks.get(&(id, targs.clone())) {
            return *t;
        }
        let ft = match fty {
            Ty::Func(f) => (**f).clone(),
            _ => unreachable!(),
        };
        let f = &self.prog.fns[id];
        let target = self.instance(id, targs.clone(), false);
        let _ = f;
        let mut func = Func {
            name: format!("thunk{}_{}", self.funcs.len(), sanitize(&self.prog.fns[id].name)),
            kind: FnKind::Closure(vec![]),
            params: vec![],
            ret: ft.ret.clone(),
            throws: ft.throws,
            mutating: false,
            locals: vec![],
            blocks: vec![],
            source_name: self.prog.fns[id].name.clone(),
        };
        let mut args = vec![];
        for (i, p) in ft.params.iter().enumerate() {
            func.locals.push(mir::Local { ty: p.clone(), name: format!("a{}", i), self_ptr: false });
            func.params.push(i);
            args.push(Op::Local(i));
        }
        let mut stmts = vec![];
        let term;
        let blocks_extra;
        if ft.throws {
            let dst = if ft.ret == Ty::Unit { None } else { Some(func.locals.len()) };
            if dst.is_some() {
                func.locals.push(mir::Local { ty: ft.ret.clone(), name: "r".into(), self_ptr: false });
            }
            let err = func.locals.len();
            func.locals.push(mir::Local { ty: self.prog.error_ty(), name: "e".into(), self_ptr: false });
            stmts.push(Stmt::CallT { dst, err, callee: MCallee::Fn(target), args });
            term = Term::IfErr(err, 1, 2);
            blocks_extra = vec![
                mir::Block { stmts: vec![], term: Term::Throw(Op::Local(err)), line: 0 },
                mir::Block { stmts: vec![], term: Term::Return(dst.map(Op::Local).unwrap_or(Op::Unit)), line: 0 },
            ];
        } else {
            let r = func.locals.len();
            func.locals.push(mir::Local { ty: ft.ret.clone(), name: "r".into(), self_ptr: false });
            stmts.push(Stmt::Assign(r, Rv::Call(MCallee::Fn(target), args)));
            term = Term::Return(if ft.ret == Ty::Unit { Op::Unit } else { Op::Local(r) });
            blocks_extra = vec![];
        }
        func.blocks.push(mir::Block { stmts, term, line: 0 });
        func.blocks.extend(blocks_extra);
        self.funcs.push(func);
        let idx = self.funcs.len() - 1;
        self.thunks.insert((id, targs), idx);
        idx
    }

    // Wraps a non-throwing function value so it can be passed where a
    // throwing one is expected.
    fn throws_adapter(&mut self, from: &Ty, to: &Ty) -> usize {
        let ft = match to {
            Ty::Func(f) => (**f).clone(),
            _ => unreachable!(),
        };
        let mut func = Func {
            name: format!("adapter{}", self.funcs.len()),
            kind: FnKind::Closure(vec![from.clone()]),
            params: vec![],
            ret: ft.ret.clone(),
            throws: true,
            mutating: false,
            locals: vec![],
            blocks: vec![],
            source_name: "adapter".into(),
        };
        let mut args = vec![];
        for (i, p) in ft.params.iter().enumerate() {
            func.locals.push(mir::Local { ty: p.clone(), name: format!("a{}", i), self_ptr: false });
            func.params.push(i);
            args.push(Op::Local(i));
        }
        let inner = func.locals.len();
        func.locals.push(mir::Local { ty: from.clone(), name: "inner".into(), self_ptr: false });
        let r = func.locals.len();
        func.locals.push(mir::Local { ty: ft.ret.clone(), name: "r".into(), self_ptr: false });
        func.blocks.push(mir::Block {
            stmts: vec![Stmt::Assign(inner, Rv::EnvField(0)), Stmt::Assign(r, Rv::Call(MCallee::Closure(Op::Local(inner)), args))],
            term: Term::Return(if ft.ret == Ty::Unit { Op::Unit } else { Op::Local(r) }),
            line: 0,
        });
        self.funcs.push(func);
        self.funcs.len() - 1
    }
}

pub fn field_ty(prog: &Program, ty: &Ty, i: usize) -> Ty {
    match ty {
        Ty::Adt(d, args) => match &prog.defs[*d].kind {
            TypeKind::Record { fields } => fields[i].ty.subst(args),
            _ => panic!("field of a non-record"),
        },
        _ => panic!("field of a non-record type"),
    }
}

pub fn strip_throws(t: &Ty) -> Ty {
    match t {
        Ty::Func(f) => Ty::func(f.params.iter().map(strip_throws).collect(), strip_throws(&f.ret), false),
        Ty::Adt(d, a) => Ty::Adt(*d, a.iter().map(strip_throws).collect()),
        Ty::Opt(x) => Ty::opt(strip_throws(x)),
        t => t.clone(),
    }
}

pub fn sanitize(s: &str) -> String {
    s.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect()
}

// "print", "List.append", "files.read", "net.Connection.read"
pub fn intrinsic_name(prog: &Program, f: &FnDef) -> String {
    match f.owner {
        Some(d) => {
            let def = &prog.defs[d];
            if def.module != 0 {
                format!("{}.{}.{}", prog.module_names[def.module], def.name, f.name)
            } else {
                format!("{}.{}", def.name, f.name)
            }
        }
        None if f.module != 0 => format!("{}.{}", prog.module_names[f.module], f.name),
        None => f.name.clone(),
    }
}
