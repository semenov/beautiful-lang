#!/usr/bin/env python3
"""Run 4 harness: build (logged), hidden checks, and the live status page.

Every step appends one JSON line to results/events.jsonl and then
re-renders STATUS.md, so STATUS.md is always the current picture.

  harness.py build <task> <lang>          compile, snapshot, log (agents use bin/build)
  harness.py start <task> <lang>          mark a writer agent as started
  harness.py done  <task> <lang> [note]   mark a writer agent as finished
  harness.py check <task> <lang>          run the hidden checks on the first and final builds
  harness.py status                       re-render STATUS.md
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent.parent
TASKS = ROOT / "tasks"
SOL = ROOT / "solutions"
EVENTS = ROOT / "results" / "events.jsonl"
STATUS = ROOT / "STATUS.md"
PLUMB = REPO / "compiler" / "target" / "release" / "plumb"
LANGS = ["go", "plumb", "go-haiku", "plumb-haiku"]


NAMES = {"go": "Go (Sonnet)", "plumb": "Plumb (Sonnet)", "go-haiku": "Go (Haiku)", "plumb-haiku": "Plumb (Haiku)"}


def base(lang):
    return "go" if lang.startswith("go") else "plumb"


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def emit(**ev):
    ev = {"ts": now(), **ev}
    EVENTS.parent.mkdir(parents=True, exist_ok=True)
    with open(EVENTS, "a") as f:
        f.write(json.dumps(ev) + "\n")
    render()


def events():
    if not EVENTS.exists():
        return []
    out = []
    for line in EVENTS.read_text().splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def src_files(d, lang):
    ext = ".go" if base(lang) == "go" else ".plumb"
    return sorted(p for p in d.rglob("*" + ext) if ".builds" not in p.parts)


def loc(d, lang):
    """Non-blank, non-comment lines of program code (tests excluded)."""
    n = 0
    for p in src_files(d, lang):
        if base(lang) == "go" and p.name.endswith("_test.go"):
            continue
        depth_skip = None  # brace depth where a plumb `test` block started
        depth = 0
        for line in p.read_text(errors="replace").splitlines():
            s = line.strip()
            if base(lang) == "plumb" and depth_skip is None and re.match(r'^test\s+"', s):
                depth_skip = depth
            opens, closes = s.count("{"), s.count("}")
            counted = depth_skip is None and s and not s.startswith("//")
            depth += opens - closes
            if depth_skip is not None and depth <= depth_skip and closes:
                depth_skip = None
                continue
            if counted:
                n += 1
    return n


def cmd_build(task, lang):
    d = SOL / task / lang
    if not d.exists():
        sys.exit(f"no solution directory {d}")
    builds = d / ".builds"
    builds.mkdir(exist_ok=True)
    n = len([p for p in builds.iterdir() if p.is_dir()]) + 1
    snap = builds / f"{n:03d}"
    snap.mkdir()
    for p in src_files(d, lang):
        dst = snap / p.relative_to(d)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)
    for extra in ["go.mod", "go.sum"]:
        if (d / extra).exists():
            shutil.copy2(d / extra, snap / extra)

    env = dict(os.environ)
    if base(lang) == "go":
        env.update(GOPROXY="off", GOFLAGS="-mod=mod", CGO_ENABLED="1")
        steps = [["go", "build", "-o", "app", "."], ["go", "vet", "."]]
    else:
        steps = [[str(PLUMB), "build", "main.plumb", "-o", "app"]]
    t0 = time.time()
    output, ok = "", True
    for st in steps:
        r = subprocess.run(st, cwd=d, env=env, capture_output=True, text=True)
        output += r.stdout + r.stderr
        if r.returncode != 0:
            ok = False
            break
    secs = round(time.time() - t0, 2)
    (snap / "build.log").write_text(output)
    if ok:
        shutil.copy2(d / "app", snap / "app")
    errs = [l for l in output.splitlines() if l.strip()]
    emit(kind="build", task=task, lang=lang, n=n, ok=ok, secs=secs,
         loc=loc(d, lang), errors=errs[:5], error_lines=len(errs))
    sys.stdout.write(output)
    print(f"[build #{n}: {'ok' if ok else 'FAILED'} in {secs}s]")
    sys.exit(0 if ok else 1)


def ok_snapshots(task, lang):
    b = SOL / task / lang / ".builds"
    if not b.exists():
        return []
    return sorted(p for p in b.iterdir() if (p / "app").exists())


def run_check(task, lang, snap, which):
    check = TASKS / task / "check.py"
    # The binary is copied under another name: writer agents testing their own
    # servers run things like `pkill -f app`, which would hit the check's server.
    work = Path(f"/tmp/r4c/{task}-{lang}-{which}")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    subject = Path("/tmp/r4c/bin") / f"subject-{task[:2]}-{lang}-{which}"
    subject.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(snap / "app", subject)
    try:
        r = subprocess.run(["python3", str(check), str(subject), str(work)],
                           capture_output=True, text=True, timeout=600)
        last = [l for l in r.stdout.splitlines() if l.startswith("{")]
        res = json.loads(last[-1]) if last else {"error": ("exit %s: " % r.returncode) + (r.stdout + r.stderr)[-2000:]}
    except subprocess.TimeoutExpired:
        res = {"error": "check timed out"}
    cases = res.get("cases", [])
    passed = sum(1 for c in cases if c.get("ok"))
    (snap / f"check-{which}.json").write_text(json.dumps(res, indent=1))
    emit(kind="check", task=task, lang=lang, which=which, build=snap.name,
         passed=passed, total=len(cases), error=res.get("error"),
         failed=[c["name"] for c in cases if not c.get("ok")])
    print(f"{task}/{lang} {which} (build {snap.name}): {passed}/{len(cases)}")


def cmd_check(task, lang):
    snaps = ok_snapshots(task, lang)
    if not snaps:
        emit(kind="check", task=task, lang=lang, which="final", build=None,
             passed=0, total=0, error="never compiled", failed=[])
        return
    run_check(task, lang, snaps[0], "first")
    if snaps[-1] != snaps[0]:
        run_check(task, lang, snaps[-1], "final")
    else:
        res = json.loads((snaps[0] / "check-first.json").read_text())
        (snaps[0] / "check-final.json").write_text(json.dumps(res, indent=1))
        cases = res.get("cases", [])
        emit(kind="check", task=task, lang=lang, which="final", build=snaps[0].name,
             passed=sum(1 for c in cases if c.get("ok")), total=len(cases),
             error=res.get("error"), failed=[c["name"] for c in cases if not c.get("ok")])


def render():
    evs = events()
    tasks = sorted(p.name for p in TASKS.iterdir() if (p / "SPEC.md").exists()) if TASKS.exists() else []
    cell = {}
    for t in tasks:
        for l in LANGS:
            cell[(t, l)] = dict(state="waiting", builds=0, fails=0, first_ok=None,
                                loc=None, first=None, final=None, start=None, end=None)
    for e in evs:
        c = cell.get((e.get("task"), e.get("lang")))
        if c is None:
            continue
        k = e["kind"]
        if k == "start":
            c["state"], c["start"] = "writing", e["ts"]
        elif k == "build":
            c["builds"] += 1
            if not e["ok"]:
                c["fails"] += 1
            elif c["first_ok"] is None:
                c["first_ok"] = c["builds"]
            c["loc"] = e["loc"]
            c["last_build_ok"] = e["ok"]
        elif k == "done":
            c["end"] = e["ts"]
            if c["final"] is None:
                c["state"] = "done"
        elif k == "check":
            c[e["which"]] = (e["passed"], e["total"])
            if e["which"] == "final":
                c["state"] = "checked"

    def fmt(c):
        icon = {"waiting": "·", "writing": "✍️", "done": "⏳", "checked": "✅"}[c["state"]]
        if c["state"] == "writing" and c["builds"]:
            icon = "🔨" if c.get("last_build_ok") else "🔴"
        parts = [icon]
        if c["builds"]:
            parts.append(f"b{c['builds']}" + (f"/{c['fails']}✗" if c["fails"] else ""))
        if c["loc"]:
            parts.append(f"{c['loc']}L")
        if c["first"]:
            parts.append(f"1st {c['first'][0]}/{c['first'][1]}")
        if c["final"]:
            parts.append(f"fin {c['final'][0]}/{c['final'][1]}")
        return " ".join(parts)

    def agg(l):
        done = [c for (t, ll), c in cell.items() if ll == l and c["final"]]
        if not done:
            return None
        s = lambda key, i: sum(c[key][i] for c in done if c[key])
        return dict(
            n=len(done),
            first=f"{s('first', 0)}/{s('first', 1)}",
            final=f"{s('final', 0)}/{s('final', 1)}",
            perfect=sum(1 for c in done if c["final"][1] and c["final"][0] == c["final"][1]),
            loc=sum(c["loc"] or 0 for c in done),
            builds=sum(c["builds"] for c in done),
            fails=sum(c["fails"] for c in done),
            tries=sum((c["first_ok"] or c["builds"]) for c in done),
        )

    shown = [l for l in LANGS if l in ("go", "plumb") or any(c["state"] != "waiting" for (t, ll), c in cell.items() if ll == l)]
    counts = {s: sum(1 for c in cell.values() if c["state"] == s) for s in ["waiting", "writing", "done", "checked"]}
    out = ["# Run 4: Go vs Plumb, live status", "",
           f"Updated {now()}. {len(tasks)} tasks × 2 languages: "
           f"{counts['checked']} checked, {counts['done']} awaiting checks, "
           f"{counts['writing']} being written, {counts['waiting']} not started.", ""]
    out += ["## Totals (checked cells only)", "",
            "| | tasks | hidden tests passed, first compiled version | final version | all tests pass | build attempts (failed) | builds until first success | lines of code |",
            "|---|---|---|---|---|---|---|---|"]
    for l in shown:
        a = agg(l)
        if a:
            out.append(f"| {NAMES[l]} | {a['n']} | {a['first']} | {a['final']} | {a['perfect']}/{a['n']} | {a['builds']} ({a['fails']}) | {a['tries']} | {a['loc']} |")
        else:
            out.append(f"| {NAMES[l]} | 0 | | | | | | |")
    out += ["", "## Per task", "",
            "`✍️` writing, `🔨` last build ok, `🔴` last build failed, `⏳` done, waiting for checks, `✅` checked. "
            "`b7/2✗` 7 builds, 2 failed; `120L` lines of code; `1st 9/12` hidden tests passed by the first version "
            "that compiled; `fin 12/12` by the final one.", "",
            "| task | " + " | ".join(NAMES[l] for l in shown) + " |", "|---|" + "---|" * len(shown)]
    for t in tasks:
        out.append(f"| {t} | " + " | ".join(fmt(cell[(t, l)]) for l in shown) + " |")
    recent = [e for e in evs if e["kind"] != "status"][-12:]
    out += ["", "## Latest events", ""]
    for e in reversed(recent):
        d = f"{e['ts'][11:]} {e.get('task')}/{e.get('lang')} {e['kind']}"
        if e["kind"] == "build":
            d += f" #{e['n']} {'ok' if e['ok'] else 'failed: ' + (e['errors'][0][:100] if e['errors'] else '')}"
        elif e["kind"] == "check":
            d += f" {e['which']}: {e['passed']}/{e['total']}" + (f" ({e['error'][:80]})" if e.get("error") else "")
        elif e.get("note"):
            d += f": {e['note'][:100]}"
        out.append(f"- {d}")
    tmp = STATUS.with_suffix(".tmp")
    tmp.write_text("\n".join(out) + "\n")
    tmp.replace(STATUS)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "build":
        cmd_build(a[1], a[2])
    elif a[0] == "start":
        emit(kind="start", task=a[1], lang=a[2])
    elif a[0] == "done":
        d = SOL / a[1] / a[2]
        kv = dict(x.split("=", 1) for x in a[3:] if "=" in x)
        emit(kind="done", task=a[1], lang=a[2], note=" ".join(x for x in a[3:] if "=" not in x),
             loc=loc(d, a[2]) if d.exists() else None, **kv)
    elif a[0] == "check":
        cmd_check(a[1], a[2])
    elif a[0] == "status":
        render()
        print(STATUS.read_text())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
