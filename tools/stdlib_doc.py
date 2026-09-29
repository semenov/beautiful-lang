#!/usr/bin/env python3
"""Writes STDLIB.md from the standard library's sources.

The reference is the public part of compiler/src/prelude.lang and
compiler/src/std/*.lang: doc comments, `pub` declarations and fields, with
bodies and private items left out. Run it after changing the stdlib:

    python3 tools/stdlib_doc.py
"""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "compiler" / "src"

# The order modules appear in: most used first.
ORDER = [
    "files", "path", "io", "process", "env", "cli", "term", "log", "time", "json",
    "http", "net", "sql", "db", "crypto", "encoding", "random", "regex",
    "csv", "xml", "template", "url", "zlib", "archive", "math",
]


def public_api(text: str, prelude: bool) -> tuple[str, str]:
    """(module intro, API listing) from a module's source."""
    lines = text.split("\n")
    intro = []
    i = 0
    while i < len(lines) and lines[i].startswith("//"):
        body = lines[i][2:]
        intro.append(body[1:] if body.startswith(" ") else body)
        i += 1
    intro = fence_code(intro)
    out = []
    pending = []  # comments waiting for the declaration they describe
    depth = 0
    skipping_until = None  # depth at which a skipped body ends
    type_depth = None  # depth inside a public type's braces
    for line in lines[i:]:
        s = line.strip()
        opens, closes = s.count("{"), s.count("}")
        if skipping_until is not None:
            depth += opens - closes
            if depth <= skipping_until:
                skipping_until = None
            continue
        if s.startswith("//"):
            pending.append(line)
            continue
        if not s:
            pending = []
            if out and out[-1] != "" and type_depth is None:
                out.append("")
            continue
        if s.startswith("import "):
            pending = []
            continue
        top = depth == 0
        in_type = type_depth is not None and depth == type_depth
        if top:
            public = s.startswith("pub ") or (prelude and re.match(r"(builtin type|type|enum|fn)\b", s))
            if not public:
                pending = []
                depth += opens - closes
                if opens > closes:
                    skipping_until = 0
                continue
            decl = s
            if re.match(r"(pub )?(builtin )?(type|enum)\b", s) and "{" in s and not s.startswith(("pub fn", "fn")):
                out.extend(pending)
                out.append(line.rstrip())
                pending = []
                depth += opens - closes
                type_depth = depth if opens > closes else None
                continue
            # a function: the signature without its body
            out.extend(pending)
            out.append(decl.split(" {")[0] if decl.endswith("{") else decl)
            pending = []
            depth += opens - closes
            if opens > closes:
                skipping_until = 0
            continue
        if in_type:
            if s == "}":
                out.append(line.rstrip())
                depth -= 1
                type_depth = None
                pending = []
                continue
            is_method = re.match(r"(pub )?(mutating )?fn\b", s)
            if is_method:
                visible = s.startswith("pub ") or prelude or "builtin" in "".join(out[-200:])
                # methods of builtin types and of prelude types are public
                if not s.startswith("pub ") and not prelude and not builtin_type_open(out):
                    pending = []
                    depth += opens - closes
                    if opens > closes:
                        skipping_until = type_depth
                    continue
                out.extend(pending)
                sig = line.rstrip()
                if sig.endswith("{"):
                    sig = sig[: sig.rfind(" {")]
                out.append(sig)
                pending = []
                depth += opens - closes
                if opens > closes:
                    skipping_until = type_depth
                continue
            # a field or a variant
            out.extend(pending)
            out.append(line.rstrip())
            pending = []
            depth += opens - closes
            continue
        depth += opens - closes
    # squeeze blank lines
    text_out = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    return "\n".join(intro).strip(), text_out


def fence_code(lines: list[str]) -> list[str]:
    """Indented comment lines (examples) become fenced code blocks."""
    out, code = [], []
    for line in lines + [""]:
        if line.startswith("  "):
            code.append(line[2:])
            continue
        if code:
            while out and out[-1] == "":
                out.pop()
            out += ["", "```", *code, "```", ""]
            code = []
            if line == "":
                continue
        out.append(line)
    return out


def builtin_type_open(out: list[str]) -> bool:
    for line in reversed(out):
        if re.match(r"\s*(pub )?(builtin )?(type|enum)\b", line) and line.rstrip().endswith("{"):
            return "builtin" in line
    return False


def main() -> None:
    std = SRC / "std"
    names = [n for n in ORDER if (std / f"{n}.lang").exists()]
    names += sorted(p.stem for p in std.glob("*.lang") if p.stem not in names)
    doc = [
        "# Standard library",
        "",
        "Generated from the sources by `tools/stdlib_doc.py`: the public",
        "declarations with their comments, bodies left out. `import <module>`,",
        "then use `module.name`. Functions without a body are built into the",
        "compiler and runtime.",
        "",
        "Modules: " + ", ".join(f"[`{n}`](#{n})" for n in names) + ", and the [prelude](#prelude)",
        "(available everywhere without `import`).",
        "",
    ]
    for n in names:
        intro, api = public_api((std / f"{n}.lang").read_text(), prelude=False)
        doc += [f"## {n}", ""]
        if intro:
            doc += [intro, ""]
        doc += ["```", api, "```", ""]
    intro, api = public_api((SRC / "prelude.lang").read_text(), prelude=True)
    doc += ["## prelude", "", "Types and functions available in every file.", "", "```", api, "```", ""]
    (ROOT / "STDLIB.md").write_text("\n".join(doc))
    print(f"STDLIB.md: {len(names)} modules")


if __name__ == "__main__":
    main()
