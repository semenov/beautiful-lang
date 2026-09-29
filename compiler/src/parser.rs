// Recursive-descent parser: tokens -> ast::Module.

use crate::ast::*;
use crate::diag::{Diag, Span};
use crate::lexer::{StrPart, Tok, Token};

pub struct Parser {
    toks: Vec<Token>,
    pos: usize,
    // In conditions (`if`, `while`, `for`, `match`, `with`) a `{` starts the
    // block, not a map literal.
    no_brace: bool,
    // The prelude may declare bodiless (runtime) functions and builtin types.
    prelude: bool,
    // In a match guard, `x =>` ends the guard instead of starting a lambda.
    in_guard: bool,
}

type PResult<T> = Result<T, Diag>;

pub fn parse_module(toks: Vec<Token>, prelude: bool) -> PResult<Module> {
    let mut p = Parser { toks, pos: 0, no_brace: false, prelude, in_guard: false };
    p.module()
}

fn is_upper(s: &str) -> bool {
    s.chars().next().map(|c| c.is_ascii_uppercase()).unwrap_or(false)
}

impl Parser {
    // ---- token helpers ----

    fn peek(&self) -> &Tok {
        &self.toks[self.pos].tok
    }
    fn peek_at(&self, n: usize) -> &Tok {
        &self.toks[(self.pos + n).min(self.toks.len() - 1)].tok
    }
    fn span(&self) -> Span {
        self.toks[self.pos].span
    }
    fn prev_span(&self) -> Span {
        self.toks[self.pos.saturating_sub(1)].span
    }
    fn bump(&mut self) -> Tok {
        let t = self.toks[self.pos].tok.clone();
        if self.pos < self.toks.len() - 1 {
            self.pos += 1;
        }
        t
    }
    fn at(&self, t: &Tok) -> bool {
        self.peek() == t
    }
    fn eat(&mut self, t: &Tok) -> bool {
        if self.at(t) {
            self.bump();
            true
        } else {
            false
        }
    }
    fn skip_newlines(&mut self) {
        while self.at(&Tok::Newline) {
            self.bump();
        }
    }
    fn describe(t: &Tok) -> String {
        match t {
            Tok::Ident(s) => format!("`{}`", s),
            Tok::Int(v) => format!("`{}`", v),
            Tok::Float(v) => format!("`{}`", v),
            Tok::Str(_) => "text".into(),
            Tok::Newline => "end of line".into(),
            Tok::Eof => "end of file".into(),
            other => format!("`{}`", tok_text(other)),
        }
    }
    fn err_here(&self, msg: impl Into<String>) -> Diag {
        Diag::new(self.span(), msg)
    }
    fn expect(&mut self, t: &Tok, what: &str) -> PResult<Span> {
        if self.at(t) {
            let s = self.span();
            self.bump();
            Ok(s)
        } else if self.at(&Tok::Catch) {
            Err(self.err_here("`catch` needs `try` before the call it handles").help("`try` goes at the start and covers the whole expression: `try h(g(x)) catch err { ... }`"))
        } else {
            Err(self.err_here(format!("expected {}, found {}", what, Self::describe(self.peek()))))
        }
    }
    fn ident(&mut self, what: &str) -> PResult<(String, Span)> {
        match self.peek().clone() {
            Tok::Ident(s) => {
                let sp = self.span();
                self.bump();
                Ok((s, sp))
            }
            t => Err(self.err_here(format!("expected {}, found {}", what, Self::describe(&t)))),
        }
    }
    fn end_of_stmt(&mut self) -> PResult<()> {
        match self.peek() {
            Tok::Newline => {
                self.bump();
                Ok(())
            }
            Tok::RBrace | Tok::Eof => Ok(()),
            Tok::Catch => Err(self.err_here("`catch` needs `try` before the call it handles").help("write `try f(x) catch err { ... }`")),
            Tok::Bar => Err(self.err_here("`|` only joins patterns in a `match` arm").help("either of two conditions: `a or b`; bits: `a.bit_or(b)`")),
            t => {
                let d = Self::describe(t);
                Err(self.err_here(format!("expected the end of the line, found {}", d)))
            }
        }
    }

    // ---- module ----

    fn module(&mut self) -> PResult<Module> {
        let mut imports = Vec::new();
        let mut items = Vec::new();
        loop {
            self.skip_newlines();
            if self.at(&Tok::Eof) {
                break;
            }
            if self.eat(&Tok::Import) {
                let (first, sp) = self.ident("a module name")?;
                let mut path = vec![first];
                let mut span = sp;
                while self.eat(&Tok::Dot) {
                    let (seg, ssp) = self.ident("a module name")?;
                    path.push(seg);
                    span = span.to(ssp);
                }
                let alias = if matches!(self.peek(), Tok::Ident(s) if s == "as") {
                    self.bump();
                    Some(self.ident("a name for the module")?.0)
                } else {
                    None
                };
                if matches!(self.peek(), Tok::LBrace) || matches!(self.peek(), Tok::Ident(s) if s == "from") {
                    return Err(self.err_here("import a whole module").help("write `import json` and use `json.decode(...)`"));
                }
                let key = path.join(".");
                imports.push(Import { path, alias, span, key });
                self.end_of_stmt()?;
                continue;
            }
            items.push(self.item()?);
        }
        Ok(Module { imports, items })
    }

    fn item(&mut self) -> PResult<Item> {
        let is_pub = self.eat(&Tok::Pub);
        let builtin = self.prelude && matches!(self.peek(), Tok::Ident(s) if s == "builtin");
        if builtin {
            self.bump();
        }
        match self.peek().clone() {
            Tok::Fn => Ok(Item::Fn(self.fn_decl(is_pub, false, false)?)),
            Tok::Type => self.type_decl(is_pub, builtin),
            Tok::Enum => Ok(Item::Enum(self.enum_decl(is_pub)?)),
            Tok::Interface => Ok(Item::Interface(self.interface_decl(is_pub)?)),
            Tok::Ident(ref s) if s == "test" && matches!(self.peek_at(1), Tok::Str(_)) => {
                let sp = self.span();
                self.bump();
                let name = match self.bump() {
                    Tok::Str(parts) => match parts.as_slice() {
                        [] => String::new(),
                        [StrPart::Lit(s)] => s.clone(),
                        _ => return Err(Diag::new(sp, "a test name is plain text without `${}`")),
                    },
                    _ => return Err(Diag::new(sp, "expected the test name in quotes: test \"name\" { ... }")),
                };
                let body = self.block()?;
                Ok(Item::Test(TestDecl { name, span: sp, body }))
            }
            Tok::Var => Err(self
                .err_here("there are no global variables")
                .help("a fixed value is a top-level `let`; state shared between tasks is a `Shared<T>` created in `main` and passed along")),
            Tok::Let => {
                // `let name: T = value`: a fixed value
                self.bump();
                let (name, nsp) = self.ident("a name")?;
                let ty = if self.eat(&Tok::Colon) { Some(self.type_expr()?) } else { None };
                self.expect(&Tok::Eq, "`=`")?;
                let value = self.expr()?;
                let vsp = value.span;
                Ok(Item::Fn(FnDecl {
                    name,
                    span: nsp,
                    is_pub,
                    generics: vec![],
                    has_self: false,
                    mutating: false,
                    params: vec![],
                    ret: ty,
                    throws: false,
                    rethrows: false,
                    body: Some(Block { stmts: vec![Stmt::Return(Some(value), vsp)], span: vsp }),
                    intrinsic: false,
                    is_const: true,
                }))
            }
            Tok::Mutating => Err(self.err_here("`mutating fn` is only allowed inside a type")),
            t => {
                let d = Self::describe(&t);
                Err(self
                    .err_here(format!("expected a declaration (`fn`, `type`, `enum`, `interface`, `test`), found {}", d))
                    .help("there is no top-level code: a program starts at `fn main()`"))
            }
        }
    }

    fn generics(&mut self) -> PResult<Vec<String>> {
        let mut g = Vec::new();
        if self.eat(&Tok::Lt) {
            loop {
                let (n, sp) = self.ident("a type parameter")?;
                if self.at(&Tok::Colon) {
                    return Err(Diag::new(sp, "type parameters have no bounds")
                        .help("use the interface as a type instead: `fn f(xs: List<Describable>)`"));
                }
                g.push(n);
                if !self.eat(&Tok::Comma) {
                    break;
                }
            }
            self.expect(&Tok::Gt, "`>`")?;
        }
        Ok(g)
    }

    fn fn_decl(&mut self, is_pub: bool, in_type: bool, mutating: bool) -> PResult<FnDecl> {
        let sp = self.expect(&Tok::Fn, "`fn`")?;
        let (name, name_sp) = self.ident("a function name")?;
        let generics = self.generics()?;
        self.expect(&Tok::LParen, "`(`")?;
        let mut params = Vec::new();
        let mut has_self = false;
        let mut first = true;
        while !self.at(&Tok::RParen) {
            if self.at(&Tok::SelfLower) {
                let s = self.span();
                self.bump();
                if !first {
                    return Err(Diag::new(s, "`self` must be the first parameter"));
                }
                if !in_type {
                    return Err(Diag::new(s, "`self` is only allowed in methods, inside a type's body"));
                }
                has_self = true;
            } else if self.at(&Tok::Var) && matches!(self.peek_at(1), Tok::SelfLower) {
                return Err(self
                    .err_here("a method that changes its value is declared with `mutating fn`")
                    .help("write `mutating fn name(...)` and use plain `self` inside"));
            } else if self.at(&Tok::Var) {
                return Err(self
                    .err_here("a function can't change its arguments")
                    .help("return the new value and assign it: `products = restock(products, ...)`, or use a `mutating fn` method on your own type"));
            } else {
                let (pname, psp) = self.ident("a parameter name")?;
                self.expect(&Tok::Colon, "`:` and the parameter type")?;
                if self.at(&Tok::Var) {
                    return Err(self
                        .err_here("a function can't change its arguments")
                        .help("return the new value and assign it: `products = restock(products, ...)`, or use a `mutating fn` method on your own type"));
                }
                let ty = self.type_expr()?;
                params.push(Param { name: pname, ty, span: psp });
            }
            first = false;
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::RParen, "`)`")?;
        let mut throws = self.eat(&Tok::Throws);
        let rethrows = self.prelude && matches!(self.peek(), Tok::Ident(s) if s == "rethrows");
        if rethrows {
            self.bump();
        }
        let ret = if self.eat(&Tok::Arrow) { Some(self.type_expr()?) } else { None };
        if self.at(&Tok::Throws) {
            return Err(self.err_here("`throws` goes before `->`").help("write `fn f() throws -> T`"));
        }
        if !throws && self.eat(&Tok::Throws) {
            throws = true;
        }
        let (body, intrinsic) = if self.at(&Tok::LBrace) {
            (Some(self.block()?), false)
        } else if self.at(&Tok::Eq) {
            return Err(self.err_here("a function body is a block: `{ return ... }`"));
        } else {
            (None, self.prelude)
        };
        if mutating && !in_type {
            return Err(Diag::new(sp, "`mutating fn` is only allowed inside a type"));
        }
        Ok(FnDecl {
            name,
            span: name_sp,
            is_pub,
            generics,
            has_self,
            mutating,
            params,
            ret,
            throws,
            rethrows,
            body,
            intrinsic,
            is_const: false,
        })
    }

    fn implements(&mut self) -> PResult<Vec<(String, Span)>> {
        let mut v = Vec::new();
        if self.eat(&Tok::Implements) {
            loop {
                v.push(self.ident("an interface name")?);
                if !self.eat(&Tok::Comma) {
                    break;
                }
            }
        }
        Ok(v)
    }

    fn type_decl(&mut self, is_pub: bool, builtin: bool) -> PResult<Item> {
        self.expect(&Tok::Type, "`type`")?;
        let (name, sp) = self.ident("a type name")?;
        let generics = self.generics()?;
        if self.eat(&Tok::Eq) {
            let ty = self.type_expr()?;
            self.end_of_stmt()?;
            if !generics.is_empty() {
                return Err(Diag::new(sp, "a new type over another type can't be generic"));
            }
            return Ok(Item::Newtype(NewtypeDecl { name, span: sp, is_pub, ty }));
        }
        let implements = self.implements()?;
        self.expect(&Tok::LBrace, "`{` to start the type's body")?;
        let mut fields = Vec::new();
        let mut methods = Vec::new();
        loop {
            self.skip_newlines();
            if self.eat(&Tok::RBrace) {
                break;
            }
            let mpub = self.eat(&Tok::Pub);
            if self.at(&Tok::Fn) {
                methods.push(self.fn_decl(mpub, true, false)?);
            } else if self.eat(&Tok::Mutating) {
                methods.push(self.fn_decl(mpub, true, true)?);
            } else {
                let (fname, fsp) = self.ident("a field name or `fn`")?;
                self.expect(&Tok::Colon, "`:` and the field type")?;
                let ty = self.type_expr()?;
                let default = if self.eat(&Tok::Eq) { Some(self.expr()?) } else { None };
                fields.push(Field { name: fname, ty, default, span: fsp });
                self.end_of_stmt()?;
            }
        }
        self.end_of_stmt()?;
        Ok(Item::Record(RecordDecl { name, span: sp, is_pub, generics, implements, fields, methods, builtin }))
    }

    fn enum_decl(&mut self, is_pub: bool) -> PResult<EnumDecl> {
        self.expect(&Tok::Enum, "`enum`")?;
        let (name, sp) = self.ident("an enum name")?;
        let generics = self.generics()?;
        let implements = self.implements()?;
        self.expect(&Tok::LBrace, "`{`")?;
        let mut variants = Vec::new();
        let mut methods = Vec::new();
        loop {
            self.skip_newlines();
            if self.eat(&Tok::RBrace) {
                break;
            }
            let mpub = self.eat(&Tok::Pub);
            if self.at(&Tok::Fn) {
                methods.push(self.fn_decl(mpub, true, false)?);
                continue;
            }
            if self.eat(&Tok::Mutating) {
                methods.push(self.fn_decl(mpub, true, true)?);
                continue;
            }
            let (vname, vsp) = self.ident("a variant name")?;
            let mut fields = Vec::new();
            if self.eat(&Tok::LParen) {
                while !self.at(&Tok::RParen) {
                    let (fname, fsp) = self.ident("a field name")?;
                    self.expect(&Tok::Colon, "`:` and the field type")?;
                    let ty = self.type_expr()?;
                    fields.push(Field { name: fname, ty, default: None, span: fsp });
                    if !self.eat(&Tok::Comma) {
                        break;
                    }
                }
                self.expect(&Tok::RParen, "`)`")?;
            }
            variants.push(Variant { name: vname, fields, span: vsp });
            if !self.eat(&Tok::Comma) {
                self.end_of_stmt()?;
            }
        }
        self.end_of_stmt()?;
        Ok(EnumDecl { name, span: sp, is_pub, generics, implements, variants, methods })
    }

    fn interface_decl(&mut self, is_pub: bool) -> PResult<InterfaceDecl> {
        self.expect(&Tok::Interface, "`interface`")?;
        let (name, sp) = self.ident("an interface name")?;
        self.expect(&Tok::LBrace, "`{`")?;
        let mut methods = Vec::new();
        loop {
            self.skip_newlines();
            if self.eat(&Tok::RBrace) {
                break;
            }
            let mutating = self.eat(&Tok::Mutating);
            let f = self.fn_decl(false, true, mutating)?;
            if f.body.is_some() {
                return Err(Diag::new(f.span, "interface methods have no body"));
            }
            methods.push(f);
            self.end_of_stmt()?;
        }
        self.end_of_stmt()?;
        Ok(InterfaceDecl { name, span: sp, is_pub, methods })
    }

    // ---- types ----

    pub fn type_expr(&mut self) -> PResult<TypeExpr> {
        let first = self.type_single()?;
        if self.at(&Tok::Plus) {
            let sp = first.span();
            let mut v = vec![first];
            while self.eat(&Tok::Plus) {
                v.push(self.type_single()?);
            }
            let sp = sp.to(self.prev_span());
            return Ok(TypeExpr::Combo(v, sp));
        }
        Ok(first)
    }

    fn type_single(&mut self) -> PResult<TypeExpr> {
        let sp = self.span();
        let mut t = if self.eat(&Tok::Fn) {
            self.expect(&Tok::LParen, "`(`")?;
            let mut params = Vec::new();
            while !self.at(&Tok::RParen) {
                params.push(self.type_expr()?);
                if !self.eat(&Tok::Comma) {
                    break;
                }
            }
            self.expect(&Tok::RParen, "`)`")?;
            let throws = self.eat(&Tok::Throws);
            let ret = if self.eat(&Tok::Arrow) { Some(Box::new(self.type_single()?)) } else { None };
            TypeExpr::Func { params, ret, throws, span: sp.to(self.prev_span()) }
        } else if self.eat(&Tok::LParen) {
            let inner = self.type_expr()?;
            if self.at(&Tok::Comma) {
                return Err(self.err_here("there are no tuples").help("return a record with named fields"));
            }
            self.expect(&Tok::RParen, "`)`")?;
            inner
        } else {
            let (first, _) = self.ident("a type")?;
            let mut path = vec![first];
            while self.at(&Tok::Dot) && matches!(self.peek_at(1), Tok::Ident(_)) {
                self.bump();
                path.push(self.ident("a type")?.0);
            }
            let mut args = Vec::new();
            if self.eat(&Tok::Lt) {
                loop {
                    args.push(self.type_expr()?);
                    if !self.eat(&Tok::Comma) {
                        break;
                    }
                }
                self.expect(&Tok::Gt, "`>`")?;
            }
            TypeExpr::Named { path, args, span: sp.to(self.prev_span()) }
        };
        while self.at(&Tok::Question) {
            self.bump();
            let s = sp.to(self.prev_span());
            t = TypeExpr::Optional(Box::new(t), s);
        }
        Ok(t)
    }

    // Speculatively parses `<T, U>`; restores the position on failure.
    fn try_type_args(&mut self) -> Option<Vec<TypeExpr>> {
        if !self.at(&Tok::Lt) {
            return None;
        }
        let save = self.pos;
        self.bump();
        let mut args = Vec::new();
        loop {
            match self.type_expr() {
                Ok(t) => args.push(t),
                Err(_) => {
                    self.pos = save;
                    return None;
                }
            }
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        if !self.eat(&Tok::Gt) || !matches!(self.peek(), Tok::LParen) {
            self.pos = save;
            return None;
        }
        Some(args)
    }

    // ---- blocks and statements ----

    pub fn block(&mut self) -> PResult<Block> {
        let lo = self.expect(&Tok::LBrace, "`{`")?;
        let saved = self.no_brace;
        self.no_brace = false;
        let mut stmts = Vec::new();
        loop {
            self.skip_newlines();
            if self.at(&Tok::RBrace) {
                break;
            }
            if self.at(&Tok::Eof) {
                return Err(Diag::new(lo, "this `{` is never closed"));
            }
            stmts.push(self.stmt()?);
            self.end_of_stmt()?;
        }
        let hi = self.expect(&Tok::RBrace, "`}`")?;
        self.no_brace = saved;
        Ok(Block { stmts, span: lo.to(hi) })
    }

    fn cond_expr(&mut self) -> PResult<Expr> {
        let saved = self.no_brace;
        self.no_brace = true;
        let e = self.expr();
        self.no_brace = saved;
        e
    }

    fn stmt(&mut self) -> PResult<Stmt> {
        let sp = self.span();
        match self.peek().clone() {
            Tok::Let | Tok::Var => {
                let mutable = matches!(self.bump(), Tok::Var);
                let (name, nsp) = self.ident("a name")?;
                if self.at(&Tok::Comma) {
                    return Err(self.err_here("there are no tuples to unpack").help("return a record and read its fields"));
                }
                let ty = if self.eat(&Tok::Colon) { Some(self.type_expr()?) } else { None };
                self.expect(&Tok::Eq, "`=`")?;
                let value = self.expr()?;
                Ok(Stmt::Let { name, mutable, ty, value, span: nsp })
            }
            Tok::Return => {
                self.bump();
                let v = if matches!(self.peek(), Tok::Newline | Tok::RBrace | Tok::Eof) { None } else { Some(self.expr()?) };
                Ok(Stmt::Return(v, sp))
            }
            Tok::Break => {
                self.bump();
                Ok(Stmt::Break(sp))
            }
            Tok::Continue => {
                self.bump();
                Ok(Stmt::Continue(sp))
            }
            Tok::Throw => {
                self.bump();
                Ok(Stmt::Throw(self.expr()?, sp))
            }
            Tok::While => {
                self.bump();
                let cond = self.cond_expr()?;
                let body = self.block()?;
                Ok(Stmt::While { cond, body, span: sp })
            }
            Tok::For => {
                self.bump();
                if self.at(&Tok::LParen) {
                    return Err(self.err_here("there are no tuples to unpack in `for`")
                        .help("`for item in xs.indexed()` gives `item.index`, `item.value`; `for entry in map` gives `entry.key`, `entry.value`"));
                }
                let (var, _) = self.ident("a loop variable")?;
                self.expect(&Tok::In, "`in`")?;
                let iter = self.cond_expr()?;
                let body = self.block()?;
                Ok(Stmt::For { var, iter, body, span: sp })
            }
            Tok::With => {
                self.bump();
                let (name, _) = self.ident("a name")?;
                self.expect(&Tok::Eq, "`=`")?;
                let value = self.cond_expr()?;
                let body = self.block()?;
                Ok(Stmt::With { name, value, body, span: sp })
            }
            Tok::Expect if !matches!(self.peek_at(1), Tok::Throws) => {
                self.bump();
                let cond = self.expr()?;
                Ok(Stmt::Expect { cond, span: sp })
            }
            _ => {
                let e = self.expr()?;
                let op = match self.peek() {
                    Tok::Eq => Some(None),
                    Tok::PlusEq => Some(Some(BinOp::Add)),
                    Tok::MinusEq => Some(Some(BinOp::Sub)),
                    Tok::StarEq => Some(Some(BinOp::Mul)),
                    Tok::SlashEq => Some(Some(BinOp::Div)),
                    Tok::PercentEq => Some(Some(BinOp::Rem)),
                    _ => None,
                };
                if let Some(op) = op {
                    self.bump();
                    let value = self.expr()?;
                    return Ok(Stmt::Assign { target: e, op, value, span: sp });
                }
                Ok(Stmt::Expr(e))
            }
        }
    }

    // ---- expressions ----

    pub fn expr(&mut self) -> PResult<Expr> {
        self.or_expr()
    }

    fn bin(&mut self, op: BinOp, l: Expr, r: Expr) -> Expr {
        let span = l.span.to(r.span);
        Expr { kind: ExprKind::Binary(op, Box::new(l), Box::new(r)), span }
    }

    fn or_expr(&mut self) -> PResult<Expr> {
        let mut l = self.and_expr()?;
        while self.eat(&Tok::Or) {
            self.skip_newlines();
            let r = self.and_expr()?;
            l = self.bin(BinOp::Or, l, r);
        }
        Ok(l)
    }

    fn and_expr(&mut self) -> PResult<Expr> {
        let mut l = self.not_expr()?;
        while self.eat(&Tok::And) {
            self.skip_newlines();
            let r = self.not_expr()?;
            l = self.bin(BinOp::And, l, r);
        }
        Ok(l)
    }

    fn not_expr(&mut self) -> PResult<Expr> {
        if self.at(&Tok::Not) {
            let sp = self.span();
            self.bump();
            let e = self.not_expr()?;
            let span = sp.to(e.span);
            return Ok(Expr { kind: ExprKind::Unary(UnOp::Not, Box::new(e)), span });
        }
        self.cmp_expr()
    }

    fn cmp_expr(&mut self) -> PResult<Expr> {
        let l = self.coalesce_expr()?;
        let op = match self.peek() {
            Tok::EqEq => BinOp::Eq,
            Tok::NotEq => BinOp::Ne,
            Tok::Lt => BinOp::Lt,
            Tok::Le => BinOp::Le,
            Tok::Gt => BinOp::Gt,
            Tok::Ge => BinOp::Ge,
            Tok::Is => {
                self.bump();
                let pat = self.pattern()?;
                let span = l.span.to(pat.span());
                return Ok(Expr { kind: ExprKind::Is(Box::new(l), pat), span });
            }
            _ => return Ok(l),
        };
        self.bump();
        self.skip_newlines();
        let r = self.coalesce_expr()?;
        if matches!(self.peek(), Tok::EqEq | Tok::NotEq | Tok::Lt | Tok::Le | Tok::Gt | Tok::Ge) {
            return Err(self.err_here("comparisons can't be chained").help("write `a < b and b < c`"));
        }
        Ok(self.bin(op, l, r))
    }

    fn coalesce_expr(&mut self) -> PResult<Expr> {
        let l = self.range_expr()?;
        if self.eat(&Tok::QQ) {
            self.skip_newlines();
            let r = self.coalesce_expr()?;
            let span = l.span.to(r.span);
            return Ok(Expr { kind: ExprKind::Coalesce(Box::new(l), Box::new(r)), span });
        }
        Ok(l)
    }

    fn range_expr(&mut self) -> PResult<Expr> {
        let l = self.add_expr()?;
        let inclusive = match self.peek() {
            Tok::DotDotLt => false,
            Tok::DotDotEq => true,
            _ => return Ok(l),
        };
        self.bump();
        let r = self.add_expr()?;
        let span = l.span.to(r.span);
        Ok(Expr { kind: ExprKind::Range { lo: Box::new(l), hi: Box::new(r), inclusive }, span })
    }

    fn add_expr(&mut self) -> PResult<Expr> {
        let mut l = self.mul_expr()?;
        loop {
            let op = match self.peek() {
                Tok::Plus => BinOp::Add,
                Tok::Minus => BinOp::Sub,
                _ => return Ok(l),
            };
            self.bump();
            self.skip_newlines();
            let r = self.mul_expr()?;
            l = self.bin(op, l, r);
        }
    }

    fn mul_expr(&mut self) -> PResult<Expr> {
        let mut l = self.unary_expr()?;
        loop {
            let op = match self.peek() {
                Tok::Star => BinOp::Mul,
                Tok::Slash => BinOp::Div,
                Tok::Percent => BinOp::Rem,
                _ => return Ok(l),
            };
            self.bump();
            self.skip_newlines();
            let r = self.unary_expr()?;
            l = self.bin(op, l, r);
        }
    }

    fn unary_expr(&mut self) -> PResult<Expr> {
        let sp = self.span();
        match self.peek() {
            Tok::Minus => {
                self.bump();
                let e = self.unary_expr()?;
                let span = sp.to(e.span);
                // fold negative literals
                return Ok(match e.kind {
                    ExprKind::Int(v) => Expr { kind: ExprKind::Int(-v), span },
                    ExprKind::Float(v) => Expr { kind: ExprKind::Float(-v), span },
                    k => Expr { kind: ExprKind::Unary(UnOp::Neg, Box::new(Expr { kind: k, span: e.span })), span },
                });
            }
            Tok::Try => {
                self.bump();
                let e = self.postfix_expr()?;
                let mut catch = None;
                if self.eat(&Tok::Catch) {
                    let (name, nsp) = self.ident("a name for the error: `catch err { ... }`")?;
                    if self.at(&Tok::FatArrow) || !self.at(&Tok::LBrace) {
                        return Err(Diag::new(nsp, "`catch err` is followed by a block")
                            .help("write `try f() catch err { fallback }`; the block's last line is the value"));
                    }
                    let b = self.block()?;
                    catch = Some((name, b));
                }
                let span = sp.to(self.prev_span());
                return Ok(Expr { kind: ExprKind::Try { expr: Box::new(e), catch }, span });
            }
            Tok::Spawn => {
                self.bump();
                let e = self.postfix_expr()?;
                let span = sp.to(e.span);
                return Ok(Expr { kind: ExprKind::Spawn(Box::new(e)), span });
            }
            Tok::Expect if matches!(self.peek_at(1), Tok::Throws) => {
                self.bump();
                self.bump();
                let e = self.postfix_expr()?;
                let span = sp.to(e.span);
                return Ok(Expr { kind: ExprKind::ExpectThrows(Box::new(e)), span });
            }
            Tok::Bar => {
                return Err(self.err_here("`|` only joins patterns in a `match` arm: `\"a\" | \"b\" => ...`").help("for either of two conditions, use `or`"));
            }
            Tok::Amp => {
                return Err(self
                    .err_here("there is no `&`: a function can't change its arguments")
                    .help("return the new value and assign it: `products = restock(products, ...)`"));
            }
            _ => {}
        }
        self.postfix_expr()
    }

    fn call_args(&mut self) -> PResult<Vec<Arg>> {
        self.expect(&Tok::LParen, "`(`")?;
        let saved = self.no_brace;
        self.no_brace = false;
        let saved_guard = self.in_guard;
        self.in_guard = false;
        let mut args = Vec::new();
        while !self.at(&Tok::RParen) {
            let sp = self.span();
            let name = if matches!(self.peek(), Tok::Ident(_)) && matches!(self.peek_at(1), Tok::Colon) {
                let (n, _) = self.ident("an argument name")?;
                self.bump();
                Some(n)
            } else {
                None
            };
            let value = self.expr()?;
            args.push(Arg { name, span: sp.to(value.span), value });
            if !self.eat(&Tok::Comma) {
                break;
            }
        }
        self.expect(&Tok::RParen, "`)` to close the call")?;
        self.no_brace = saved;
        self.in_guard = saved_guard;
        Ok(args)
    }

    fn postfix_expr(&mut self) -> PResult<Expr> {
        let mut e = self.primary()?;
        // `a?.b`: the chain after `?.` works on the value inside `a`
        let mut chains: Vec<(Expr, String)> = vec![];
        loop {
            match self.peek() {
                Tok::LParen => {
                    let args = self.call_args()?;
                    let span = e.span.to(self.prev_span());
                    e = Expr { kind: ExprKind::Call { callee: Box::new(e), type_args: vec![], args }, span };
                }
                Tok::Lt if matches!(e.kind, ExprKind::Ident(_) | ExprKind::Field(..)) => {
                    match self.try_type_args() {
                        Some(targs) => {
                            let args = self.call_args()?;
                            let span = e.span.to(self.prev_span());
                            e = Expr { kind: ExprKind::Call { callee: Box::new(e), type_args: targs, args }, span };
                        }
                        None => break,
                    }
                }
                Tok::Dot => {
                    self.bump();
                    let (name, nsp) = match self.peek().clone() {
                        Tok::Int(_) => {
                            return Err(self.err_here("there are no tuples to index").help("use a record with named fields"))
                        }
                        _ => self.ident("a field or method name")?,
                    };
                    let span = e.span.to(nsp);
                    e = Expr { kind: ExprKind::Field(Box::new(e), name, nsp), span };
                }
                Tok::LBracket => {
                    self.bump();
                    let idx = self.expr()?;
                    self.expect(&Tok::RBracket, "`]`")?;
                    let span = e.span.to(self.prev_span());
                    e = Expr { kind: ExprKind::Index(Box::new(e), Box::new(idx)), span };
                }
                Tok::Question if matches!(self.peek_at(1), Tok::Dot) => {
                    self.bump();
                    self.bump();
                    let var = format!("?.{}", self.pos);
                    let (name, nsp) = self.ident("a field or method name")?;
                    let vspan = e.span;
                    let base = std::mem::replace(&mut e, Expr { kind: ExprKind::Ident(var.clone()), span: vspan });
                    chains.push((base, var));
                    let span = e.span.to(nsp);
                    e = Expr { kind: ExprKind::Field(Box::new(e), name, nsp), span };
                }
                Tok::Question => {
                    return Err(self.err_here("unexpected `?`").help("use `try f()` to pass an error up, `??` for a fallback, `?.` to reach into an optional value"));
                }
                _ => break,
            }
        }
        while let Some((base, var)) = chains.pop() {
            let span = base.span.to(e.span);
            e = Expr { kind: ExprKind::OptChain { base: Box::new(base), var, rest: Box::new(e) }, span };
        }
        Ok(e)
    }

    fn is_lambda_start(&self) -> bool {
        // `(a, b) =>` / `() =>`
        let mut i = self.pos + 1;
        let mut depth = 1;
        while i < self.toks.len() {
            match self.toks[i].tok {
                Tok::LParen => depth += 1,
                Tok::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        return matches!(self.toks.get(i + 1).map(|t| &t.tok), Some(Tok::FatArrow));
                    }
                }
                Tok::Newline | Tok::Eof | Tok::LBrace => return false,
                _ => {}
            }
            i += 1;
        }
        false
    }

    fn lambda_body(&mut self) -> PResult<Expr> {
        if self.at(&Tok::LBrace) {
            let b = self.block()?;
            let span = b.span;
            Ok(Expr { kind: ExprKind::Block(b), span })
        } else {
            let saved = self.no_brace;
            self.no_brace = false;
            let e = self.expr();
            self.no_brace = saved;
            e
        }
    }

    fn primary(&mut self) -> PResult<Expr> {
        let sp = self.span();
        let tok = self.peek().clone();
        let mk = |kind, span| Ok(Expr { kind, span });
        match tok {
            Tok::Int(v) => {
                self.bump();
                mk(ExprKind::Int(v), sp)
            }
            Tok::Float(v) => {
                self.bump();
                mk(ExprKind::Float(v), sp)
            }
            Tok::True | Tok::False => {
                self.bump();
                mk(ExprKind::Bool(tok == Tok::True), sp)
            }
            Tok::None => {
                self.bump();
                mk(ExprKind::None, sp)
            }
            Tok::SelfLower => {
                self.bump();
                mk(ExprKind::SelfRef, sp)
            }
            Tok::Str(parts) => {
                self.bump();
                let mut segs = Vec::new();
                for p in parts {
                    match p {
                        StrPart::Lit(s) => segs.push(StrSeg::Lit(s)),
                        StrPart::Expr(toks) => {
                            if toks.len() <= 1 {
                                return Err(Diag::new(sp, "empty `${}` in text"));
                            }
                            let mut sub = Parser { toks, pos: 0, no_brace: false, prelude: self.prelude, in_guard: false };
                            let e = sub.expr()?;
                            if !sub.at(&Tok::Eof) {
                                return Err(sub.err_here("unexpected token inside `${...}`"));
                            }
                            segs.push(StrSeg::Expr(e));
                        }
                    }
                }
                mk(ExprKind::Str(segs), sp)
            }
            Tok::Ident(name) => {
                if matches!(self.peek_at(1), Tok::FatArrow) && !self.in_guard {
                    self.bump();
                    self.bump();
                    let body = self.lambda_body()?;
                    let span = sp.to(body.span);
                    return mk(ExprKind::Lambda { params: vec![(name, sp)], body: Box::new(body) }, span);
                }
                self.bump();
                mk(ExprKind::Ident(name), sp)
            }
            Tok::LParen => {
                if self.is_lambda_start() {
                    self.bump();
                    let mut params = Vec::new();
                    while !self.at(&Tok::RParen) {
                        params.push(self.ident("a lambda parameter")?);
                        if self.at(&Tok::Colon) {
                            return Err(self.err_here("lambda parameters have no type annotations; the type comes from where the lambda is used"));
                        }
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(&Tok::RParen, "`)`")?;
                    self.expect(&Tok::FatArrow, "`=>`")?;
                    let body = self.lambda_body()?;
                    let span = sp.to(body.span);
                    return mk(ExprKind::Lambda { params, body: Box::new(body) }, span);
                }
                self.bump();
                let saved = self.no_brace;
                self.no_brace = false;
                let e = self.expr()?;
                self.no_brace = saved;
                if self.at(&Tok::Comma) {
                    return Err(self.err_here("there are no tuples").help("use a record with named fields"));
                }
                self.expect(&Tok::RParen, "`)`")?;
                Ok(e)
            }
            Tok::LBracket => {
                self.bump();
                let saved = self.no_brace;
                self.no_brace = false;
                let mut items = Vec::new();
                while !self.at(&Tok::RBracket) {
                    items.push(self.expr()?);
                    if !self.eat(&Tok::Comma) {
                        break;
                    }
                }
                self.expect(&Tok::RBracket, "`]`")?;
                self.no_brace = saved;
                mk(ExprKind::List(items), sp.to(self.prev_span()))
            }
            Tok::LBrace if !self.no_brace => {
                self.bump();
                let saved = self.no_brace;
                self.no_brace = false;
                let mut entries = Vec::new();
                loop {
                    self.skip_newlines();
                    if self.at(&Tok::RBrace) {
                        break;
                    }
                    let k = self.expr()?;
                    let first = entries.is_empty();
                    // `x ?? { log(...); fallback }`: a block where a value goes
                    self.expect(&Tok::Colon, "`:` between a key and a value").map_err(|d| {
                        if first {
                            d.help("a `{` where a value goes starts a map (`{\"a\": 1}`), not a block of statements; for statements before a fallback: `if x is some(v) { v } else { ...; fallback }`")
                        } else {
                            d
                        }
                    })?;
                    self.skip_newlines();
                    let v = self.expr()?;
                    entries.push((k, v));
                    self.skip_newlines();
                    if !self.eat(&Tok::Comma) {
                        self.skip_newlines();
                        break;
                    }
                }
                self.expect(&Tok::RBrace, "`}`")?;
                self.no_brace = saved;
                mk(ExprKind::Map(entries), sp.to(self.prev_span()))
            }
            Tok::If => self.if_expr(),
            Tok::Match => {
                self.bump();
                let scrut = self.cond_expr()?;
                self.expect(&Tok::LBrace, "`{`")?;
                let saved = self.no_brace;
                self.no_brace = false;
                let mut arms = Vec::new();
                loop {
                    self.skip_newlines();
                    if self.at(&Tok::RBrace) {
                        break;
                    }
                    let asp = self.span();
                    if self.at(&Tok::Ident("case".into())) {
                        return Err(self.err_here("match arms have no `case`").help("write `Circle(r) => ...`"));
                    }
                    let mut pat = self.pattern()?;
                    if self.at(&Tok::Bar) {
                        let mut alts = vec![pat];
                        while self.eat(&Tok::Bar) {
                            self.skip_newlines();
                            alts.push(self.pattern()?);
                        }
                        let span = alts[0].span().to(alts[alts.len() - 1].span());
                        pat = Pattern::Or(alts, span);
                    }
                    let guard = if self.eat(&Tok::If) {
                        self.in_guard = true;
                        let g = self.expr();
                        self.in_guard = false;
                        Some(g?)
                    } else {
                        None
                    };
                    self.expect(&Tok::FatArrow, "`=>`")?;
                    let body = self.lambda_body()?;
                    arms.push(MatchArm { pat, guard, span: asp.to(body.span), body });
                    if !self.eat(&Tok::Comma) {
                        self.end_of_stmt()?;
                    }
                }
                self.expect(&Tok::RBrace, "`}`")?;
                self.no_brace = saved;
                mk(ExprKind::Match { scrut: Box::new(scrut), arms }, sp.to(self.prev_span()))
            }
            Tok::Throw | Tok::Return | Tok::Break | Tok::Continue => {
                let s = self.stmt()?;
                mk(ExprKind::Diverge(Box::new(s)), sp.to(self.prev_span()))
            }
            Tok::LBrace => Err(self.err_here("expected an expression, found `{`")),
            t => {
                let d = Self::describe(&t);
                Err(self.err_here(format!("expected an expression, found {}", d)))
            }
        }
    }

    fn if_expr(&mut self) -> PResult<Expr> {
        let sp = self.expect(&Tok::If, "`if`")?;
        let cond = self.cond_expr()?;
        if self.at(&Tok::Ident("then".into())) {
            return Err(self.err_here("there is no `then`").help("write `if c { a } else { b }`"));
        }
        let then = self.block()?;
        // allow `else` on the next line
        let save = self.pos;
        self.skip_newlines();
        let els = if self.eat(&Tok::Else) {
            if self.at(&Tok::If) {
                Some(Box::new(self.if_expr()?))
            } else {
                let b = self.block()?;
                let span = b.span;
                Some(Box::new(Expr { kind: ExprKind::Block(b), span }))
            }
        } else {
            self.pos = save;
            None
        };
        let span = sp.to(self.prev_span());
        Ok(Expr { kind: ExprKind::If { cond: Box::new(cond), then, els }, span })
    }

    fn pattern(&mut self) -> PResult<Pattern> {
        let sp = self.span();
        let tok = self.peek().clone();
        match tok {
            Tok::Ident(ref n) if n == "_" => {
                self.bump();
                Ok(Pattern::Wild(sp))
            }
            Tok::Int(v) => {
                self.bump();
                Ok(Pattern::Int(v, sp))
            }
            Tok::Minus => {
                self.bump();
                match self.bump() {
                    Tok::Int(v) => Ok(Pattern::Int(-v, sp)),
                    Tok::Float(v) => Ok(Pattern::Float(-v, sp)),
                    _ => Err(Diag::new(sp, "expected a number")),
                }
            }
            Tok::Float(v) => {
                self.bump();
                Ok(Pattern::Float(v, sp))
            }
            Tok::True | Tok::False => {
                self.bump();
                Ok(Pattern::Bool(tok == Tok::True, sp))
            }
            Tok::None => {
                self.bump();
                Ok(Pattern::None(sp))
            }
            Tok::Str(parts) => {
                self.bump();
                match parts.as_slice() {
                    [] => Ok(Pattern::Str(String::new(), sp)),
                    [StrPart::Lit(s)] => Ok(Pattern::Str(s.clone(), sp)),
                    _ => Err(Diag::new(sp, "a text pattern can't contain `${}`")),
                }
            }
            Tok::Ident(n) => {
                self.bump();
                if n == "some" {
                    self.expect(&Tok::LParen, "`(`")?;
                    let inner = self.pattern()?;
                    self.expect(&Tok::RParen, "`)`")?;
                    return Ok(Pattern::Some(Box::new(inner), sp.to(self.prev_span())));
                }
                if !is_upper(&n) && !self.at(&Tok::Dot) && !self.at(&Tok::LParen) {
                    return Ok(Pattern::Bind(n, sp));
                }
                let mut path = vec![n];
                while self.eat(&Tok::Dot) {
                    path.push(self.ident("a name")?.0);
                }
                let args = if self.eat(&Tok::LParen) {
                    let mut v = Vec::new();
                    while !self.at(&Tok::RParen) {
                        if matches!(self.peek(), Tok::Ident(_)) && matches!(self.peek_at(1), Tok::Colon) {
                            return Err(self.err_here("patterns bind fields by position").help("write `Circle(r)`, not `Circle(radius: r)`"));
                        }
                        v.push(self.pattern()?);
                        if !self.eat(&Tok::Comma) {
                            break;
                        }
                    }
                    self.expect(&Tok::RParen, "`)`")?;
                    Some(v)
                } else {
                    None
                };
                Ok(Pattern::Ctor { path, args, span: sp.to(self.prev_span()) })
            }
            Tok::LParen => Err(self.err_here("there are no tuple patterns")),
            t => {
                let d = Self::describe(&t);
                Err(self.err_here(format!("expected a pattern, found {}", d)))
            }
        }
    }
}

pub fn tok_text(t: &Tok) -> &'static str {
    match t {
        Tok::Fn => "fn",
        Tok::Let => "let",
        Tok::Var => "var",
        Tok::Type => "type",
        Tok::Enum => "enum",
        Tok::Interface => "interface",
        Tok::Implements => "implements",
        Tok::Pub => "pub",
        Tok::Import => "import",
        Tok::If => "if",
        Tok::Else => "else",
        Tok::For => "for",
        Tok::In => "in",
        Tok::While => "while",
        Tok::Break => "break",
        Tok::Continue => "continue",
        Tok::Return => "return",
        Tok::Match => "match",
        Tok::Is => "is",
        Tok::Try => "try",
        Tok::Catch => "catch",
        Tok::Throw => "throw",
        Tok::Throws => "throws",
        Tok::With => "with",
        Tok::Expect => "expect",
        Tok::And => "and",
        Tok::Or => "or",
        Tok::Not => "not",
        Tok::True => "true",
        Tok::False => "false",
        Tok::None => "none",
        Tok::Mutating => "mutating",
        Tok::SelfLower => "self",
        Tok::Spawn => "spawn",
        Tok::LParen => "(",
        Tok::RParen => ")",
        Tok::LBracket => "[",
        Tok::RBracket => "]",
        Tok::LBrace => "{",
        Tok::RBrace => "}",
        Tok::Comma => ",",
        Tok::Colon => ":",
        Tok::Dot => ".",
        Tok::Arrow => "->",
        Tok::FatArrow => "=>",
        Tok::Question => "?",
        Tok::QQ => "??",
        Tok::DotDotLt => "..<",
        Tok::DotDotEq => "..=",
        Tok::Eq => "=",
        Tok::EqEq => "==",
        Tok::NotEq => "!=",
        Tok::Lt => "<",
        Tok::Gt => ">",
        Tok::Le => "<=",
        Tok::Ge => ">=",
        Tok::Plus => "+",
        Tok::Minus => "-",
        Tok::Star => "*",
        Tok::Slash => "/",
        Tok::Percent => "%",
        Tok::PlusEq => "+=",
        Tok::MinusEq => "-=",
        Tok::StarEq => "*=",
        Tok::SlashEq => "/=",
        Tok::PercentEq => "%=",
        Tok::Amp => "&",
        Tok::Bar => "|",
        _ => "?",
    }
}
