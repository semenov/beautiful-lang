// Turns source text into tokens.
//
// Newlines are significant (they end statements), except inside `(` and `[`,
// and before a line that starts with `.` (a continued method chain).
// Interpolated strings are lexed recursively: each `${...}` part becomes its
// own token list, which the parser parses as an expression.

use crate::diag::{Diag, Span};

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    Ident(String),
    Int(i64),
    Float(f64),
    Str(Vec<StrPart>),
    // keywords
    Fn,
    Let,
    Var,
    Type,
    Enum,
    Interface,
    Implements,
    Pub,
    Import,
    If,
    Else,
    For,
    In,
    While,
    Break,
    Continue,
    Return,
    Match,
    Is,
    Try,
    Catch,
    Throw,
    Throws,
    With,
    Expect,
    And,
    Or,
    Not,
    True,
    False,
    None,
    Mutating,
    SelfLower,
    Spawn,
    // punctuation
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Comma,
    Colon,
    Dot,
    Arrow,    // ->
    FatArrow, // =>
    Question, // ?
    QQ,       // ??
    DotDotLt, // ..<
    DotDotEq, // ..=
    Eq,       // =
    EqEq,
    NotEq,
    Lt,
    Gt,
    Le,
    Ge,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    Amp,
    Newline,
    Eof,
}

#[derive(Debug, Clone, PartialEq)]
pub enum StrPart {
    Lit(String),
    Expr(Vec<Token>),
}

#[derive(Debug, Clone, PartialEq)]
pub struct Token {
    pub tok: Tok,
    pub span: Span,
}

pub fn keyword(s: &str) -> Option<Tok> {
    Some(match s {
        "fn" => Tok::Fn,
        "let" => Tok::Let,
        "var" => Tok::Var,
        "type" => Tok::Type,
        "enum" => Tok::Enum,
        "interface" => Tok::Interface,
        "implements" => Tok::Implements,
        "pub" => Tok::Pub,
        "import" => Tok::Import,
        "if" => Tok::If,
        "else" => Tok::Else,
        "for" => Tok::For,
        "in" => Tok::In,
        "while" => Tok::While,
        "break" => Tok::Break,
        "continue" => Tok::Continue,
        "return" => Tok::Return,
        "match" => Tok::Match,
        "is" => Tok::Is,
        "try" => Tok::Try,
        "catch" => Tok::Catch,
        "throw" => Tok::Throw,
        "throws" => Tok::Throws,
        "with" => Tok::With,
        "expect" => Tok::Expect,
        "and" => Tok::And,
        "or" => Tok::Or,
        "not" => Tok::Not,
        "true" => Tok::True,
        "false" => Tok::False,
        "none" => Tok::None,
        "mutating" => Tok::Mutating,
        "self" => Tok::SelfLower,
        "spawn" => Tok::Spawn,
        _ => return None,
    })
}

struct Lexer<'a> {
    src: &'a [u8],
    pos: usize,
    file: u32,
    out: Vec<Token>,
    depth: Vec<u8>, // stack of open brackets: b'(' / b'[' / b'{'
}

pub fn lex(src: &str, file: u32) -> Result<Vec<Token>, Diag> {
    let mut lx = Lexer { src: src.as_bytes(), pos: 0, file, out: Vec::new(), depth: Vec::new() };
    lx.run(false)?;
    lx.push(Tok::Newline, lx.pos, lx.pos);
    lx.push(Tok::Eof, lx.pos, lx.pos);
    Ok(lx.out)
}

impl<'a> Lexer<'a> {
    fn span(&self, lo: usize, hi: usize) -> Span {
        Span { file: self.file, lo: lo as u32, hi: hi as u32 }
    }

    fn push(&mut self, tok: Tok, lo: usize, hi: usize) {
        let span = self.span(lo, hi);
        self.out.push(Token { tok, span });
    }

    fn err(&self, lo: usize, msg: impl Into<String>) -> Diag {
        Diag::new(self.span(lo, (lo + 1).min(self.src.len())), msg)
    }

    fn peek(&self, off: usize) -> u8 {
        *self.src.get(self.pos + off).unwrap_or(&0)
    }

    fn newlines_significant(&self) -> bool {
        !matches!(self.depth.last(), Some(b'(') | Some(b'['))
    }

    // Is the next non-blank, non-comment line a `.method` continuation?
    fn next_line_continues(&self) -> bool {
        let mut p = self.pos;
        loop {
            while p < self.src.len() && matches!(self.src[p], b' ' | b'\t' | b'\r' | b'\n') {
                p += 1;
            }
            if p + 1 < self.src.len() && self.src[p] == b'/' && self.src[p + 1] == b'/' {
                while p < self.src.len() && self.src[p] != b'\n' {
                    p += 1;
                }
                continue;
            }
            break;
        }
        p < self.src.len()
            && self.src[p] == b'.'
            && self.src.get(p + 1) != Some(&b'.')
    }

    // Lexes until end of input, or (when `in_interp`) until the `}` closing
    // an interpolation.
    fn run(&mut self, in_interp: bool) -> Result<(), Diag> {
        let base_depth = self.depth.len();
        while self.pos < self.src.len() {
            let c = self.peek(0);
            let lo = self.pos;
            match c {
                b' ' | b'\t' | b'\r' => self.pos += 1,
                b'\n' => {
                    self.pos += 1;
                    if self.newlines_significant()
                        && !self.next_line_continues()
                        && !matches!(self.out.last().map(|t| &t.tok), Some(Tok::Newline) | None)
                    {
                        self.push(Tok::Newline, lo, lo + 1);
                    }
                }
                b'/' if self.peek(1) == b'/' => {
                    while self.pos < self.src.len() && self.peek(0) != b'\n' {
                        self.pos += 1;
                    }
                }
                b'"' => self.string()?,
                b'0'..=b'9' => self.number()?,
                c if c == b'_' || c.is_ascii_alphabetic() => {
                    while self.pos < self.src.len()
                        && (self.peek(0) == b'_' || self.peek(0).is_ascii_alphanumeric())
                    {
                        self.pos += 1;
                    }
                    let s = std::str::from_utf8(&self.src[lo..self.pos]).unwrap();
                    let tok = keyword(s).unwrap_or_else(|| Tok::Ident(s.to_string()));
                    self.push(tok, lo, self.pos);
                }
                b'}' if in_interp && self.depth.len() == base_depth => {
                    self.pos += 1;
                    return Ok(());
                }
                _ => self.punct()?,
            }
        }
        if in_interp {
            return Err(self.err(self.pos.saturating_sub(1), "unterminated `${` in text"));
        }
        Ok(())
    }

    fn punct(&mut self) -> Result<(), Diag> {
        let lo = self.pos;
        let two = [self.peek(0), self.peek(1)];
        let three = [self.peek(0), self.peek(1), self.peek(2)];
        let (tok, n) = match &three {
            b"..<" => (Tok::DotDotLt, 3),
            b"..=" => (Tok::DotDotEq, 3),
            _ => match &two {
                b"->" => (Tok::Arrow, 2),
                b"=>" => (Tok::FatArrow, 2),
                b"??" => (Tok::QQ, 2),
                b"==" => (Tok::EqEq, 2),
                b"!=" => (Tok::NotEq, 2),
                b"<=" => (Tok::Le, 2),
                b">=" => (Tok::Ge, 2),
                b"+=" => (Tok::PlusEq, 2),
                b"-=" => (Tok::MinusEq, 2),
                b"*=" => (Tok::StarEq, 2),
                b"/=" => (Tok::SlashEq, 2),
                b"%=" => (Tok::PercentEq, 2),
                b"&&" => return Err(self.err(lo, "use `and` instead of `&&`")),
                b"||" => return Err(self.err(lo, "use `or` instead of `||`")),
                b".." => return Err(self.err(lo, "a bare `..` doesn't exist: use `..<` (excludes the end) or `..=` (includes it)")),
                _ => match self.peek(0) {
                    b'(' => (Tok::LParen, 1),
                    b')' => (Tok::RParen, 1),
                    b'[' => (Tok::LBracket, 1),
                    b']' => (Tok::RBracket, 1),
                    b'{' => (Tok::LBrace, 1),
                    b'}' => (Tok::RBrace, 1),
                    b',' => (Tok::Comma, 1),
                    b':' => (Tok::Colon, 1),
                    b'.' => (Tok::Dot, 1),
                    b'?' => (Tok::Question, 1),
                    b'=' => (Tok::Eq, 1),
                    b'<' => (Tok::Lt, 1),
                    b'>' => (Tok::Gt, 1),
                    b'+' => (Tok::Plus, 1),
                    b'-' => (Tok::Minus, 1),
                    b'*' => (Tok::Star, 1),
                    b'/' => (Tok::Slash, 1),
                    b'%' => (Tok::Percent, 1),
                    b'&' => (Tok::Amp, 1),
                    b'!' => return Err(self.err(lo, "use `not x` instead of `!x`")),
                    b';' => return Err(self.err(lo, "no semicolons: a statement ends at the end of the line")),
                    b'\'' => return Err(self.err(lo, "text uses double quotes only")),
                    c => return Err(self.err(lo, format!("unexpected character `{}`", c as char))),
                },
            },
        };
        self.pos += n;
        match tok {
            Tok::LParen => self.depth.push(b'('),
            Tok::LBracket => self.depth.push(b'['),
            Tok::LBrace => self.depth.push(b'{'),
            Tok::RParen | Tok::RBracket | Tok::RBrace => {
                self.depth.pop();
            }
            _ => {}
        }
        // A `{` opens a block: newlines count again inside it, even if we are
        // inside parentheses (multi-line lambdas).
        self.push(tok, lo, self.pos);
        Ok(())
    }

    fn number(&mut self) -> Result<(), Diag> {
        let lo = self.pos;
        while self.peek(0).is_ascii_digit() || self.peek(0) == b'_' {
            self.pos += 1;
        }
        let mut is_float = false;
        if self.peek(0) == b'.' && self.peek(1).is_ascii_digit() {
            is_float = true;
            self.pos += 1;
            while self.peek(0).is_ascii_digit() || self.peek(0) == b'_' {
                self.pos += 1;
            }
        }
        if matches!(self.peek(0), b'e' | b'E')
            && (self.peek(1).is_ascii_digit()
                || (matches!(self.peek(1), b'+' | b'-') && self.peek(2).is_ascii_digit()))
        {
            is_float = true;
            self.pos += 2;
            while self.peek(0).is_ascii_digit() {
                self.pos += 1;
            }
        }
        let text: String = std::str::from_utf8(&self.src[lo..self.pos])
            .unwrap()
            .chars()
            .filter(|c| *c != '_')
            .collect();
        if is_float {
            let v: f64 = text.parse().map_err(|_| self.err(lo, "bad number"))?;
            self.push(Tok::Float(v), lo, self.pos);
        } else {
            let v: i64 = text
                .parse()
                .map_err(|_| self.err(lo, "this number doesn't fit in an Int (64-bit)"))?;
            self.push(Tok::Int(v), lo, self.pos);
        }
        Ok(())
    }

    fn string(&mut self) -> Result<(), Diag> {
        let lo = self.pos;
        let triple = self.peek(1) == b'"' && self.peek(2) == b'"';
        self.pos += if triple { 3 } else { 1 };
        let mut parts: Vec<StrPart> = Vec::new();
        let mut cur: Vec<u8> = Vec::new();
        loop {
            if self.pos >= self.src.len() {
                return Err(self.err(lo, "unterminated text"));
            }
            let c = self.peek(0);
            if triple && c == b'"' && self.peek(1) == b'"' && self.peek(2) == b'"' {
                self.pos += 3;
                break;
            }
            if !triple && c == b'"' {
                self.pos += 1;
                break;
            }
            if !triple && c == b'\n' {
                return Err(self.err(lo, "unterminated text (use \"\"\" for text over several lines)"));
            }
            if c == b'\\' {
                let e = self.peek(1);
                let ch = match e {
                    b'n' => b'\n',
                    b't' => b'\t',
                    b'r' => b'\r',
                    b'0' => 0,
                    b'"' => b'"',
                    b'\\' => b'\\',
                    b'$' => b'$',
                    _ => return Err(self.err(self.pos, "unknown escape; known: \\n \\t \\r \\0 \\\" \\\\ \\$")),
                };
                cur.push(ch);
                self.pos += 2;
                continue;
            }
            if c == b'$' && self.peek(1) == b'{' {
                if !cur.is_empty() {
                    parts.push(StrPart::Lit(String::from_utf8(std::mem::take(&mut cur)).unwrap()));
                }
                self.pos += 2;
                let saved = std::mem::take(&mut self.out);
                let saved_depth = std::mem::take(&mut self.depth);
                self.run(true)?;
                let mut toks = std::mem::replace(&mut self.out, saved);
                self.depth = saved_depth;
                toks.retain(|t| t.tok != Tok::Newline);
                let end = self.pos;
                toks.push(Token { tok: Tok::Eof, span: self.span(end, end) });
                parts.push(StrPart::Expr(toks));
                continue;
            }
            cur.push(c);
            self.pos += 1;
        }
        if !cur.is_empty() {
            parts.push(StrPart::Lit(String::from_utf8(cur).unwrap()));
        }
        if triple {
            parts = dedent(parts);
        }
        self.push(Tok::Str(parts), lo, self.pos);
        Ok(())
    }
}

// For """ text: a line break right after the opening quotes is dropped, a
// blank last line (the one holding the closing quotes) is dropped, and the
// common indentation of the lines is removed.
fn dedent(mut parts: Vec<StrPart>) -> Vec<StrPart> {
    let starts_with_newline = matches!(parts.first(), Some(StrPart::Lit(s)) if s.starts_with('\n'));
    if let Some(StrPart::Lit(s)) = parts.first_mut() {
        if s.starts_with('\n') {
            s.remove(0);
        }
    }
    // Collect (part index, byte offset) of every line start that begins with
    // literal text, with that line's text up to the next newline.
    let mut min_indent = usize::MAX;
    let last_lit = parts.len().checked_sub(1);
    for (pi, p) in parts.iter().enumerate() {
        if let StrPart::Lit(s) = p {
            let segs: Vec<&str> = s.split('\n').collect();
            for (i, seg) in segs.iter().enumerate() {
                let is_line_start = i > 0 || (pi == 0 && starts_with_newline);
                if !is_line_start {
                    continue;
                }
                let blank = seg.trim().is_empty();
                let is_final = Some(pi) == last_lit && i == segs.len() - 1;
                if !blank || is_final {
                    min_indent = min_indent.min(seg.len() - seg.trim_start_matches(' ').len());
                }
            }
        }
    }
    if min_indent == usize::MAX {
        min_indent = 0;
    }
    for (pi, p) in parts.iter_mut().enumerate() {
        if let StrPart::Lit(s) = p {
            let segs: Vec<&str> = s.split('\n').collect();
            let mut out = String::new();
            for (i, seg) in segs.iter().enumerate() {
                let is_line_start = i > 0 || (pi == 0 && starts_with_newline);
                if i > 0 {
                    out.push('\n');
                }
                if is_line_start {
                    let ind = seg.len() - seg.trim_start_matches(' ').len();
                    out.push_str(&seg[ind.min(min_indent)..]);
                } else {
                    out.push_str(seg);
                }
            }
            *s = out;
        }
    }
    if let Some(StrPart::Lit(s)) = parts.last_mut() {
        if let Some(idx) = s.rfind('\n') {
            if s[idx + 1..].trim().is_empty() {
                s.truncate(idx);
            }
        } else if parts.len() > 1 || starts_with_newline {
            // single literal that is only indentation before the closing quotes
        }
    }
    parts.retain(|p| !matches!(p, StrPart::Lit(s) if s.is_empty()));
    parts
}
