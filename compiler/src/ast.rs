// The syntax tree produced by the parser.

use crate::diag::Span;

#[derive(Debug, Clone)]
pub struct Module {
    pub imports: Vec<Import>,
    pub items: Vec<Item>,
}

// `import store.users` / `import billing.users as billing_users`
#[derive(Debug, Clone)]
pub struct Import {
    pub path: Vec<String>,
    pub alias: Option<String>,
    pub span: Span,
    // the module it refers to, filled in by the loader
    pub key: String,
}

impl Import {
    pub fn local_name(&self) -> &str {
        self.alias.as_deref().unwrap_or_else(|| self.path.last().unwrap())
    }
}

#[derive(Debug, Clone)]
pub enum Item {
    Fn(FnDecl),
    Record(RecordDecl),
    Enum(EnumDecl),
    Newtype(NewtypeDecl),
    Interface(InterfaceDecl),
    Test(TestDecl),
}

#[derive(Debug, Clone)]
pub struct FnDecl {
    pub name: String,
    pub span: Span,
    pub is_pub: bool,
    pub generics: Vec<String>,
    // `self` / `mutating fn` apply to methods only
    pub has_self: bool,
    pub mutating: bool,
    pub params: Vec<Param>,
    pub ret: Option<TypeExpr>,
    pub throws: bool,
    // prelude only: throws exactly when a function argument throws
    pub rethrows: bool,
    pub body: Option<Block>, // None for interface method signatures
    // builtin (prelude) function implemented by the runtime
    pub intrinsic: bool,
    // a top-level `let`: a fixed value, kept as a function without
    // parameters whose body returns it
    pub is_const: bool,
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub ty: TypeExpr,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub ty: TypeExpr,
    pub default: Option<Expr>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct RecordDecl {
    pub name: String,
    pub span: Span,
    pub is_pub: bool,
    pub generics: Vec<String>,
    pub implements: Vec<(String, Span)>,
    pub fields: Vec<Field>,
    pub methods: Vec<FnDecl>,
    pub builtin: bool,
}

#[derive(Debug, Clone)]
pub struct Variant {
    pub name: String,
    pub fields: Vec<Field>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct EnumDecl {
    pub name: String,
    pub span: Span,
    pub is_pub: bool,
    pub generics: Vec<String>,
    pub implements: Vec<(String, Span)>,
    pub variants: Vec<Variant>,
    pub methods: Vec<FnDecl>,
}

#[derive(Debug, Clone)]
pub struct NewtypeDecl {
    pub name: String,
    pub span: Span,
    pub is_pub: bool,
    pub ty: TypeExpr,
}

#[derive(Debug, Clone)]
pub struct InterfaceDecl {
    pub name: String,
    pub span: Span,
    pub is_pub: bool,
    pub methods: Vec<FnDecl>,
}

#[derive(Debug, Clone)]
pub struct TestDecl {
    pub name: String,
    pub span: Span,
    pub body: Block,
}

#[derive(Debug, Clone)]
pub enum TypeExpr {
    // `Int`, `List<T>`, `http.Request`
    Named { path: Vec<String>, args: Vec<TypeExpr>, span: Span },
    Optional(Box<TypeExpr>, Span),
    Func { params: Vec<TypeExpr>, ret: Option<Box<TypeExpr>>, throws: bool, span: Span },
    // `A + B`
    Combo(Vec<TypeExpr>, Span),
}

impl TypeExpr {
    pub fn span(&self) -> Span {
        match self {
            TypeExpr::Named { span, .. }
            | TypeExpr::Optional(_, span)
            | TypeExpr::Func { span, .. }
            | TypeExpr::Combo(_, span) => *span,
        }
    }
}

#[derive(Debug, Clone)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let { name: String, mutable: bool, ty: Option<TypeExpr>, value: Expr, span: Span },
    Assign { target: Expr, op: Option<BinOp>, value: Expr, span: Span },
    Expr(Expr),
    Return(Option<Expr>, Span),
    Break(Span),
    Continue(Span),
    Throw(Expr, Span),
    While { cond: Expr, body: Block, span: Span },
    For { var: String, iter: Expr, body: Block, span: Span },
    With { name: String, value: Expr, body: Block, span: Span },
    Expect { cond: Expr, span: Span },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
}

#[derive(Debug, Clone)]
pub struct Arg {
    pub name: Option<String>,
    pub value: Expr,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct MatchArm {
    pub pat: Pattern,
    pub guard: Option<Expr>,
    pub body: Expr,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Pattern {
    Wild(Span),
    Bind(String, Span),
    Int(i64, Span),
    Float(f64, Span),
    Str(String, Span),
    Bool(bool, Span),
    None(Span),
    Some(Box<Pattern>, Span),
    // `Circle(r)`, `Status.Draft`, `NotFound` (type test)
    Ctor { path: Vec<String>, args: Option<Vec<Pattern>>, span: Span },
    // `"a" | "b"`: any of them (in a match arm, without names)
    Or(Vec<Pattern>, Span),
}

impl Pattern {
    pub fn span(&self) -> Span {
        match self {
            Pattern::Wild(s)
            | Pattern::Bind(_, s)
            | Pattern::Int(_, s)
            | Pattern::Float(_, s)
            | Pattern::Str(_, s)
            | Pattern::Bool(_, s)
            | Pattern::None(s)
            | Pattern::Some(_, s) => *s,
            Pattern::Ctor { span, .. } => *span,
            Pattern::Or(_, span) => *span,
        }
    }
}

#[derive(Debug, Clone)]
pub enum StrSeg {
    Lit(String),
    Expr(Expr),
}

#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(f64),
    Str(Vec<StrSeg>),
    Bool(bool),
    None,
    Ident(String),
    SelfRef,
    List(Vec<Expr>),
    Map(Vec<(Expr, Expr)>),
    // `x.name`
    Field(Box<Expr>, String, Span),
    // `f(args)`, `x.m(args)`, `T<A>(args)`; `type_args` is explicit `<...>`
    Call { callee: Box<Expr>, type_args: Vec<TypeExpr>, args: Vec<Arg> },
    // `name<T>` without a call (e.g. `Stack<Int>` before `(`) is folded into Call
    Index(Box<Expr>, Box<Expr>),
    Unary(UnOp, Box<Expr>),
    Binary(BinOp, Box<Expr>, Box<Expr>),
    Coalesce(Box<Expr>, Box<Expr>),
    Range { lo: Box<Expr>, hi: Box<Expr>, inclusive: bool },
    Lambda { params: Vec<(String, Span)>, body: Box<Expr> },
    Block(Block),
    If { cond: Box<Expr>, then: Block, els: Option<Box<Expr>> },
    Match { scrut: Box<Expr>, arms: Vec<MatchArm> },
    Is(Box<Expr>, Pattern),
    Try { expr: Box<Expr>, catch: Option<(String, Block)> },
    ExpectThrows(Box<Expr>),
    // `spawn f(x)`: start the call as a task
    Spawn(Box<Expr>),
    // `throw`, `return`, `break`, `continue` used in expression position
    // (match arms, `??`, catch blocks)
    Diverge(Box<Stmt>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Neg,
    Not,
}
