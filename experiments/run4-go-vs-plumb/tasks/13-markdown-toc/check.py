import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks

c = Checks()

def expect(name, r, out, code=0):
    c.case(name, r.code == code and r.out == out, r)

doc = "# Größe & Maße\n\nText.\n\n## Setup\n\n#### Deep\n\n## Setup ##\n\n```sh\n# not a heading\n```\n\n### Step 1.2: run `make`\n"
(c.work / "doc.md").write_text(doc)
expect("example", c.run(["--max-level", "3", "doc.md"]),
       "- [Größe & Maße](#größe--maße)\n  - [Setup](#setup)\n  - [Setup](#setup-1)\n    - [Step 1.2: run `make`](#step-12-run-make)\n")

expect("all levels by default", c.run([], stdin="# A\n## B\n### C\n#### D\n##### E\n###### F\n"),
       "- [A](#a)\n  - [B](#b)\n    - [C](#c)\n      - [D](#d)\n        - [E](#e)\n          - [F](#f)\n")

expect("level range; slugs count headings outside it", c.run(["--min-level", "2", "--max-level", "3"],
       stdin="# Intro\n## Intro\n### Usage\n#### Usage\n## Usage\n"),
       "- [Intro](#intro-1)\n  - [Usage](#usage)\n- [Usage](#usage-2)\n")

expect("lines that are not headings", c.run([], stdin=(
       "#hashtag\n####### seven\n    # four spaces\n\\# escaped\nSetext\n======\nAlso\n------\n"
       "text # not at start\n   ### Three spaces\n#\tTab\n")),
       "    - [Three spaces](#three-spaces)\n- [Tab](#tab)\n")

expect("closing hashes and empty headings", c.run([], stdin=(
       "## Title ##\n# C#\n### ###\n#\n##   \n## a ## b\n# Spaced #   \n## x \\#\n# #tag\n")),
       "  - [Title](#title)\n- [C#](#c)\n  - [a ## b](#a--b)\n- [Spaced](#spaced)\n  - [x \\#](#x-)\n- [#tag](#tag)\n")

expect("fenced code blocks", c.run([], stdin=(
       "# Before\n```python\n# comment\n~~~\n# still code\n```\n## Middle\n~~~~\n# code\n~~~\n# code too\n~~~~~  \n"
       "## After tilde\n  ```\n# indented fence content\n   ````\n### After indented\n```\n# x\n``` not a close\n# still code\n```\n"
       "## End\n")),
       "- [Before](#before)\n  - [Middle](#middle)\n  - [After tilde](#after-tilde)\n    - [After indented](#after-indented)\n"
       "  - [End](#end)\n")

expect("unclosed fence runs to the end", c.run([], stdin="# One\n````\n# Two\n```\n# Three\n"), "- [One](#one)\n")
expect("four-space indented fence is not a fence", c.run([], stdin="# One\n    ```\n# Two\n"), "- [One](#one)\n- [Two](#two)\n")

expect("slug punctuation and spacing", c.run([], stdin=(
       "# Hello, World!\n# a  b\n# snake_case and kebab-case\n# 100% (sure)?\n# see [docs](http://x.y/z)\n# Tab\there\n")),
       "- [Hello, World!](#hello-world)\n- [a  b](#a--b)\n- [snake_case and kebab-case](#snake_case-and-kebab-case)\n"
       "- [100% (sure)?](#100-sure)\n- [see [docs](http://x.y/z)](#see-docshttpxyz)\n- [Tab\there](#tabhere)\n")

expect("unicode slugs", c.run([], stdin=(
       "# ÉTÉ à Paris\n# Привет Мир\n# 日本語の見出し\n# Emoji 🎉 party\n# Cafe\u0301 NFD\n# Ωμέγα 2\n")),
       "- [ÉTÉ à Paris](#été-à-paris)\n- [Привет Мир](#привет-мир)\n- [日本語の見出し](#日本語の見出し)\n"
       "- [Emoji 🎉 party](#emoji--party)\n- [Cafe\u0301 NFD](#cafe\u0301-nfd)\n- [Ωμέγα 2](#ωμέγα-2)\n")

expect("duplicate slugs numbered", c.run([], stdin="# A\n# a\n## A!\n# B\n# A\n# !!!\n# ???\n"),
       "- [A](#a)\n- [a](#a-1)\n  - [A!](#a-2)\n- [B](#b)\n- [A](#a-3)\n- [!!!](#)\n- [???](#-1)\n")

expect("CRLF line endings", c.run([], stdin=b"# One\r\n## Two ##\r\n```\r\n# no\r\n```\r\n## Three\r\n"),
       "- [One](#one)\n  - [Two](#two)\n  - [Three](#three)\n")

expect("no headings", c.run([], stdin="just text\n\nmore text\n"), "")
expect("empty input", c.run([], stdin=""), "")
expect("last line without newline", c.run(["--min-level", "2"], stdin="## Last"), "- [Last](#last)\n")

lines, want = [], []
for i in range(20000):
    lines.append("## Item\n")
    want.append("- [Item](#item)\n" if i == 0 else f"- [Item](#item-{i})\n")
    lines.append("text\n```\n## code\n```\n")
expect("20000 duplicate headings", c.run(["--min-level", "2"], stdin="".join(lines), timeout=30), "".join(want))

r = c.run(["missing.md"])
c.case("missing file", r.code == 2 and r.out == "" and "error: cannot read missing.md" in r.err, r)

bad = []
for args in (["--min-level", "0"], ["--max-level", "7"], ["--min-level", "4", "--max-level", "2"],
             ["--min-level", "x"], ["--max-level"], ["--bogus"], ["doc.md", "doc.md"]):
    r = c.run(args, stdin="# a\n")
    if not (r.code == 64 and r.err.startswith("usage:") and r.out == ""):
        bad.append((args, r))
c.case("usage errors", not bad, bad)
c.finish()
