// `plumb fmt`: one layout for every file, with no settings.
//
// What it changes: indentation (two spaces per open `{`, `(` or `[`; a line
// starting with `.` continues the one above and goes one level deeper),
// trailing spaces, runs of blank lines (at most one, none right after `{`
// or before `}`), and the final newline. Text inside strings (including
// multi-line """ strings) and comments is never touched. The result is
// checked: the file must read as the same tokens as before.

// What a character is part of while scanning.
#[derive(Clone, Copy, PartialEq)]
enum In {
    Code,
    Str,       // "..."
    LongStr,   // """..."""
}

// The state at the start of a line.
#[derive(Clone)]
struct Scan {
    depth: i32,
    // for each open bracket: the indentation of the line that opened it
    open: Vec<usize>,
    // for each open string: the brace depth of `${` inside it
    stack: Vec<(In, i32)>,
}

fn opening(c: char) -> bool {
    c == '{' || c == '(' || c == '['
}

fn closing(c: char) -> bool {
    c == '}' || c == ')' || c == ']'
}

// Scans one line, updating the depth and the string state. Returns whether
// the line started inside a string (its text must be kept as it is).
fn scan_line(line: &str, st: &mut Scan, indent: usize) -> bool {
    let started_in_string = matches!(st.stack.last(), Some((In::Str, _)) | Some((In::LongStr, _)));
    let chars: Vec<char> = line.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        let mode = st.stack.last().map(|s| s.0).unwrap_or(In::Code);
        match mode {
            In::Code => {
                if c == '/' && i + 1 < chars.len() && chars[i + 1] == '/' {
                    break; // a comment: the rest of the line
                }
                if c == '"' {
                    if i + 2 < chars.len() && chars[i + 1] == '"' && chars[i + 2] == '"' {
                        st.stack.push((In::LongStr, 0));
                        i += 3;
                        continue;
                    }
                    st.stack.push((In::Str, 0));
                } else if opening(c) {
                    st.depth += 1;
                    st.open.push(indent);
                } else if closing(c) {
                    st.depth -= 1;
                    st.open.pop();
                }
            }
            In::Str | In::LongStr => {
                if c == '\\' {
                    i += 2;
                    continue;
                }
                if c == '$' && i + 1 < chars.len() && chars[i + 1] == '{' {
                    // code inside the string until the matching `}`
                    st.stack.push((In::Code, st.depth));
                    st.depth += 1;
                    st.open.push(indent);
                    i += 2;
                    continue;
                }
                if mode == In::Str && c == '"' {
                    st.stack.pop();
                } else if mode == In::LongStr && c == '"' && i + 2 < chars.len() && chars[i + 1] == '"' && chars[i + 2] == '"' {
                    st.stack.pop();
                    i += 3;
                    continue;
                }
            }
        }
        // the end of an interpolation: back into its string
        if mode == In::Code && c == '}' {
            if let Some(&(In::Code, d)) = st.stack.last() {
                if st.depth == d {
                    st.stack.pop();
                }
            }
        }
        i += 1;
    }
    started_in_string
}

pub fn format(src: &str) -> String {
    let mut out: Vec<String> = vec![];
    // lines inside a multi-line string: kept exactly
    let mut keep: Vec<bool> = vec![];
    let mut st = Scan { depth: 0, open: vec![], stack: vec![] };
    for raw in src.lines() {
        let before = st.clone();
        // one level inside the innermost bracket still open after this
        // line's leading closers: `foo(match x {` indents its arms once
        let line = raw.trim();
        let leading_closers = line.chars().take_while(|c| closing(*c)).count();
        let mut level = if before.open.len() > leading_closers { before.open[before.open.len() - 1 - leading_closers] + 1 } else { 0 };
        if line.starts_with('.') && !line.starts_with("..") {
            level += 1;
        }
        let in_string = scan_line(raw, &mut st, level);
        if std::env::var("PLUMB_FMT_DEBUG").is_ok() {
            eprintln!("{:3} {:?} {}", st.depth, st.stack.iter().map(|s| (s.0 == In::Code, s.1)).collect::<Vec<_>>(), raw);
        }
        keep.push(in_string);
        if in_string {
            // inside a multi-line string: exactly as written
            out.push(raw.to_string());
            continue;
        }
        if line.is_empty() {
            out.push(String::new());
            continue;
        }
        out.push(format!("{}{}", "  ".repeat(level), line));
    }
    // blank lines: at most one, none after `{` or before `}`
    let mut tidy: Vec<String> = vec![];
    for (i, l) in out.iter().enumerate() {
        if keep[i] {
            tidy.push(l.clone());
            continue;
        }
        if l.is_empty() {
            let prev = tidy.last().map(|p: &String| p.trim_end().to_string()).unwrap_or_default();
            let next = out[i + 1..].iter().find(|x| !x.is_empty()).map(|x| x.trim_start().to_string()).unwrap_or_default();
            if tidy.is_empty() || prev.is_empty() || prev.ends_with('{') || next.starts_with('}') || next.is_empty() {
                continue;
            }
        }
        tidy.push(l.trim_end().to_string());
    }
    let mut s = tidy.join("\n");
    s.push('\n');
    s
}

// Whether two sources read as the same tokens (formatting only moved
// whitespace).
pub fn same_tokens(a: &str, b: &str) -> bool {
    match (crate::lexer::lex(a, 0), crate::lexer::lex(b, 0)) {
        (Ok(x), Ok(y)) => {
            for (p, q) in x.iter().zip(y.iter()) {
                if no_spans(&format!("{:?}", p.tok)) != no_spans(&format!("{:?}", q.tok)) {
                    if std::env::var("PLUMB_FMT_DEBUG").is_ok() {
                        eprintln!("before: {:?}\nafter:  {:?}", p.tok, q.tok);
                    }
                    return false;
                }
            }
            x.len() == y.len()
        }
        _ => false,
    }
}

// a token's text without source positions (tokens inside strings carry them)
fn no_spans(s: &str) -> String {
    let mut out = String::new();
    let mut rest = s;
    while let Some(i) = rest.find("span: Span {") {
        out.push_str(&rest[..i]);
        let after = &rest[i..];
        let end = after.find('}').map(|e| e + 1).unwrap_or(after.len());
        rest = &after[end..];
    }
    out.push_str(rest);
    out
}
