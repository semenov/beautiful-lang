// Projects and packages.
//
// A project is a directory with `lang.toml`:
//
//     [package]
//     name = "notes"
//     version = "0.1.0"
//
//     [dependencies]
//     router = { git = "https://github.com/someone/router", version = "v1.2.0" }
//     redis = { git = "https://github.com/someone/tools", version = "v2.0.0", path = "packages/redis" }
//     mylib = { path = "../mylib" }
//
// `path` with `git` is a package in a subdirectory of the repository (one
// repository can hold several); `path` alone is a package on this disk
// (for developing packages side by side), relative to the project.
//
// `lang add` records a dependency and pins the exact commit in `lang.lock`.
// Packages are fetched with git into ~/.lang/packages/<name>/<commit>/.
// There is no central registry: a package is a git repository and a
// version is a tag (like Go modules). One version of each package per build.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Debug, Clone, Default)]
pub struct Manifest {
    pub name: String,
    pub version: String,
    pub deps: BTreeMap<String, Dep>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Dep {
    pub git: String,
    pub version: String,
    pub path: String,
}

#[derive(Debug, Clone, Default)]
pub struct Locked {
    pub git: String,
    pub version: String,
    pub commit: String,
}

// ---- a small TOML subset: [tables], key = "string", key = { k = "v", ... }

fn parse_toml(text: &str) -> Result<BTreeMap<String, BTreeMap<String, TomlValue>>, String> {
    let mut out: BTreeMap<String, BTreeMap<String, TomlValue>> = BTreeMap::new();
    let mut table = String::new();
    for (n, raw) in text.lines().enumerate() {
        let line = raw.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        if line.starts_with('[') {
            table = line.trim_matches(|c| c == '[' || c == ']').trim().to_string();
            out.entry(table.clone()).or_default();
            continue;
        }
        let (k, v) = line.split_once('=').ok_or(format!("line {}: expected `key = value`", n + 1))?;
        let value = parse_value(v.trim()).ok_or(format!("line {}: can't read the value", n + 1))?;
        out.entry(table.clone()).or_default().insert(k.trim().trim_matches('"').to_string(), value);
    }
    Ok(out)
}

#[derive(Debug, Clone)]
enum TomlValue {
    Str(String),
    Table(BTreeMap<String, String>),
}

fn parse_value(v: &str) -> Option<TomlValue> {
    if let Some(s) = v.strip_prefix('"').and_then(|s| s.strip_suffix('"')) {
        return Some(TomlValue::Str(s.to_string()));
    }
    if let Some(inner) = v.strip_prefix('{').and_then(|s| s.strip_suffix('}')) {
        let mut m = BTreeMap::new();
        for part in inner.split(',') {
            let part = part.trim();
            if part.is_empty() {
                continue;
            }
            let (k, v) = part.split_once('=')?;
            let v = v.trim().strip_prefix('"')?.strip_suffix('"')?;
            m.insert(k.trim().to_string(), v.to_string());
        }
        return Some(TomlValue::Table(m));
    }
    None
}

pub fn read_manifest(root: &Path) -> Result<Option<Manifest>, String> {
    let path = root.join("lang.toml");
    let text = match std::fs::read_to_string(&path) {
        Ok(t) => t,
        Err(_) => return Ok(None),
    };
    let t = parse_toml(&text).map_err(|e| format!("{}: {}", path.display(), e))?;
    let mut m = Manifest::default();
    if let Some(p) = t.get("package") {
        if let Some(TomlValue::Str(s)) = p.get("name") {
            m.name = s.clone();
        }
        if let Some(TomlValue::Str(s)) = p.get("version") {
            m.version = s.clone();
        }
    }
    if let Some(d) = t.get("dependencies") {
        for (name, v) in d {
            match v {
                TomlValue::Table(tbl) => {
                    let sub = tbl.get("path").cloned().unwrap_or_default();
                    let git = match tbl.get("git").cloned() {
                        Some(g) => g,
                        None if !sub.is_empty() => String::new(),
                        None => return Err(format!("{}: dependency `{}` needs `git = \"...\"` (or `path = \"...\"` for a package on this disk)", path.display(), name)),
                    };
                    let version = tbl.get("version").cloned().unwrap_or_default();
                    m.deps.insert(name.clone(), Dep { git, version, path: sub });
                }
                TomlValue::Str(_) => return Err(format!("{}: write `{} = {{ git = \"...\", version = \"...\" }}`", path.display(), name)),
            }
        }
    }
    Ok(Some(m))
}

pub fn write_manifest(root: &Path, m: &Manifest) -> Result<(), String> {
    let mut s = format!("[package]\nname = \"{}\"\nversion = \"{}\"\n", m.name, m.version);
    if !m.deps.is_empty() {
        s += "\n[dependencies]\n";
        for (n, d) in &m.deps {
            let mut parts = vec![];
            if !d.git.is_empty() {
                parts.push(format!("git = \"{}\"", d.git));
            }
            if !d.version.is_empty() {
                parts.push(format!("version = \"{}\"", d.version));
            }
            if !d.path.is_empty() {
                parts.push(format!("path = \"{}\"", d.path));
            }
            s += &format!("{} = {{ {} }}\n", n, parts.join(", "));
        }
    }
    std::fs::write(root.join("lang.toml"), s).map_err(|e| e.to_string())
}

pub fn read_lock(root: &Path) -> BTreeMap<String, Locked> {
    let mut out = BTreeMap::new();
    let text = match std::fs::read_to_string(root.join("lang.lock")) {
        Ok(t) => t,
        Err(_) => return out,
    };
    if let Ok(t) = parse_toml(&text) {
        for (name, tbl) in t {
            if let Some(n) = name.strip_prefix("package.") {
                let get = |k: &str| match tbl.get(k) {
                    Some(TomlValue::Str(s)) => s.clone(),
                    _ => String::new(),
                };
                out.insert(n.to_string(), Locked { git: get("git"), version: get("version"), commit: get("commit") });
            }
        }
    }
    out
}

pub fn write_lock(root: &Path, lock: &BTreeMap<String, Locked>) -> Result<(), String> {
    let mut s = String::from("# Written by `lang add` / `lang fetch`. Pins every package to an exact commit.\n");
    for (n, l) in lock {
        s += &format!("\n[package.{}]\ngit = \"{}\"\nversion = \"{}\"\ncommit = \"{}\"\n", n, l.git, l.version, l.commit);
    }
    std::fs::write(root.join("lang.lock"), s).map_err(|e| e.to_string())
}

// The project root: the nearest directory with `lang.toml`, else the file's directory.
pub fn find_root(file: &Path) -> PathBuf {
    let abs = std::fs::canonicalize(file).unwrap_or_else(|_| file.to_path_buf());
    let mut dir = abs.parent().map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from("."));
    let start = dir.clone();
    loop {
        if dir.join("lang.toml").exists() {
            return dir;
        }
        if !dir.pop() {
            return start;
        }
    }
}

pub fn packages_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".lang").join("packages")
}

pub fn package_path(name: &str, commit: &str) -> PathBuf {
    packages_dir().join(name).join(commit)
}

fn git(args: &[&str], dir: Option<&Path>) -> Result<String, String> {
    let mut c = Command::new("git");
    c.args(args);
    if let Some(d) = dir {
        c.current_dir(d);
    }
    let out = c.output().map_err(|e| format!("can't run git: {}", e))?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

// Downloads `dep` (the tag `version`, or the default branch) and returns the
// exact commit. The files end up in package_path(name, commit).
pub fn fetch(name: &str, dep: &Dep) -> Result<Locked, String> {
    let tmp = packages_dir().join(format!(".download-{}-{}", name, std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    std::fs::create_dir_all(packages_dir()).map_err(|e| e.to_string())?;
    let mut args = vec!["clone", "--quiet", "--depth", "1"];
    if !dep.version.is_empty() {
        args.push("--branch");
        args.push(&dep.version);
    }
    let tmp_s = tmp.to_string_lossy().to_string();
    args.push(&dep.git);
    args.push(&tmp_s);
    git(&args, None).map_err(|e| format!("can't download `{}` from {}: {}", name, dep.git, e))?;
    let commit = git(&["rev-parse", "HEAD"], Some(&tmp))?;
    let dest = package_path(name, &commit);
    if dest.exists() {
        let _ = std::fs::remove_dir_all(&tmp);
    } else {
        std::fs::create_dir_all(dest.parent().unwrap()).map_err(|e| e.to_string())?;
        let _ = std::fs::remove_dir_all(tmp.join(".git"));
        std::fs::rename(&tmp, &dest).map_err(|e| e.to_string())?;
    }
    Ok(Locked { git: dep.git.clone(), version: dep.version.clone(), commit })
}

// Makes sure every dependency (and theirs) is downloaded and pinned.
// Returns package name -> directory.
pub fn resolve(root: &Path, update: bool) -> Result<BTreeMap<String, PathBuf>, String> {
    let manifest = match read_manifest(root)? {
        Some(m) => m,
        None => return Ok(BTreeMap::new()),
    };
    let mut lock = read_lock(root);
    let mut dirs = BTreeMap::new();
    let mut pending: Vec<(String, Dep, String)> = manifest.deps.iter().map(|(n, d)| (n.clone(), d.clone(), manifest.name.clone())).collect();
    let mut changed = false;
    while let Some((name, dep, wanted_by)) = pending.pop() {
        if dep.git.is_empty() {
            // on this disk: nothing to download or pin
            if dirs.contains_key(&name) {
                continue;
            }
            let dir = root.join(&dep.path);
            if !dir.join(format!("{}.lang", name)).exists() {
                return Err(format!("package `{}`: there is no {}.lang in {}", name, name, dir.display()));
            }
            if let Some(sub) = read_manifest(&dir)? {
                for (n, d) in sub.deps {
                    let d = if d.git.is_empty() { Dep { path: dir.join(&d.path).to_string_lossy().to_string(), ..d } } else { d };
                    pending.push((n, d, name.clone()));
                }
            }
            dirs.insert(name, dir);
            continue;
        }
        if dirs.contains_key(&name) {
            if let Some(l) = lock.get(&name) {
                if l.git != dep.git {
                    return Err(format!("`{}` is wanted from two places: {} and {} (by `{}`)", name, l.git, dep.git, wanted_by));
                }
            }
            continue;
        }
        let locked = match lock.get(&name) {
            Some(l) if !update && l.git == dep.git && l.version == dep.version && package_path(&name, &l.commit).exists() => l.clone(),
            Some(l) if !update && l.git == dep.git && l.version == dep.version => {
                // pinned but not downloaded here yet: fetch exactly that commit
                let got = fetch(&name, &dep)?;
                if got.commit != l.commit {
                    return Err(format!("`{}` {} now points to commit {}, but lang.lock pins {}; run `lang update` if the change is expected", name, dep.version, got.commit, l.commit));
                }
                got
            }
            _ => {
                changed = true;
                fetch(&name, &dep)?
            }
        };
        let dir = package_path(&name, &locked.commit).join(&dep.path);
        if !dir.join(format!("{}.lang", name)).exists() {
            return Err(format!("package `{}`: there is no {}.lang in {}{}", name, name, dep.git, if dep.path.is_empty() { String::new() } else { format!(" / {}", dep.path) }));
        }
        if let Some(sub) = read_manifest(&dir)? {
            for (n, d) in sub.deps {
                pending.push((n, d, name.clone()));
            }
        }
        lock.insert(name.clone(), locked);
        dirs.insert(name, dir);
    }
    lock.retain(|n, _| dirs.contains_key(n));
    if changed || update {
        write_lock(root, &lock)?;
    }
    Ok(dirs)
}
