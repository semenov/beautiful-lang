// Types, declarations and the typed syntax tree produced by the checker.

use crate::ast::{BinOp, UnOp};
use crate::diag::Span;
use std::collections::HashMap;

pub type DefId = usize;
pub type FnId = usize;
pub type LocalId = usize;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Ty {
    Int,
    Float,
    Bool,
    Text,
    Unit,
    Never,
    // user and builtin named types (records, enums, newtypes, List, Map, ...)
    Adt(DefId, Vec<Ty>),
    Opt(Box<Ty>),
    Func(Box<FnTy>),
    // an interface value (one interface, or several: `A + B`), sorted ids
    Iface(Vec<DefId>),
    // a type parameter of the function being checked (index into its generics)
    Param(u32),
    // an inference variable
    Var(u32),
    Err,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FnTy {
    pub params: Vec<Ty>,
    pub ret: Ty,
    pub throws: bool,
}

impl Ty {
    pub fn opt(t: Ty) -> Ty {
        Ty::Opt(Box::new(t))
    }
    pub fn func(params: Vec<Ty>, ret: Ty, throws: bool) -> Ty {
        Ty::Func(Box::new(FnTy { params, ret, throws }))
    }
    pub fn is_err(&self) -> bool {
        matches!(self, Ty::Err)
    }

    pub fn subst(&self, args: &[Ty]) -> Ty {
        match self {
            Ty::Param(i) => args.get(*i as usize).cloned().unwrap_or(Ty::Err),
            Ty::Adt(d, a) => Ty::Adt(*d, a.iter().map(|t| t.subst(args)).collect()),
            Ty::Opt(t) => Ty::opt(t.subst(args)),
            Ty::Func(f) => Ty::Func(Box::new(FnTy {
                params: f.params.iter().map(|t| t.subst(args)).collect(),
                ret: f.ret.subst(args),
                throws: f.throws,
            })),
            t => t.clone(),
        }
    }

    pub fn has_params(&self) -> bool {
        match self {
            Ty::Param(_) => true,
            Ty::Adt(_, a) => a.iter().any(|t| t.has_params()),
            Ty::Opt(t) => t.has_params(),
            Ty::Func(f) => f.params.iter().any(|t| t.has_params()) || f.ret.has_params(),
            _ => false,
        }
    }
}

#[derive(Clone, Debug)]
pub struct FieldDef {
    pub name: String,
    pub ty: Ty,
    pub default: Option<TExpr>,
    // the default as written, for `--help`
    pub default_text: Option<String>,
    // the comment above the field, for `--help`
    pub doc: Option<String>,
    pub span: Span,
}

#[derive(Clone, Debug)]
pub struct VariantDef {
    pub name: String,
    pub fields: Vec<FieldDef>,
}

#[derive(Clone, Debug)]
pub enum TypeKind {
    Record { fields: Vec<FieldDef> },
    Enum { variants: Vec<VariantDef> },
    Newtype(Ty),
    Interface { methods: Vec<FnId> },
    // List, Map, Set, Int, Float, Bool, String: implemented by the runtime
    Builtin,
}

#[derive(Clone, Debug)]
pub struct TypeDef {
    pub name: String,
    pub generics: Vec<String>,
    pub kind: TypeKind,
    pub methods: HashMap<String, FnId>,
    // builtin read-only properties (`xs.length`)
    pub props: Vec<(String, Ty)>,
    pub implements: Vec<DefId>,
    pub span: Span,
    pub is_prelude: bool,
    pub module: usize,
    pub is_pub: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelfMode {
    None,
    Value,
    Mutating,
}

#[derive(Clone, Debug)]
pub struct FnDef {
    pub name: String,
    pub owner: Option<DefId>,
    // owner's generics first, then the function's own
    pub generics: Vec<String>,
    pub self_mode: SelfMode,
    pub params: Vec<(String, Ty)>,
    pub ret: Ty,
    pub throws: bool,
    pub rethrows: bool,
    pub intrinsic: bool,
    pub body: Option<TBody>,
    pub span: Span,
    pub is_prelude: bool,
    pub module: usize,
    pub is_pub: bool,
    // a top-level `let`
    pub is_const: bool,
}

#[derive(Clone, Debug)]
pub struct LocalDef {
    pub name: String,
    pub ty: Ty,
    pub mutable: bool,
}

#[derive(Clone, Debug)]
pub struct TBody {
    pub locals: Vec<LocalDef>,
    // `self` first for methods
    pub params: Vec<LocalId>,
    pub block: TBlock,
}

#[derive(Clone, Debug)]
pub struct TestDef {
    pub name: String,
    pub body: TBody,
    pub span: Span,
}

// ---- typed tree ----

#[derive(Clone, Debug)]
pub struct TExpr {
    pub kind: TK,
    pub ty: Ty,
    pub span: Span,
}

#[derive(Clone, Debug)]
pub enum Callee {
    // a named function or method; for methods the receiver is args[0]
    Fn(FnId, Vec<Ty>),
    // calling a function value
    Value(Box<TExpr>),
    // an interface method; the receiver is args[0]
    Iface { iface: DefId, method: FnId },
}

#[derive(Clone, Debug)]
pub enum TK {
    Int(i64),
    Float(f64),
    Bool(bool),
    Text(String),
    NoneLit,
    Unit,
    Local(LocalId),
    FnRef(FnId, Vec<Ty>),
    Call { callee: Callee, args: Vec<TExpr>, throws: bool },
    MutCall { fn_id: FnId, type_args: Vec<Ty>, place: TPlace, args: Vec<TExpr>, throws: bool },
    IfaceMutCall { iface: DefId, method: FnId, place: TPlace, args: Vec<TExpr>, throws: bool },
    Record { def: DefId, fields: Vec<TExpr> },
    Variant { def: DefId, idx: usize, fields: Vec<TExpr> },
    Wrap(Box<TExpr>),
    Unwrap(Box<TExpr>),
    Field(Box<TExpr>, usize),
    Prop(Box<TExpr>, String),
    Index(Box<TExpr>, Box<TExpr>),
    MapGet(Box<TExpr>, Box<TExpr>),
    Unary(UnOp, Box<TExpr>),
    Binary(BinOp, Box<TExpr>, Box<TExpr>),
    Coalesce(Box<TExpr>, Box<TExpr>),
    Some(Box<TExpr>),
    ToIface(Box<TExpr>),
    Lambda { params: Vec<LocalId>, captures: Vec<LocalId>, body: Box<TExpr> },
    Block(TBlock),
    If { cond: Box<TExpr>, then: TBlock, els: Option<Box<TExpr>> },
    Match { scrut: Box<TExpr>, arms: Vec<TArm> },
    Is(Box<TExpr>, TPat),
    Try { call: Box<TExpr>, catch: Option<(LocalId, TBlock)> },
    ExpectThrows(Box<TExpr>),
    Spawn(Box<TExpr>),
    Interp(Vec<TExpr>),
    ToText(Box<TExpr>),
    List(Vec<TExpr>),
    Map(Vec<(TExpr, TExpr)>),
    EmptySet,
    Diverge(Box<TStmt>),
}

#[derive(Clone, Debug)]
pub struct TArm {
    pub pat: TPat,
    pub guard: Option<TExpr>,
    pub body: TExpr,
}

#[derive(Clone, Debug)]
pub enum TPat {
    Wild,
    Bind(LocalId),
    Int(i64),
    Float(f64),
    Text(String),
    Bool(bool),
    None,
    Some(Box<TPat>),
    Variant { idx: usize, args: Vec<TPat> },
    // a record inside an interface value: `NotFound(id)`, `err is NotFound`
    Type { def: DefId, targs: Vec<Ty>, args: Vec<TPat>, bind: Option<LocalId> },
    // a record matched by its fields (scrutinee already has the record type)
    Record { args: Vec<TPat> },
}

#[derive(Clone, Debug)]
pub struct TPlace {
    pub root: LocalId,
    pub path: Vec<PlaceElem>,
    pub ty: Ty,
}

#[derive(Clone, Debug)]
pub enum PlaceElem {
    Field(usize),
    Index(TExpr),
    MapKey(TExpr),
}

#[derive(Clone, Debug)]
pub struct TBlock {
    pub stmts: Vec<TStmt>,
    pub tail: Option<Box<TExpr>>,
    pub ty: Ty,
}

#[derive(Clone, Debug)]
pub enum TStmt {
    Let(LocalId, TExpr),
    Discard(TExpr),
    Assign(TPlace, TExpr),
    Expr(TExpr),
    Return(Option<TExpr>),
    Break,
    Continue,
    Throw(TExpr),
    While(TExpr, TBlock),
    ForList { var: LocalId, list: TExpr, body: TBlock },
    ForRange { var: LocalId, lo: TExpr, hi: TExpr, inclusive: bool, body: TBlock },
    ForMap { var: LocalId, map: TExpr, body: TBlock },
    ForSet { var: LocalId, set: TExpr, body: TBlock },
    ForChannel { var: LocalId, chan: TExpr, body: TBlock },
    // `with x = shared.lock() { ... }`: `x` is a mutable copy of the value
    WithLock { var: LocalId, shared: TExpr, body: TBlock },
    // `with f = try files.open(p) { ... }`: `close` runs on every way out
    With { var: LocalId, value: TExpr, body: TBlock, close: FnId },
    Expect { cond: TExpr, text: String, span: Span },
}

pub struct Builtins {
    pub int: DefId,
    pub float: DefId,
    pub bool_: DefId,
    pub text: DefId,
    pub list: DefId,
    pub map: DefId,
    pub set: DefId,
    pub entry: DefId,
    pub indexed: DefId,
    pub error: DefId,
    pub failure: DefId,
    pub task: DefId,
    pub shared: DefId,
    pub locked: DefId,
    pub channel: DefId,
    pub cancelled: DefId,
    pub channel_closed: DefId,
    pub bytes: DefId,
    pub decimal: DefId,
}

pub struct Program {
    pub module_names: Vec<String>,
    pub defs: Vec<TypeDef>,
    pub fns: Vec<FnDef>,
    pub tests: Vec<TestDef>,
    pub b: Builtins,
    pub main: Option<FnId>,
}

impl Program {
    pub fn list_of(&self, t: Ty) -> Ty {
        Ty::Adt(self.b.list, vec![t])
    }
    pub fn error_ty(&self) -> Ty {
        Ty::Iface(vec![self.b.error])
    }

    pub fn show(&self, t: &Ty) -> String {
        match t {
            Ty::Int => "Int".into(),
            Ty::Float => "Float".into(),
            Ty::Bool => "Bool".into(),
            Ty::Text => "String".into(),
            Ty::Unit => "nothing".into(),
            Ty::Never => "Never".into(),
            Ty::Adt(d, args) => {
                let name = &self.defs[*d].name;
                if args.is_empty() {
                    name.clone()
                } else {
                    format!("{}<{}>", name, args.iter().map(|a| self.show(a)).collect::<Vec<_>>().join(", "))
                }
            }
            Ty::Opt(t) => match **t {
                Ty::Func(_) => format!("({})?", self.show(t)),
                _ => format!("{}?", self.show(t)),
            },
            Ty::Func(f) => {
                let ps = f.params.iter().map(|a| self.show(a)).collect::<Vec<_>>().join(", ");
                let mut s = format!("fn({})", ps);
                if f.throws {
                    s += " throws";
                }
                if f.ret != Ty::Unit {
                    s += &format!(" -> {}", self.show(&f.ret));
                }
                s
            }
            Ty::Iface(ids) => ids.iter().map(|d| self.defs[*d].name.clone()).collect::<Vec<_>>().join(" + "),
            Ty::Param(i) => format!("T{}", i),
            Ty::Var(_) => "_".into(),
            Ty::Err => "?".into(),
        }
    }
}
