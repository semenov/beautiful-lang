mod ast;
mod cgen;
mod check;
mod diag;
mod lexer;
mod lower;
mod mir;
mod parser;
mod rc;
mod types;

use diag::Sources;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const USAGE: &str = "usage:
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
        _ => return None,
    })
}

// Parses and checks; prints errors. Returns the program on success.
fn front_end(path: &str, sources: &mut Sources) -> Option<(types::Program, u32)> {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("error: can't read {}: {}", path, e);
            return None;
        }
    };
    let parse = |sources: &mut Sources, name: &str, src: &str, privileged: bool| -> Result<ast::Module, ()> {
        let f = sources.add(name.to_string(), src.to_string());
        lexer::lex(src, f).and_then(|t| parser::parse_module(t, privileged)).map_err(|d| eprint!("{}", sources.render(&d)))
    };
    let prelude = parse(sources, "<prelude>", include_str!("prelude.lang"), true).ok()?;
    let uf = sources.files.len() as u32;
    let user = parse(sources, path, &text, false).ok()?;
    // standard library modules the program uses, and the ones they use
    let mut mods: Vec<(String, ast::Module, bool)> = vec![("<prelude>".into(), prelude, true)];
    let mut pending: Vec<(String, diag::Span)> = user.imports.clone();
    let mut loaded: Vec<String> = vec![];
    while let Some((name, span)) = pending.pop() {
        if loaded.contains(&name) {
            continue;
        }
        match std_module(&name) {
            Some(src) => {
                let m = parse(sources, &format!("<{}>", name), src, true).ok()?;
                pending.extend(m.imports.iter().cloned());
                loaded.push(name.clone());
                mods.push((name, m, true));
            }
            None => {
                let d = diag::Diag::new(span, format!("unknown module `{}`", name));
                eprint!("{}", sources.render(&d));
                return None;
            }
        }
    }
    mods.push((path.to_string(), user, false));
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
