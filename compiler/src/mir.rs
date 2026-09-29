// The mid-level IR: a control-flow graph of basic blocks per function, with
// every intermediate value in a numbered local. All types are concrete
// (generics are instantiated).
//
// Ownership: every local owns its value. Reference counting operations
// (`Dup`, `Drop`) are not produced by lowering; the `rc` pass inserts them
// from liveness. Each operand use either consumes (moves) its local or
// borrows it; see `uses`.

use crate::ast::{BinOp, UnOp};
use crate::types::{DefId, Ty};

pub type L = usize;
pub type B = usize;
pub type FnIdx = usize;

#[derive(Clone, Debug)]
pub enum Op {
    Local(L),
    Int(i64),
    Float(f64),
    Bool(bool),
    Text(String),
    Unit,
}

#[derive(Clone, Debug)]
pub enum Proj {
    Field(usize),
    Index(Op),
    // `insert`: the last step of `m[k] = v` adds the key if it's missing
    MapKey(Op, bool),
}

#[derive(Clone, Debug)]
pub struct Place {
    pub root: L,
    pub path: Vec<(Proj, Ty)>, // each step with the type of the value it reaches
}

#[derive(Clone, Debug)]
pub enum Callee {
    Fn(FnIdx),
    // a closure value (borrowed)
    Closure(Op),
    // interface method slot; the receiver (borrowed) is the first argument
    Iface(usize),
    // implemented by the code generator; arguments are borrowed unless
    // `consumes_args` says otherwise
    Intrinsic(String, Vec<Ty>),
}

#[derive(Clone, Debug)]
pub enum Rv {
    Use(Op),
    // borrowed reads that produce a new reference
    Read(Place),
    Field(Op, usize),
    Tag(Op),
    VariantField(Op, usize, usize),
    OptIsSome(Op),
    OptGet(Op, bool), // bool: panic if missing
    IfaceIs(Op, Ty),
    IfaceGet(Op, Ty),
    EnvField(usize),
    // operators (operands borrowed)
    Bin(BinOp, Op, Op),
    Un(UnOp, Op),
    Eq(Op, Op),
    Cmp(Op, Op), // -1 / 0 / 1 for orderable values
    // constructors (operands consumed)
    Record(Vec<Op>),
    Variant(usize, Vec<Op>),
    Some(Op),
    None,
    List(Vec<Op>),
    Map(Vec<(Op, Op)>),
    EmptySet,
    Closure(FnIdx, Vec<Op>),
    ToIface(Op, Ty), // from type
    // calls
    Call(Callee, Vec<Op>),
}

#[derive(Clone, Debug)]
pub enum Stmt {
    Assign(L, Rv),
    // a throwing call: on success `dst` gets the result and `err` stays
    // empty; on failure `err` gets the error
    CallT { dst: Option<L>, err: L, callee: Callee, args: Vec<Op> },
    // write into a place (copy-on-write at every step)
    Store(Place, Op),
    // a mutating method: gets a pointer to the place
    MutCall { dst: Option<L>, err: Option<L>, place: Place, callee: Callee, args: Vec<Op> },
    Dup(L),
    Drop(L),
    // source line of the following statements (for panic messages)
    Line(u32),
}

#[derive(Clone, Debug)]
pub enum Term {
    Goto(B),
    If(Op, B, B),
    IfErr(L, B, B),
    Switch(Op, Vec<(i64, B)>, B),
    Return(Op),
    Throw(Op),
    Unreachable,
}

#[derive(Clone, Debug)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub term: Term,
    pub line: u32,
}

#[derive(Clone, Debug)]
pub struct Local {
    pub ty: Ty,
    pub name: String,
    // `self` of a mutating method: a pointer, never owned
    pub self_ptr: bool,
}

#[derive(Clone, Debug)]
pub enum FnKind {
    Normal,
    // env captures (types in order)
    Closure(Vec<Ty>),
    Test(String),
}

#[derive(Clone, Debug)]
pub struct Func {
    pub name: String,
    pub kind: FnKind,
    pub params: Vec<L>,
    pub ret: Ty,
    pub throws: bool,
    pub mutating: bool, // params[0] is a self pointer
    pub locals: Vec<Local>,
    pub blocks: Vec<Block>,
    pub source_name: String,
}

#[derive(Clone, Debug)]
pub struct Vtable {
    pub concrete: Ty,
    pub iface: Vec<DefId>,
    // one function per method slot
    pub methods: Vec<FnIdx>,
}

pub struct Module {
    pub funcs: Vec<Func>,
    pub vtables: Vec<Vtable>,
    pub main: Option<FnIdx>,
    pub tests: Vec<FnIdx>,
    pub failure_vtable: usize,
}

impl Stmt {
    // (local, consumed?) for every local the statement reads
    pub fn uses(&self, out: &mut Vec<(L, bool)>) {
        fn op(o: &Op, consume: bool, out: &mut Vec<(L, bool)>) {
            if let Op::Local(l) = o {
                out.push((*l, consume));
            }
        }
        fn place(p: &Place, out: &mut Vec<(L, bool)>) {
            out.push((p.root, false));
            for (pr, _) in &p.path {
                match pr {
                    Proj::Index(o) => op(o, false, out),
                    Proj::MapKey(o, _) => op(o, false, out),
                    Proj::Field(_) => {}
                }
            }
        }
        fn callee_args(c: &Callee, args: &[Op], out: &mut Vec<(L, bool)>) {
            match c {
                Callee::Fn(_) => args.iter().for_each(|a| op(a, true, out)),
                Callee::Closure(f) => {
                    op(f, false, out);
                    args.iter().for_each(|a| op(a, true, out));
                }
                Callee::Iface(_) => {
                    if let Some(first) = args.first() {
                        op(first, false, out);
                    }
                    args.iter().skip(1).for_each(|a| op(a, true, out));
                }
                Callee::Intrinsic(name, _) => {
                    let consume = consumes_args(name);
                    args.iter().for_each(|a| op(a, consume, out));
                }
            }
        }
        match self {
            Stmt::Assign(_, rv) => match rv {
                Rv::Use(o) => op(o, true, out),
                Rv::Read(p) => place(p, out),
                Rv::Field(o, _) | Rv::Tag(o) | Rv::VariantField(o, _, _) | Rv::OptIsSome(o) | Rv::OptGet(o, _) | Rv::IfaceIs(o, _) | Rv::IfaceGet(o, _) | Rv::Un(_, o) => op(o, false, out),
                Rv::Bin(_, a, b) | Rv::Eq(a, b) | Rv::Cmp(a, b) => {
                    op(a, false, out);
                    op(b, false, out);
                }
                Rv::Record(v) | Rv::Variant(_, v) | Rv::List(v) | Rv::Closure(_, v) => v.iter().for_each(|a| op(a, true, out)),
                Rv::Some(o) | Rv::ToIface(o, _) => op(o, true, out),
                Rv::Map(v) => v.iter().for_each(|(a, b)| {
                    op(a, true, out);
                    op(b, true, out);
                }),
                Rv::None | Rv::EmptySet | Rv::EnvField(_) => {}
                Rv::Call(c, args) => callee_args(c, args, out),
            },
            Stmt::CallT { callee, args, .. } => callee_args(callee, args, out),
            Stmt::Store(p, v) => {
                place(p, out);
                op(v, true, out);
            }
            Stmt::MutCall { place: p, callee, args, .. } => {
                place(p, out);
                match callee {
                    Callee::Intrinsic(name, _) => {
                        let consume = mut_consumes_args(name);
                        args.iter().for_each(|a| op(a, consume, out));
                    }
                    _ => args.iter().for_each(|a| op(a, true, out)),
                }
            }
            Stmt::Dup(_) | Stmt::Drop(_) | Stmt::Line(_) => {}
        }
    }

    pub fn defs(&self, out: &mut Vec<L>) {
        match self {
            Stmt::Assign(d, _) => out.push(*d),
            Stmt::CallT { dst, err, .. } => {
                if let Some(d) = dst {
                    out.push(*d);
                }
                out.push(*err);
            }
            Stmt::MutCall { dst, err, .. } => {
                if let Some(d) = dst {
                    out.push(*d);
                }
                if let Some(e) = err {
                    out.push(*e);
                }
            }
            _ => {}
        }
    }
}

impl Term {
    pub fn uses(&self, out: &mut Vec<(L, bool)>) {
        match self {
            Term::If(Op::Local(l), _, _) | Term::Switch(Op::Local(l), _, _) => out.push((*l, false)),
            Term::IfErr(l, _, _) => out.push((*l, false)),
            Term::Return(Op::Local(l)) | Term::Throw(Op::Local(l)) => out.push((*l, true)),
            _ => {}
        }
    }
    pub fn succs(&self) -> Vec<B> {
        match self {
            Term::Goto(b) => vec![*b],
            Term::If(_, t, f) | Term::IfErr(_, t, f) => vec![*t, *f],
            Term::Switch(_, cases, d) => {
                let mut v: Vec<B> = cases.iter().map(|c| c.1).collect();
                v.push(*d);
                v
            }
            _ => vec![],
        }
    }
    pub fn succs_mut(&mut self) -> Vec<&mut B> {
        match self {
            Term::Goto(b) => vec![b],
            Term::If(_, t, f) | Term::IfErr(_, t, f) => vec![t, f],
            Term::Switch(_, cases, d) => {
                let mut v: Vec<&mut B> = cases.iter_mut().map(|c| &mut c.1).collect();
                v.push(d);
                v
            }
            _ => vec![],
        }
    }
}

// Intrinsics that take ownership of their arguments.
pub fn consumes_args(name: &str) -> bool {
    matches!(name, "print_consume")
}

// Mutating intrinsics that store their arguments.
pub fn mut_consumes_args(name: &str) -> bool {
    matches!(name, "List.append" | "List.append_all" | "List.insert" | "Set.add")
}
