mod ast;
mod cgen;
mod check;
mod diag;
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

const USAGE: &str = "usage:
  lang new <name>                   create a project
  lang add <name> <git-url> [--version <tag>]   add a package
  lang fetch                        download the packages in lang.lock
  lang update                       move packages to their newest matching versions
  lang run <file.lang> [args]       compile and run
  lang build <file.lang> [-o out]   compile an optimized binary
  lang test <file.lang>             run the tests in the file
  lang check <file.lang>            only check for errors
options:
  --emit-c <file.c>                 also write the generated C
  --debug                           check memory safety and leaks (slow)";

struct Opts {
    cmd: String,
    path: String,
    out: Option<String>,
    emit_c: Option<String>,
    debug: bool,
    args: Vec<String>,
}

fn parse_args() -> Option<Opts> {
    let mut args = std::env::args().skip(1);
    let cmd = args.next()?;
    let mut path = None;
    let mut out = None;
    let mut emit_c = None;
    let mut debug = false;
    let mut rest = vec![];
    while let Some(a) = args.next() {
        match a.as_str() {
            "-o" if path.is_none() || cmd != "run" => out = Some(args.next()?),
            "--emit-c" => emit_c = Some(args.next()?),
            "--debug" if path.is_none() => debug = true,
            _ if path.is_none() => path = Some(a),
            _ => rest.push(a),
        }
    }
    Some(Opts { cmd, path: path?, out, emit_c, debug, args: rest })
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
    c.check_program(&mods);
    if !c.diags.is_empty() {
        let mut ds = c.diags.clone();
        ds.sort_by_key(|d| (d.span.file, d.span.lo));
        for d in &ds {
            eprint!("{}", sources.render(d));
        }
        eprintln!("{} error(s)", ds.len());
        return None;
    }
    Some((c.prog, uf))
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
            let (name, git) = match (args.get(1), args.get(2)) {
                (Some(n), Some(g)) => (n.clone(), g.clone()),
                _ => return fail("usage: lang add <name> <git-url> [--version <tag>]".into()),
            };
            let version = args.iter().position(|a| a == "--version").and_then(|i| args.get(i + 1)).cloned().unwrap_or_default();
            let root = project::find_root(&cwd.join("x.lang"));
            let mut m = match project::read_manifest(&root) {
                Ok(Some(m)) => m,
                Ok(None) => return fail("there is no lang.toml here; create a project with `lang new <name>`".into()),
                Err(e) => return fail(e),
            };
            m.deps.insert(name.clone(), project::Dep { git, version });
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
    let c_path = exe.with_extension("c");
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
        cmd.args(["-O0", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-DLT_DEBUG_ALLOC"]);
    } else {
        cmd.arg(if optimize { "-O2" } else { "-O1" });
    }
    let status = cmd
        .args(["-std=gnu11", "-w", "-fwrapv", "-o"])
        .arg(exe)
        .arg(&c_path)
        .arg("-lm")
        .status();
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

fn build_dir() -> PathBuf {
    let d = std::env::temp_dir().join("lang-build");
    let _ = std::fs::create_dir_all(&d);
    d
}

fn main() -> ExitCode {
    let raw: Vec<String> = std::env::args().skip(1).collect();
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
            let status = Command::new(&exe).args(&opts.args).status();
            let _ = std::fs::remove_file(&exe);
            if std::env::var("LANG_KEEP_C").is_err() {
                let _ = std::fs::remove_file(exe.with_extension("c"));
            }
            match status {
                Ok(s) => ExitCode::from(s.code().unwrap_or(1) as u8),
                Err(e) => {
                    eprintln!("error: can't run the program: {}", e);
                    ExitCode::from(1)
                }
            }
        }
        "build" => {
            let exe = PathBuf::from(opts.out.clone().unwrap_or(stem));
            if compile(&opts, false, true, &exe) {
                let _ = std::fs::remove_file(exe.with_extension("c"));
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
