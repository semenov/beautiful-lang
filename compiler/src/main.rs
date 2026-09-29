mod ast;
mod cgen;
mod check;
mod diag;
mod doc;
mod fmt;
mod lexer;
mod lower;
mod mir;
mod parser;
mod project;
mod rc;
mod types;

use diag::Sources;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const USAGE: &str = "lang: a language for CLI tools, scripts and backend services.

New to it (an AI agent, or a person)? Start here:
  lang guide                        the language in one read: syntax, rules, fixes for common errors
  lang guide <topic>                one part of it: lang guide errors, lang guide concurrency
  lang doc                          the standard library's modules
  lang doc <module>                 a module's API: lang doc http
  lang doc <module>.<name>          one function or type: lang doc http.Router, lang doc List.map
  lang doc --search <word>          find functions by name or description: lang doc --search gzip

Programs:
  lang run <file.lang> [args]       compile and run
  lang fmt [files or dirs]          lay the code out the one standard way (--check: only report)
  lang build <file.lang> [-o out]   compile an optimized binary
  lang test <file.lang>             run the file's `test` blocks
  lang check <file.lang>            only check for errors (fast)

Projects and packages:
  lang new <name>                   create a project (lang.toml, main.lang)
  lang add <name> <git-url> [--version <tag>] [--path <dir in the repository>]
  lang add <name> --path <dir>      a package on this disk
  lang fetch                        download the packages in lang.lock
  lang update                       move packages to their newest matching versions

Options:
  --debug                           check memory safety and leaks (slow)
  --static                          (build, Linux) one file that needs no libraries at all
  --emit-c <file.c>                 also write the generated C";

// The standard library's modules, for `lang doc`.
const STD_NAMES: &[&str] = &[
    "files", "path", "io", "process", "env", "cli", "term", "log", "time", "json", "http", "net", "sql", "db", "crypto", "encoding", "random", "regex", "csv", "xml", "template", "url", "zlib", "archive", "math",
];

// `lang help`, `lang guide`, `lang doc`
fn learn_command(args: &[String]) -> Option<ExitCode> {
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("help");
    match cmd {
        "help" | "--help" | "-h" => {
            println!("{}", USAGE);
            Some(ExitCode::SUCCESS)
        }
        "guide" => {
            match args.get(1) {
                None => print!("{}", doc::GUIDE),
                Some(topic) => match doc::guide_topic(topic) {
                    Some(text) => print!("{}", text),
                    None => {
                        eprintln!("no part of the guide is about `{}`; the parts are:\n  {}", topic, doc::guide_topics().join("\n  "));
                        return Some(ExitCode::from(1));
                    }
                },
            }
            Some(ExitCode::SUCCESS)
        }
        "doc" => Some(doc_command(&args[1..])),
        "fmt" => Some(fmt_command(&args[1..])),
        _ => None,
    }
}

// Modules `lang doc` can show: the prelude, the standard library, and the
// packages of the project in the current directory.
fn doc_sources() -> Vec<(String, String, bool)> {
    let mut out = vec![("prelude".to_string(), include_str!("prelude.lang").to_string(), true)];
    for n in STD_NAMES {
        if let Some(src) = std_module(n) {
            out.push((n.to_string(), src.to_string(), false));
        }
    }
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let root = project::find_root(&cwd.join("x.lang"));
    if let Ok(deps) = project::resolve(&root, false) {
        for (name, dir) in deps {
            if let Ok(src) = std::fs::read_to_string(dir.join(format!("{}.lang", name))) {
                out.push((name, src, false));
            }
        }
    }
    out
}

// `lang fmt [--check] [paths]`: the files given, or every .lang file under
// the current directory.
fn fmt_command(args: &[String]) -> ExitCode {
    let check = args.iter().any(|a| a == "--check");
    let mut paths: Vec<PathBuf> = args.iter().filter(|a| !a.starts_with("--")).map(PathBuf::from).collect();
    if paths.is_empty() {
        paths.push(PathBuf::from("."));
    }
    let mut files = vec![];
    fn collect(p: &Path, out: &mut Vec<PathBuf>) {
        if p.is_dir() {
            if let Ok(rd) = std::fs::read_dir(p) {
                let mut entries: Vec<_> = rd.flatten().map(|e| e.path()).collect();
                entries.sort();
                for e in entries {
                    let name = e.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
                    if name.starts_with('.') || name == "target" || name == "node_modules" {
                        continue;
                    }
                    collect(&e, out);
                }
            }
        } else if p.extension().map(|e| e == "lang").unwrap_or(false) {
            out.push(p.to_path_buf());
        }
    }
    for p in &paths {
        collect(p, &mut files);
    }
    let mut changed = 0;
    let mut failed = false;
    for f in &files {
        let src = match std::fs::read_to_string(f) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("error: can't read {}: {}", f.display(), e);
                failed = true;
                continue;
            }
        };
        let out = fmt::format(&src);
        if out == src {
            continue;
        }
        if !fmt::same_tokens(&src, &out) {
            eprintln!("error: {}: formatting would change the code (a formatter bug); left as it is", f.display());
            failed = true;
            continue;
        }
        changed += 1;
        if check {
            println!("{}: not formatted", f.display());
        } else if let Err(e) = std::fs::write(f, &out) {
            eprintln!("error: can't write {}: {}", f.display(), e);
            failed = true;
        }
    }
    if check {
        if changed > 0 {
            eprintln!("{} file(s) not formatted: run `lang fmt`", changed);
            return ExitCode::from(1);
        }
    } else if changed > 0 {
        println!("formatted {} file(s)", changed);
    }
    if failed {
        ExitCode::from(1)
    } else {
        ExitCode::SUCCESS
    }
}

fn doc_command(args: &[String]) -> ExitCode {
    let sources = doc_sources();
    let parsed: Vec<(String, doc::ModuleDoc)> = sources.iter().map(|(n, s, p)| (n.clone(), doc::parse(s, *p))).collect();
    let find_module = |n: &str| parsed.iter().find(|(m, _)| m == n);
    match args.first().map(|s| s.as_str()) {
        None => {
            println!("The standard library (`import <module>`, then `module.name`):\n");
            for (n, m) in &parsed {
                // the first sentence of the module's comment
                let flat = m.intro.split("\n\n").next().unwrap_or("").replace('\n', " ");
                let first = flat.split(". ").next().unwrap_or("").trim().trim_end_matches('.');
                let first = first.strip_prefix(&format!("{}:", n)).unwrap_or(first).trim();
                let label = if n == "prelude" { "prelude (no import)".to_string() } else { n.clone() };
                println!("  {:<20} {}", label, first);
            }
            println!("\nMore: lang doc <module>, lang doc <module>.<name>, lang doc --search <word>");
            ExitCode::SUCCESS
        }
        Some("--search") => {
            let word = match args.get(1) {
                Some(w) => w.to_lowercase(),
                None => {
                    eprintln!("usage: lang doc --search <word>");
                    return ExitCode::from(2);
                }
            };
            let mut hits = 0;
            for (mname, m) in &parsed {
                for it in &m.items {
                    let owner = if mname == "prelude" { String::new() } else { format!("{}.", mname) };
                    if it.text.to_lowercase().contains(&word) {
                        println!("{}{}:\n{}\n", owner, it.name, indent(&first_lines(&it.text)));
                        hits += 1;
                    }
                    for mem in &it.members {
                        if mem.text.to_lowercase().contains(&word) {
                            println!("{}{}.{}:\n{}\n", owner, it.name, mem.name, indent(&mem.text));
                            hits += 1;
                        }
                    }
                }
            }
            if hits == 0 {
                eprintln!("nothing mentions `{}`", word);
                return ExitCode::from(1);
            }
            ExitCode::SUCCESS
        }
        Some(q) => {
            let parts: Vec<&str> = q.split('.').collect();
            // a module, or module.name, or module.Type.method
            if let Some((n, m)) = find_module(parts[0]) {
                if parts.len() == 1 {
                    print!("{}", doc::render_module(n, m));
                    return ExitCode::SUCCESS;
                }
                return show_item(&m.items, &parts[1..], q);
            }
            // a prelude type: List, List.map, String.split
            let prelude = &find_module("prelude").unwrap().1;
            if prelude.items.iter().any(|i| i.name == parts[0]) {
                return show_item(&prelude.items, &parts, q);
            }
            eprintln!("there is no module or prelude type `{}`; see `lang doc` for the list", parts[0]);
            ExitCode::from(1)
        }
    }
}

fn show_item(items: &[doc::Item], path: &[&str], full: &str) -> ExitCode {
    let it = match items.iter().find(|i| i.name == path[0]) {
        Some(i) => i,
        None => {
            let names: Vec<&str> = items.iter().map(|i| i.name.as_str()).collect();
            eprintln!("no `{}`; there is: {}", full, names.join(", "));
            return ExitCode::from(1);
        }
    };
    if path.len() == 1 {
        println!("{}", doc::render_item(it));
        return ExitCode::SUCCESS;
    }
    match it.members.iter().find(|m| m.name == path[1]) {
        Some(m) => {
            println!("{}", m.text);
            ExitCode::SUCCESS
        }
        None => {
            let names: Vec<&str> = it.members.iter().map(|i| i.name.as_str()).collect();
            eprintln!("`{}` has no `{}`; it has: {}", path[0], path[1], names.join(", "));
            ExitCode::from(1)
        }
    }
}

fn first_lines(text: &str) -> String {
    let lines: Vec<&str> = text.lines().collect();
    if lines.len() <= 6 {
        return text.to_string();
    }
    format!("{}\n...", lines[..6].join("\n"))
}

fn indent(text: &str) -> String {
    text.lines().map(|l| format!("  {}", l)).collect::<Vec<_>>().join("\n")
}


struct Opts {
    cmd: String,
    path: String,
    out: Option<String>,
    emit_c: Option<String>,
    debug: bool,
    static_link: bool,
    args: Vec<String>,
}

fn parse_args() -> Option<Opts> {
    let mut args = std::env::args().skip(1);
    let cmd = args.next()?;
    let mut path = None;
    let mut out = None;
    let mut emit_c = None;
    let mut debug = false;
    let mut static_link = false;
    let mut rest = vec![];
    while let Some(a) = args.next() {
        match a.as_str() {
            "-o" if path.is_none() || cmd != "run" => out = Some(args.next()?),
            "--emit-c" => emit_c = Some(args.next()?),
            "--debug" if path.is_none() => debug = true,
            "--static" if path.is_none() => static_link = true,
            _ if path.is_none() => path = Some(a),
            _ => rest.push(a),
        }
    }
    Some(Opts { cmd, path: path?, out, emit_c, debug, static_link, args: rest })
}

// Standard library modules, embedded in the compiler.
fn std_module(name: &str) -> Option<&'static str> {
    Some(match name {
        "time" => include_str!("std/time.lang"),
        "files" => include_str!("std/files.lang"),
        "process" => include_str!("std/process.lang"),
        "env" => include_str!("std/env.lang"),
        "log" => include_str!("std/log.lang"),
        "random" => include_str!("std/random.lang"),
        "json" => include_str!("std/json.lang"),
        "cli" => include_str!("std/cli.lang"),
        "http" => include_str!("std/http.lang"),
        "crypto" => include_str!("std/crypto.lang"),
        "encoding" => include_str!("std/encoding.lang"),
        "net" => include_str!("std/net.lang"),
        "db" => include_str!("std/db.lang"),
        "regex" => include_str!("std/regex.lang"),
        "csv" => include_str!("std/csv.lang"),
        "url" => include_str!("std/url.lang"),
        "math" => include_str!("std/math.lang"),
        "path" => include_str!("std/path.lang"),
        "zlib" => include_str!("std/zlib.lang"),
        "xml" => include_str!("std/xml.lang"),
        "io" => include_str!("std/io.lang"),
        "sql" => include_str!("std/sql.lang"),
        "term" => include_str!("std/term.lang"),
        "template" => include_str!("std/template.lang"),
        "archive" => include_str!("std/archive.lang"),
        _ => return None,
    })
}

struct Loaded {
    key: String,
    module: ast::Module,
    privileged: bool,
}

// Where a module's own imports are looked up: the project, or a package.
#[derive(Clone)]
struct Base {
    root: PathBuf,
    prefix: String, // "" for the project, the package name for a package
}

// Parses and checks the program; prints errors. Modules are found in this
// order: the standard library, the project's packages, the project's files.
fn front_end(path: &str, sources: &mut Sources) -> Option<(types::Program, u32)> {
    let entry = PathBuf::from(path);
    let text = match std::fs::read_to_string(&entry) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("error: can't read {}: {}", path, e);
            return None;
        }
    };
    let root = project::find_root(&entry);
    let deps = match project::resolve(&root, false) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("error: {}", e);
            return None;
        }
    };
    let parse = |sources: &mut Sources, name: &str, src: &str, privileged: bool| -> Option<ast::Module> {
        let f = sources.add(name.to_string(), src.to_string());
        lexer::lex(src, f).and_then(|t| parser::parse_module(t, privileged)).map_err(|d| eprint!("{}", sources.render(&d))).ok()
    };
    let prelude = parse(sources, "<prelude>", include_str!("prelude.lang"), true)?;
    let uf = sources.files.len() as u32;
    let mut user = parse(sources, path, &text, false)?;
    let abs_entry = std::fs::canonicalize(&entry).unwrap_or(entry.clone());
    let entry_key = abs_entry
        .strip_prefix(&root)
        .ok()
        .map(|r| r.with_extension("").to_string_lossy().replace('/', "."))
        .unwrap_or_else(|| "main".into());

    let mut loaded: Vec<Loaded> = vec![Loaded { key: "<prelude>".into(), module: prelude, privileged: true }];
    let mut graph: std::collections::HashMap<String, Vec<(String, diag::Span)>> = std::collections::HashMap::new();
    let mut failed = false;
    // resolves the imports of `m` (seen from `base`) and queues new modules
    let mut queue: Vec<(ast::Module, Base, String, bool)> = vec![];
    let project_base = Base { root: root.clone(), prefix: String::new() };
    let mut resolve_imports = |m: &mut ast::Module, base: &Base, from_key: &str, sources: &mut Sources, queue: &mut Vec<(ast::Module, Base, String, bool)>, known: &mut Vec<String>| -> bool {
        let mut ok = true;
        for imp in m.imports.iter_mut() {
            let dotted = imp.path.join(".");
            let (key, file, next_base, privileged): (String, Option<PathBuf>, Base, bool) = if imp.path.len() == 1 && std_module(&dotted).is_some() {
                (dotted.clone(), None, base.clone(), true)
            } else if let Some(dir) = deps.get(&imp.path[0]) {
                let rel = if imp.path.len() == 1 { imp.path[0].clone() } else { imp.path[1..].join("/") };
                (dotted.clone(), Some(dir.join(format!("{}.lang", rel))), Base { root: dir.clone(), prefix: imp.path[0].clone() }, false)
            } else {
                let key = if base.prefix.is_empty() { dotted.clone() } else { format!("{}.{}", base.prefix, dotted) };
                (key, Some(base.root.join(format!("{}.lang", imp.path.join("/")))), base.clone(), false)
            };
            imp.key = key.clone();
            graph.entry(from_key.to_string()).or_default().push((key.clone(), imp.span));
            if key == entry_key && base.prefix.is_empty() {
                let d = diag::Diag::new(imp.span, "the main file can't be imported").help("move the shared code into its own file and import that");
                eprint!("{}", sources.render(&d));
                ok = false;
                continue;
            }
            if known.contains(&key) {
                continue;
            }
            known.push(key.clone());
            let (name, src) = match &file {
                None => (format!("<{}>", dotted), std_module(&dotted).unwrap().to_string()),
                Some(f) => match std::fs::read_to_string(f) {
                    Ok(s) => (f.to_string_lossy().to_string(), s),
                    Err(_) => {
                        let shown = f.strip_prefix(&root).map(|p| p.to_path_buf()).unwrap_or(f.clone());
                        let d = diag::Diag::new(imp.span, format!("there is no module `{}`", dotted))
                            .help(format!("expected the file {}, a package in lang.toml, or a standard module", shown.display()));
                        eprint!("{}", sources.render(&d));
                        ok = false;
                        continue;
                    }
                },
            };
            let f = sources.add(name, src.clone());
            match lexer::lex(&src, f).and_then(|t| parser::parse_module(t, privileged || file.is_none())) {
                Ok(pm) => queue.push((pm, next_base, key, file.is_none())),
                Err(d) => {
                    eprint!("{}", sources.render(&d));
                    ok = false;
                }
            }
        }
        ok
    };
    let mut known: Vec<String> = vec![entry_key.clone()];
    if !resolve_imports(&mut user, &project_base, &entry_key, sources, &mut queue, &mut known) {
        failed = true;
    }
    while let Some((mut m, base, key, privileged)) = queue.pop() {
        if !resolve_imports(&mut m, &base, &key, sources, &mut queue, &mut known) {
            failed = true;
        }
        loaded.push(Loaded { key, module: m, privileged });
    }
    if failed {
        return None;
    }
    // import cycles
    fn visit(k: &str, graph: &std::collections::HashMap<String, Vec<(String, diag::Span)>>, stack: &mut Vec<String>, done: &mut Vec<String>) -> Option<(Vec<String>, diag::Span)> {
        if done.iter().any(|d| d == k) {
            return None;
        }
        stack.push(k.to_string());
        for (next, sp) in graph.get(k).map(|v| v.as_slice()).unwrap_or(&[]) {
            if let Some(pos) = stack.iter().position(|s| s == next) {
                let mut cycle = stack[pos..].to_vec();
                cycle.push(next.clone());
                return Some((cycle, *sp));
            }
            if let Some(c) = visit(next, graph, stack, done) {
                return Some(c);
            }
        }
        stack.pop();
        done.push(k.to_string());
        None
    }
    if let Some((cycle, sp)) = visit(&entry_key, &graph, &mut vec![], &mut vec![]) {
        let d = diag::Diag::new(sp, format!("modules import each other: {}", cycle.join(" → "))).help("move what they share into a third module that both import");
        eprint!("{}", sources.render(&d));
        return None;
    }
    let mut mods: Vec<(String, ast::Module, bool)> = loaded.into_iter().map(|l| (l.key, l.module, l.privileged)).collect();
    mods.push((entry_key, user, false));
    let mut c = check::Checker::new();
    c.source_texts = sources.files.iter().map(|f| f.text.clone()).collect();
    c.check_program(&mods);
    if !c.diags.is_empty() {
        let mut ds = merge_can_fail(c.diags.clone(), &c.source_texts);
        ds.sort_by_key(|d| (d.span.file, d.span.lo));
        for d in &ds {
            eprint!("{}", sources.render(d));
        }
        eprintln!("{} error(s)", ds.len());
        return None;
    }
    Some((c.prog, uf))
}

// Calls that can fail nested in one another (`json.decode<C>(files.read(p))`)
// give one error with the whole fix, not one per call.
fn merge_can_fail(ds: Vec<diag::Diag>, texts: &[String]) -> Vec<diag::Diag> {
    const MSG: &str = "this call can fail";
    let inside = |a: &diag::Span, b: &diag::Span| a.file == b.file && b.lo <= a.lo && a.hi <= b.hi && (a.lo, a.hi) != (b.lo, b.hi);
    let calls: Vec<diag::Span> = ds.iter().filter(|d| d.msg == MSG).map(|d| d.span).collect();
    let mut out = vec![];
    for d in ds {
        if d.msg != MSG {
            out.push(d);
            continue;
        }
        // inner calls are reported with their outermost one
        if calls.iter().any(|o| inside(&d.span, o)) {
            continue;
        }
        let inner: Vec<diag::Span> = calls.iter().filter(|c| inside(c, &d.span)).cloned().collect();
        if inner.is_empty() {
            out.push(d);
            continue;
        }
        let text = match texts.get(d.span.file as usize).and_then(|t| t.get(d.span.lo as usize..d.span.hi as usize)) {
            Some(t) => t.to_string(),
            None => {
                out.push(d);
                continue;
            }
        };
        // `try ` before each call, from the back so offsets stay right
        let mut fixed = text.clone();
        let mut at: Vec<usize> = inner.iter().map(|c| (c.lo - d.span.lo) as usize).collect();
        at.sort();
        at.dedup();
        for i in at.iter().rev() {
            fixed.insert_str(*i, "try ");
        }
        let fixed = format!("try {}", fixed);
        let mut nd = diag::Diag::new(d.span, format!("{} calls here can fail", inner.len() + 1));
        nd = nd.help(if fixed.len() <= 100 && !at.contains(&0) { format!("write `try` before each: `{}`", fixed) } else { "write `try` before each call, or handle the errors with `catch`".to_string() });
        out.push(nd);
    }
    out
}

// `lang new`, `lang add`, `lang fetch`, `lang update`
fn project_command(args: &[String]) -> Option<ExitCode> {
    let cmd = args.first()?.as_str();
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let fail = |e: String| {
        eprintln!("error: {}", e);
        Some(ExitCode::from(1))
    };
    match cmd {
        "new" => {
            let name = match args.get(1) {
                Some(n) => n.clone(),
                None => return fail("usage: lang new <name>".into()),
            };
            let dir = cwd.join(&name);
            if dir.exists() {
                return fail(format!("{} already exists", dir.display()));
            }
            if std::fs::create_dir_all(&dir).is_err() {
                return fail(format!("can't create {}", dir.display()));
            }
            let m = project::Manifest { name: name.clone(), version: "0.1.0".into(), deps: Default::default() };
            if let Err(e) = project::write_manifest(&dir, &m) {
                return fail(e);
            }
            let _ = std::fs::write(dir.join("main.lang"), format!("fn main() {{\n  print(\"hello from {}\")\n}}\n", name));
            println!("created {}/ (lang.toml, main.lang)\nrun it: cd {} && lang run main.lang", name, name);
            Some(ExitCode::SUCCESS)
        }
        "add" => {
            let opt = |flag: &str| args.iter().position(|a| a == flag).and_then(|i| args.get(i + 1)).cloned().unwrap_or_default();
            let version = opt("--version");
            let path = opt("--path");
            let (name, git) = match (args.get(1), args.get(2)) {
                (Some(n), Some(g)) if !g.starts_with("--") => (n.clone(), g.clone()),
                (Some(n), _) if !path.is_empty() => (n.clone(), String::new()),
                _ => return fail("usage: lang add <name> <git-url> [--version <tag>] [--path <dir in the repository>], or lang add <name> --path <dir>".into()),
            };
            let root = project::find_root(&cwd.join("x.lang"));
            let mut m = match project::read_manifest(&root) {
                Ok(Some(m)) => m,
                Ok(None) => return fail("there is no lang.toml here; create a project with `lang new <name>`".into()),
                Err(e) => return fail(e),
            };
            m.deps.insert(name.clone(), project::Dep { git, version, path });
            if let Err(e) = project::write_manifest(&root, &m) {
                return fail(e);
            }
            match project::resolve(&root, false) {
                Ok(dirs) => {
                    println!("added {} ({} package(s) pinned in lang.lock)
use it: import {}", name, dirs.len(), name);
                    Some(ExitCode::SUCCESS)
                }
                Err(e) => fail(e),
            }
        }
        "fetch" | "update" => {
            let root = project::find_root(&cwd.join("x.lang"));
            match project::resolve(&root, cmd == "update") {
                Ok(dirs) => {
                    for (n, d) in &dirs {
                        println!("{} -> {}", n, d.display());
                    }
                    Some(ExitCode::SUCCESS)
                }
                Err(e) => fail(e),
            }
        }
        _ => None,
    }
}

fn compile(opts: &Opts, tests: bool, optimize: bool, exe: &Path) -> bool {
    let mut sources = Sources::default();
    let (prog, uf) = match front_end(&opts.path, &mut sources) {
        Some(x) => x,
        None => return false,
    };
    if tests && prog.tests.is_empty() {
        eprintln!("no tests in {}", opts.path);
        return false;
    }
    if !tests && prog.main.is_none() {
        eprintln!("error: {} has no `fn main()`", opts.path);
        return false;
    }
    let lowerer = lower::Lowerer::new(&prog, &sources, uf);
    let mut module = lowerer.run(tests);
    for f in module.funcs.iter_mut() {
        rc::insert_rc(f, &prog);
    }
    if std::env::var("LANG_DUMP_MIR").is_ok() {
        for f in &module.funcs {
            eprintln!("== {} ({})", f.name, f.source_name);
            for (i, l) in f.locals.iter().enumerate() {
                eprintln!("  l{}: {} {}", i, prog.show(&l.ty), l.name);
            }
            for (i, b) in f.blocks.iter().enumerate() {
                eprintln!(" b{}:", i);
                for s in &b.stmts {
                    eprintln!("    {:?}", s);
                }
                eprintln!("    -> {:?}", b.term);
            }
        }
    }
    let file_name = Path::new(&opts.path).file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let generator = cgen::CGen::new(&prog, &module, file_name);
    let c = generator.generate(tests);
    // in the build directory, so it never overwrites the user's files
    let exe_name = exe.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let c_path = build_dir().join(format!("{}-{}.c", exe_name, std::process::id()));
    if let Err(e) = std::fs::write(&c_path, &c) {
        eprintln!("error: can't write {}: {}", c_path.display(), e);
        return false;
    }
    if let Some(p) = &opts.emit_c {
        let _ = std::fs::write(p, &c);
    }
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".into());
    let mut cmd = Command::new(&cc);
    if opts.debug {
        if cfg!(target_env = "musl") {
            // no AddressSanitizer on musl: the leak counter and trapping UB checks only
            cmd.args(["-O0", "-g", "-fsanitize=undefined", "-fsanitize-trap=undefined", "-DLT_DEBUG_ALLOC"]);
        } else {
            cmd.args(["-O0", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-DLT_DEBUG_ALLOC"]);
        }
    } else {
        cmd.arg(if optimize { "-O2" } else { "-O1" });
    }
    // libraries the program needs, from `// link:` lines
    let mut libs: Vec<String> = c.lines().take(16).filter_map(|l| l.strip_prefix("// link: ")).flat_map(|l| l.split_whitespace().map(String::from).collect::<Vec<_>>()).collect();
    if opts.static_link {
        libs = static_libs(&libs);
        cmd.arg("-static");
    }
    let status = cmd
        .args(["-std=gnu11", "-w", "-fwrapv", "-o"])
        .arg(exe)
        .arg(&c_path)
        .arg("-lm")
        .args(&libs)
        .status();
    // kept when the C compiler fails: the message points at it
    if matches!(&status, Ok(s) if s.success()) && std::env::var("LANG_KEEP_C").is_err() {
        let _ = std::fs::remove_file(&c_path);
    }
    match status {
        Ok(s) if s.success() => true,
        Ok(_) => {
            eprintln!("error: the C compiler failed on {} (this is a compiler bug)", c_path.display());
            false
        }
        Err(e) => {
            eprintln!("error: can't run the C compiler `{}`: {}", cc, e);
            false
        }
    }
}

// A static program needs the libraries' own dependencies too (libcurl needs
// OpenSSL, zlib, nghttp2...): pkg-config knows them.
fn static_libs(libs: &[String]) -> Vec<String> {
    let mut pkgs = vec![];
    let mut rest = vec![];
    for l in libs {
        match l.as_str() {
            "-lcurl" => pkgs.push("libcurl"),
            "-lsqlite3" => pkgs.push("sqlite3"),
            "-lz" => pkgs.push("zlib"),
            "-lssl" => pkgs.push("libssl"),
            "-lcrypto" => pkgs.push("libcrypto"),
            _ => rest.push(l.clone()),
        }
    }
    if pkgs.is_empty() {
        return rest;
    }
    match Command::new("pkg-config").arg("--static").arg("--libs").args(&pkgs).output() {
        Ok(o) if o.status.success() => {
            rest.extend(String::from_utf8_lossy(&o.stdout).split_whitespace().map(String::from));
            rest
        }
        _ => {
            eprintln!("warning: pkg-config didn't find {}; linking may fail (install the static libraries, e.g. `apk add curl-static openssl-libs-static`)", pkgs.join(", "));
            libs.to_vec()
        }
    }
}

fn build_dir() -> PathBuf {
    let d = std::env::temp_dir().join("lang-build");
    let _ = std::fs::create_dir_all(&d);
    d
}

fn main() -> ExitCode {
    let raw: Vec<String> = std::env::args().skip(1).collect();
    if let Some(code) = learn_command(&raw) {
        return code;
    }
    if let Some(code) = project_command(&raw) {
        return code;
    }
    let opts = match parse_args() {
        Some(o) => o,
        None => {
            eprintln!("{}", USAGE);
            return ExitCode::from(2);
        }
    };
    let stem = Path::new(&opts.path).file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or("out".into());
    match opts.cmd.as_str() {
        "check" => {
            let mut sources = Sources::default();
            match front_end(&opts.path, &mut sources) {
                Some(_) => {
                    println!("ok");
                    ExitCode::SUCCESS
                }
                None => ExitCode::from(1),
            }
        }
        "run" | "test" => {
            let tests = opts.cmd == "test";
            let exe = build_dir().join(format!("{}{}-{}", stem, if tests { "-test" } else { "" }, std::process::id()));
            if !compile(&opts, tests, false, &exe) {
                return ExitCode::from(1);
            }
            // `lang run` becomes the program, so signals (Ctrl-C, kill), the exit
            // status and standard input are its own; it deletes its temporary
            // file when it starts (LANG_RUN_EXE)
            use std::os::unix::process::CommandExt;
            let err = Command::new(&exe).arg0(&stem).args(&opts.args).env("LANG_RUN_EXE", &exe).exec();
            let _ = std::fs::remove_file(&exe);
            eprintln!("error: can't run the program: {}", err);
            ExitCode::from(1)
        }
        "build" => {
            if opts.static_link && cfg!(target_os = "macos") {
                eprintln!("error: macOS doesn't allow fully static programs (Apple supports only the system's shared C library).\nThere is no need: `lang build` programs use only libraries that come with macOS, so they run on any Mac as they are.\nFor a static Linux program, build on Linux (Alpine is simplest: see tools/linux/Dockerfile).");
                return ExitCode::from(1);
            }
            if opts.static_link && cfg!(target_env = "gnu") {
                eprintln!("note: with glibc, a static program still loads glibc's modules for looking up host names at run time; build on a musl system (Alpine) for a program that needs nothing");
            }
            let exe = PathBuf::from(opts.out.clone().unwrap_or(stem));
            if compile(&opts, false, true, &exe) {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            }
        }
        _ => {
            eprintln!("{}", USAGE);
            ExitCode::from(2)
        }
    }
}
