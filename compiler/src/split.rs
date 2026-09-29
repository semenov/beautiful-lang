// Splitting a generated C file in two, so the runtime can be compiled once
// and cached: `plumb run` and `plumb test` then compile only the program's
// own code.
//
// The C file is the runtime (everything before the "---- generated ----"
// line) followed by the program. The runtime's top-level items become:
//
//   runtime unit                          program unit
//   static function (not inline)  ->  defined, not static   a prototype
//   static inline / LT_INLINE     ->  kept                  kept
//   static const table            ->  kept                  kept
//   other static variable         ->  defined, not static   extern
//   types, macros, prototypes     ->  kept                  kept
//
// Functions the runtime only declares are defined by the program (callbacks
// such as lt_http_dispatch): the program defines them without `static`, and
// the runtime unit gets a weak stand-in for programs that don't.

pub const MARKER: &str = "\n// ---- generated ----\n";

enum Item<'a> {
    // a preprocessor line, a blank line or a comment: kept as it is
    Other(&'a str),
    // up to and including the `;`
    Decl(&'a str),
    // the signature (up to the body's `{`) and the whole text
    Func { sig: &'a str, all: &'a str },
}

// Splits top-level C into items, skipping strings, characters and comments.
fn items(src: &str) -> Vec<Item<'_>> {
    let b = src.as_bytes();
    let mut out = vec![];
    let mut i = 0;
    let n = b.len();
    while i < n {
        // blank space and comments between items
        let start = i;
        while i < n && (b[i] == b' ' || b[i] == b'\n' || b[i] == b'\t' || b[i] == b'\r') {
            i += 1;
        }
        if i + 1 < n && b[i] == b'/' && b[i + 1] == b'/' {
            while i < n && b[i] != b'\n' {
                i += 1;
            }
            out.push(Item::Other(&src[start..i]));
            continue;
        }
        if i + 1 < n && b[i] == b'/' && b[i + 1] == b'*' {
            i += 2;
            while i + 1 < n && !(b[i] == b'*' && b[i + 1] == b'/') {
                i += 1;
            }
            i = (i + 2).min(n);
            out.push(Item::Other(&src[start..i]));
            continue;
        }
        if i >= n {
            out.push(Item::Other(&src[start..i]));
            break;
        }
        if b[i] == b'#' {
            // with its continuation lines
            while i < n && b[i] != b'\n' {
                if b[i] == b'\\' && i + 1 < n {
                    i += 1;
                }
                i += 1;
            }
            out.push(Item::Other(&src[start..i]));
            continue;
        }
        if start < i {
            out.push(Item::Other(&src[start..i]));
        }
        let begin = i;
        let mut depth = 0;
        let mut last = b' '; // the last significant character at depth 0
        let mut body_at = None;
        while i < n {
            let c = b[i];
            match c {
                b'"' | b'\'' => {
                    i += 1;
                    while i < n && b[i] != c {
                        if b[i] == b'\\' {
                            i += 1;
                        }
                        i += 1;
                    }
                }
                b'/' if i + 1 < n && b[i + 1] == b'/' => {
                    while i < n && b[i] != b'\n' {
                        i += 1;
                    }
                    continue;
                }
                b'/' if i + 1 < n && b[i + 1] == b'*' => {
                    i += 2;
                    while i + 1 < n && !(b[i] == b'*' && b[i + 1] == b'/') {
                        i += 1;
                    }
                    i += 1;
                }
                b'(' | b'[' => depth += 1,
                b')' | b']' => depth -= 1,
                b'{' => {
                    if depth == 0 && body_at.is_none() && last == b')' {
                        body_at = Some(i);
                    }
                    depth += 1;
                }
                b'}' => {
                    depth -= 1;
                    if depth == 0 && body_at.is_some() {
                        i += 1;
                        break;
                    }
                }
                b';' if depth == 0 && body_at.is_none() => {
                    i += 1;
                    break;
                }
                // a preprocessor line inside a function or an initializer;
                // braces count in the first branch of #if/#else only
                b'\n' if i + 1 < n && b[i + 1] == b'#' => {
                    i += 1;
                    let directive = directive_at(b, i);
                    i = line_end(b, i);
                    if directive == "else" || directive == "elif" {
                        // skip to the matching #endif
                        let mut nest = 0;
                        while i < n {
                            i += 1;
                            if i < n && b[i] == b'#' && b[i - 1] == b'\n' {
                                let d = directive_at(b, i);
                                if d.starts_with("if") {
                                    nest += 1;
                                } else if d == "endif" {
                                    if nest == 0 {
                                        i = line_end(b, i);
                                        break;
                                    }
                                    nest -= 1;
                                }
                            }
                        }
                    }
                    continue;
                }
                _ => {}
            }
            if depth == 0 && !c.is_ascii_whitespace() {
                last = c;
            }
            i += 1;
        }
        let all = &src[begin..i.min(n)];
        match body_at {
            Some(at) => out.push(Item::Func { sig: src[begin..at].trim_end(), all }),
            None => out.push(Item::Decl(all)),
        }
    }
    out
}

// "ifdef", "else", "endif"... for the `#` at b[i]
fn directive_at(b: &[u8], i: usize) -> String {
    let mut j = i + 1;
    while j < b.len() && (b[j] == b' ' || b[j] == b'\t') {
        j += 1;
    }
    let k = j;
    while j < b.len() && b[j].is_ascii_alphabetic() {
        j += 1;
    }
    String::from_utf8_lossy(&b[k..j]).to_string()
}

// the end of the line at b[i] (with its continuation lines)
fn line_end(b: &[u8], mut i: usize) -> usize {
    while i < b.len() && b[i] != b'\n' {
        if b[i] == b'\\' && i + 1 < b.len() {
            i += 1;
        }
        i += 1;
    }
    i
}

fn words(s: &str) -> Vec<&str> {
    s.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).filter(|w| !w.is_empty()).collect()
}

// The text without __attribute__((...)) parts.
fn strip_attrs(s: &str) -> String {
    let mut t = s.to_string();
    while let Some(a) = t.find("__attribute__((") {
        let mut depth = 0;
        let mut end = t.len();
        for (j, c) in t[a..].char_indices() {
            if c == '(' {
                depth += 1;
            } else if c == ')' {
                depth -= 1;
                if depth == 0 {
                    end = a + j + 1;
                    break;
                }
            }
        }
        t.replace_range(a..end, " ");
    }
    t
}

fn last_ident(s: &str) -> String {
    let s = s.trim_end();
    let k = s.rfind(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).map(|k| k + 1).unwrap_or(0);
    s[k..].to_string()
}

// The name a declaration declares: the identifier before the first `(` that
// isn't `(*`, or (a variable) the last identifier before `=`, `[` or `;`.
fn declared_name(sig: &str) -> Option<String> {
    let t = strip_attrs(sig.split('=').next()?);
    if let Some(j) = t.find('(') {
        let after = t[j + 1..].trim_start();
        if let Some(inner) = after.strip_prefix('*') {
            // a function pointer variable: (*name)
            let name: String = inner.trim_start().chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '_').collect();
            return Some(name).filter(|n| !n.is_empty());
        }
        return Some(last_ident(&t[..j])).filter(|n| !n.is_empty());
    }
    let head = t.split(|c| c == '[' || c == ';').next()?;
    Some(last_ident(head)).filter(|n| !n.is_empty())
}

fn is_function_decl(d: &str) -> bool {
    let t = strip_attrs(d.split('=').next().unwrap_or(d));
    let s = t.trim_end().trim_end_matches(';').trim_end();
    s.ends_with(')') && !s.contains("(*") && !words(s).contains(&"typedef")
}

fn inline(sig: &str) -> bool {
    let w = words(sig);
    w.contains(&"inline") || w.contains(&"LT_INLINE")
}

// `static` (or LT_NOINLINE) out of a declaration
fn unstatic(s: &str) -> String {
    let s = s.replacen("LT_NOINLINE", "__attribute__((noinline))", 1);
    match s.find("static ") {
        Some(k) if k == 0 || !s.as_bytes()[k - 1].is_ascii_alphanumeric() => format!("{}{}", &s[..k], &s[k + 7..]),
        _ => s,
    }
}

pub struct Units {
    pub runtime: String,
    pub program: String,
}

pub fn split(c: &str) -> Option<Units> {
    let at = c.find(MARKER)?;
    let (rt, generated) = (&c[..at], &c[at..]);
    let its = items(rt);
    let mut defined = std::collections::HashSet::new();
    for it in &its {
        if let Item::Func { sig, .. } = it {
            if let Some(n) = declared_name(sig) {
                defined.insert(n);
            }
        }
    }
    let mut runtime = String::with_capacity(rt.len() + 4096);
    let mut program = String::with_capacity(rt.len() / 2);
    let mut callbacks: Vec<(String, String)> = vec![];
    let mut stood_in = std::collections::HashSet::new();
    for it in &its {
        match it {
            Item::Other(s) => {
                runtime += s;
                program += s;
            }
            Item::Func { sig, all } => {
                if inline(sig) {
                    runtime += all;
                    program += all;
                } else {
                    runtime += &unstatic(all);
                    if !sig.contains("constructor") {
                        program += &unstatic(sig);
                        program += ";";
                    }
                }
            }
            Item::Decl(d) => {
                let w = words(d);
                if w.first().map(|f| *f == "__asm__" || *f == "asm").unwrap_or(false) {
                    // assembly defines symbols: once, in the runtime
                    runtime += d;
                } else if !w.contains(&"static") || w.contains(&"typedef") {
                    runtime += d;
                    program += d;
                } else if is_function_decl(d) {
                    let name = declared_name(d).unwrap_or_default();
                    let plain = unstatic(d);
                    let callback = !defined.contains(&name) && !inline(d);
                    if callback && !callbacks.iter().any(|(n, _)| *n == name) {
                        callbacks.push((name.clone(), plain.trim_end_matches(';').trim().to_string()));
                    }
                    if inline(d) {
                        runtime += d;
                        program += d;
                    } else if callback && !stood_in.contains(&name) {
                        // the program may not define it: a weak stand-in, in
                        // the same #if context as the declaration
                        stood_in.insert(name.clone());
                        runtime += "__attribute__((weak)) ";
                        runtime += plain.trim_end().trim_end_matches(';');
                        runtime += " { abort(); }";
                        program += &plain;
                    } else {
                        runtime += &plain;
                        program += &plain;
                    }
                } else if w.contains(&"const") && d.contains('=') {
                    // a table: both units keep a copy (sizeof works)
                    runtime += d;
                    program += d;
                } else {
                    runtime += &unstatic(d);
                    let head = d.split('=').next().unwrap_or(d).trim_end().trim_end_matches(';');
                    program += "extern ";
                    program += unstatic(head).trim_start();
                    program += ";";
                }
            }
        }
    }
    runtime += "\n";
    // the program defines its callbacks without `static`
    let names: Vec<&str> = callbacks.iter().map(|(n, _)| n.as_str()).collect();
    for line in generated.split_inclusive('\n') {
        let mut l = line;
        let fixed;
        if l.starts_with("static ") && is_callback_line(l, &names) {
            fixed = l["static ".len()..].to_string();
            l = &fixed;
            program += l;
            continue;
        }
        program += l;
    }
    Some(Units { runtime, program })
}

fn is_callback_line(l: &str, names: &[&str]) -> bool {
    let head = l.split('(').next().unwrap_or("");
    let last = head.rsplit(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).next().unwrap_or("");
    names.contains(&last)
}

// The program's unit in `parts` pieces to compile in parallel. Each piece
// has everything before the generated code, and the generated code's
// types, data and inline functions; every other function is defined in
// one piece (no longer `static`) and declared in the others at the same
// place. Functions go to the piece with the least code so far.
pub fn split_program(program: &str, parts: usize) -> Option<Vec<String>> {
    let at = program.find(MARKER)?;
    let (head, generated) = (&program[..at], &program[at..]);
    let its = items(generated);
    let mut out: Vec<String> = (0..parts).map(|_| String::with_capacity(program.len() / parts + head.len())).collect();
    let mut load = vec![0usize; parts];
    for o in out.iter_mut() {
        o.push_str(head);
    }
    for it in &its {
        match it {
            Item::Other(s) => out.iter_mut().for_each(|o| o.push_str(s)),
            Item::Decl(d) => {
                // prototypes lose `static` (their definitions are shared now)
                let text = if is_function_decl(d) && !inline(d) { unstatic(d) } else { d.to_string() };
                out.iter_mut().for_each(|o| o.push_str(&text));
            }
            Item::Func { sig, all } => {
                if inline(sig) {
                    out.iter_mut().for_each(|o| o.push_str(all));
                    continue;
                }
                let k = (0..parts).min_by_key(|&k| load[k]).unwrap_or(0);
                load[k] += all.len();
                let def = unstatic(all);
                let proto = format!("{};", unstatic(sig));
                for (i, o) in out.iter_mut().enumerate() {
                    o.push_str(if i == k { &def } else { &proto });
                }
            }
        }
    }
    Some(out)
}
