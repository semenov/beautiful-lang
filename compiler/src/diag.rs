// Source positions and compiler messages.

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Span {
    pub file: u32,
    pub lo: u32,
    pub hi: u32,
}

impl Span {
    pub fn to(self, other: Span) -> Span {
        Span { file: self.file, lo: self.lo.min(other.lo), hi: self.hi.max(other.hi) }
    }
}

#[derive(Debug, Clone)]
pub struct Diag {
    pub span: Span,
    pub msg: String,
    pub help: Option<String>,
}

impl Diag {
    pub fn new(span: Span, msg: impl Into<String>) -> Diag {
        Diag { span, msg: msg.into(), help: None }
    }
    pub fn help(mut self, help: impl Into<String>) -> Diag {
        self.help = Some(help.into());
        self
    }
}

pub struct SourceFile {
    pub path: String,
    pub text: String,
}

#[derive(Default)]
pub struct Sources {
    pub files: Vec<SourceFile>,
}

impl Sources {
    pub fn add(&mut self, path: String, text: String) -> u32 {
        self.files.push(SourceFile { path, text });
        (self.files.len() - 1) as u32
    }

    pub fn line_col(&self, span: Span) -> (usize, usize) {
        let text = &self.files[span.file as usize].text;
        let lo = (span.lo as usize).min(text.len());
        let before = &text[..lo];
        let line = before.matches('\n').count() + 1;
        let col = lo - before.rfind('\n').map(|i| i + 1).unwrap_or(0) + 1;
        (line, col)
    }

    pub fn render(&self, d: &Diag) -> String {
        let f = &self.files[d.span.file as usize];
        let (line, col) = self.line_col(d.span);
        let src_line = f.text.lines().nth(line - 1).unwrap_or("");
        let width = ((d.span.hi.saturating_sub(d.span.lo)) as usize).max(1);
        let width = width.min(src_line.len().saturating_sub(col - 1).max(1));
        let mut s = format!("error: {}\n  --> {}:{}:{}\n", d.msg, f.path, line, col);
        let num = line.to_string();
        let pad = " ".repeat(num.len());
        s += &format!("{} |\n{} | {}\n{} | {}{}\n", pad, num, src_line, pad, " ".repeat(col - 1), "^".repeat(width));
        if let Some(h) = &d.help {
            s += &format!("{} = help: {}\n", pad, h);
        }
        s
    }
}
