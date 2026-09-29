// `plumb guide` and `plumb doc`: the language and its libraries, explained from
// the command line. The guide is AGENTS.md, built into the compiler; the
// library reference is read from the modules' sources (doc comments and
// public declarations), so it is always the code that's running.

use std::fmt::Write;

pub const GUIDE: &str = include_str!("../../AGENTS.md");

// A documented declaration: its comments and signature, and for a type its
// fields, variants and methods.
#[derive(Debug, Clone)]
pub struct Item {
    pub name: String,
    pub text: String,
    pub members: Vec<Item>,
}

pub struct ModuleDoc {
    pub intro: String,
    pub items: Vec<Item>,
}

fn decl_name(s: &str) -> String {
    let s = s.trim();
    let words: Vec<&str> = s.split_whitespace().collect();
    let pos = words.iter().position(|w| matches!(*w, "fn" | "type" | "enum" | "interface" | "let")).map(|i| i + 1).unwrap_or(0);
    let w = words.get(pos).copied().unwrap_or("");
    w.split(|c: char| !(c.is_alphanumeric() || c == '_')).next().unwrap_or("").to_string()
}

fn is_type_decl(s: &str) -> bool {
    let s = s.trim_start_matches("pub ").trim_start_matches("builtin ");
    s.starts_with("type ") || s.starts_with("enum ") || s.starts_with("interface ")
}

fn is_fn_decl(s: &str) -> bool {
    let s = s.trim_start_matches("pub ").trim_start_matches("mutating ");
    s.starts_with("fn ")
}

// `{` minus `}` in a line of code, not counting strings (with their
// `${...}` parts) and comments.
fn brace_delta(s: &str) -> i32 {
    let mut d = 0;
    // for each open string: the brace depth of the `${` it is inside of
    let mut stack: Vec<Option<i32>> = vec![];
    let mut depth = 0;
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if let Some(None) = stack.last() {
            // inside a string
            match c {
                '\\' => {
                    chars.next();
                }
                '"' => {
                    stack.pop();
                }
                '$' if chars.peek() == Some(&'{') => {
                    chars.next();
                    depth += 1;
                    stack.push(Some(depth));
                }
                _ => {}
            }
            continue;
        }
        match c {
            '"' => stack.push(None),
            '/' if chars.peek() == Some(&'/') => break,
            '{' => {
                depth += 1;
                if stack.is_empty() {
                    d += 1;
                }
            }
            '}' => {
                if let Some(Some(k)) = stack.last() {
                    if *k == depth {
                        stack.pop();
                        depth -= 1;
                        continue;
                    }
                }
                depth -= 1;
                if stack.is_empty() {
                    d -= 1;
                }
            }
            _ => {}
        }
    }
    d
}

fn signature(line: &str) -> String {
    let l = line.trim_end();
    match l.strip_suffix('{') {
        Some(x) => x.trim_end().to_string(),
        None => l.to_string(),
    }
}

// Public declarations of a module's source. In the prelude everything is
// public; in a builtin type or an interface every method is.
pub fn parse(src: &str, prelude: bool) -> ModuleDoc {
    let lines: Vec<&str> = src.lines().collect();
    let mut i = 0;
    let mut intro = vec![];
    while i < lines.len() && lines[i].starts_with("//") {
        let b = &lines[i][2..];
        intro.push(b.strip_prefix(' ').unwrap_or(b).to_string());
        i += 1;
    }
    let mut items: Vec<Item> = vec![];
    let mut pending: Vec<String> = vec![];
    let mut depth: i32 = 0;
    let mut skip_to: Option<i32> = None;
    // the open public type: (item, all its methods public)
    let mut open: Option<(Item, bool)> = None;
    for line in &lines[i..] {
        let s = line.trim();
        let delta = brace_delta(s);
        if let Some(target) = skip_to {
            depth += delta;
            if depth <= target {
                skip_to = None;
            }
            continue;
        }
        if s.starts_with("//") {
            pending.push(line.trim_end().to_string());
            continue;
        }
        if s.is_empty() || s.starts_with("import ") {
            pending.clear();
            continue;
        }
        if depth == 0 {
            let public = s.starts_with("pub ") || (prelude && (is_type_decl(s) || is_fn_decl(s) || s.starts_with("builtin ")));
            if !public {
                pending.clear();
                depth += delta;
                if delta > 0 {
                    skip_to = Some(0);
                }
                continue;
            }
            let mut text = pending.join("\n");
            if !text.is_empty() {
                text.push('\n');
            }
            pending.clear();
            if is_type_decl(s) && delta > 0 {
                text.push_str(line.trim_end());
                let all_public = prelude || s.contains("builtin ") || s.contains("interface ");
                open = Some((Item { name: decl_name(s), text, members: vec![] }, all_public));
                depth += delta;
                continue;
            }
            text.push_str(&signature(line));
            let name = decl_name(s);
            if !name.starts_with("__") {
                items.push(Item { name, text, members: vec![] });
            }
            depth += delta;
            if delta > 0 {
                skip_to = Some(0);
            }
            continue;
        }
        if depth == 1 {
            if let Some((ty, all_public)) = open.as_mut() {
                if s == "}" {
                    depth -= 1;
                    let (t, _) = open.take().unwrap();
                    items.push(t);
                    pending.clear();
                    continue;
                }
                let mut text = pending.join("\n");
                if !text.is_empty() {
                    text.push('\n');
                }
                pending.clear();
                if is_fn_decl(s) {
                    if s.starts_with("pub ") || *all_public {
                        text.push_str(&signature(line));
                        ty.members.push(Item { name: decl_name(s), text, members: vec![] });
                    }
                    depth += delta;
                    if delta > 0 {
                        skip_to = Some(1);
                    }
                    continue;
                }
                // a field or a variant
                text.push_str(line.trim_end());
                let name: String = s.chars().take_while(|c| c.is_alphanumeric() || *c == '_').collect();
                ty.members.push(Item { name, text, members: vec![] });
                depth += delta;
                continue;
            }
        }
        depth += delta;
    }
    ModuleDoc { intro: intro.join("\n").trim().to_string(), items }
}

pub fn render_item(it: &Item) -> String {
    let mut out = it.text.clone();
    if it.members.is_empty() && out.trim_end().ends_with('{') {
        out.push_str("\n}");
    }
    if !it.members.is_empty() {
        out.push('\n');
        for m in &it.members {
            for l in m.text.lines() {
                let _ = writeln!(out, "  {}", l.trim_start());
            }
        }
        out.push('}');
    }
    out
}

pub fn render_module(name: &str, m: &ModuleDoc) -> String {
    let mut out = format!("module {}\n\n", name);
    if !m.intro.is_empty() {
        out += &m.intro;
        out += "\n\n";
    }
    for it in &m.items {
        out += &render_item(it);
        out += "\n\n";
    }
    out.trim_end().to_string() + "\n"
}

// The guide's sections whose heading matches `topic` (any word, any case).
pub fn guide_topic(topic: &str) -> Option<String> {
    let t = topic.to_lowercase();
    let mut out = String::new();
    let mut take = false;
    for line in GUIDE.lines() {
        if let Some(h) = line.strip_prefix("## ") {
            let hl = h.to_lowercase();
            take = hl.contains(&t) || (t.len() >= 4 && hl.split(|c: char| !c.is_alphanumeric()).any(|w| w.starts_with(&t[..4.min(t.len())])));
        }
        if take {
            out += line;
            out.push('\n');
        }
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

pub fn guide_topics() -> Vec<String> {
    GUIDE.lines().filter_map(|l| l.strip_prefix("## ")).map(|s| s.to_string()).collect()
}
