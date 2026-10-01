# markdown-toc

A command-line tool that prints a table of contents for a Markdown
document, with links to GitHub-style heading anchors.

## Usage

```
app [--min-level N] [--max-level M] [FILE]
```

Reads FILE, or standard input when FILE is not given. The input is UTF-8;
lines end with `\n` or `\r\n` (the `\r` is not part of the line).
`--min-level` (default 1) and `--max-level` (default 6) select which
heading levels are listed; they must satisfy 1 ≤ N ≤ M ≤ 6.

## Headings

Only ATX headings count (underlined "setext" headings are ignored). A line
is a heading of level L when it consists of:

1. 0 to 3 spaces;
2. exactly L `#` characters, 1 ≤ L ≤ 6 (so `####### x` is not a heading);
3. then either the end of the line, or at least one space or tab followed
   by the heading text (so `#hashtag` is not a heading).

The heading **text** is the rest of the line with leading and trailing
spaces and tabs removed, and then the run of `#` characters at its end
(if any) removed, provided that run is the whole text or is preceded by a
space or tab (after which trailing spaces and tabs are removed again):
`## Title ##` → `Title`, `# C#` → `C#`, `### ###` → empty. Headings with
empty text are not listed and get no anchor.

**Fenced code blocks.** A line of 0 to 3 spaces followed by at least three
backticks or at least three tildes opens a fence. The fence is closed by
the next line made of 0 to 3 spaces, at least as many of the same
character as the opening run, and then only spaces or tabs. Nothing inside
a fence (including the opening and closing lines) is a heading. A fence
that is never closed runs to the end of the input.

## Anchors (slugs)

The slug of a heading is computed from its text:

1. Convert to lowercase (Unicode simple lowercase mapping).
2. Remove every character that is not a Unicode letter (categories L*),
   a combining mark (M*), a decimal digit (Nd), a space, `-` or `_`.
3. Replace each space with `-` (two spaces give two hyphens).

Every listed-or-not heading with non-empty text gets a slug, in document
order, also those outside the selected levels. If the same slug already
occurred earlier, append `-1` for its second occurrence, `-2` for its
third, and so on (counted per original slug).

## Output

One line per heading of a selected level, in document order:
2 × (L − N) spaces, then `- [TEXT](#SLUG)`, where TEXT is the heading text
as defined above. No headings: no output. Exit code 0.

## Errors

- FILE can't be read: `error: cannot read FILE` to standard error, exit
  code 2.
- Unknown option, missing or invalid level value, N > M, or more than one
  FILE: a message starting with `usage:` to standard error, exit code 64.

## Example

```
$ app --max-level 3 doc.md          # doc.md shown below
- [Größe & Maße](#größe--maße)
  - [Setup](#setup)
  - [Setup](#setup-1)
    - [Step 1.2: run `make`](#step-12-run-make)
```

doc.md: `# Größe & Maße`, `## Setup`, `#### Deep`, `## Setup ##`, a fenced
block containing `# not a heading`, then `### Step 1.2: run \`make\``.
