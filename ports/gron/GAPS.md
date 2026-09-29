# Gaps found while porting gron to lang

Running log. Each entry: what I wanted, what I wrote instead, severity,
repro. Severity: **major** (a workaround of 50+ lines, a crash, or a
visible difference from Go), **minor** (a few lines or a detour).

## Status (2026-09-29)

Everything gron does is ported: file / stdin / URL input, `--ungron`,
`--stream`, `--values`, `--no-sort`, `--json`, colors on a terminal
(`--colorize`, `--monochrome`, `NO_COLOR`, `TERM=dumb`), `--version`, Go's
`flag` parsing, exit codes and error wording.

Parity with the Go binary (`go build` in /tmp/port-src/gron):

- `scripts/compare.sh`: Go's testdata plus mine, every mode: 280/280 same
  output and exit code.
- `scripts/fuzz.py` + `scripts/fuzz_compare.sh`: 300 random documents and
  300 random streams (odd keys, all escapes, U+2028, surrogates, big and
  odd numbers, 1e400, damaged gron text): 3595/3600 same. The 5 others are
  damaged lines with bytes that aren't UTF-8 (see "Known differences").
- 53 MB file: gron, `--json`, `-c`, `-s`, `-v` byte-identical to Go;
  `--ungron` identical on 128k and 510k lines.
- Deep JSON: identical up to Go's limit of 10000 levels, and the same
  "exceeded max depth" error beyond it.
- Colors on a real terminal (checked with `script`): identical.
- URLs (`scripts/url_compare.sh`): same, except the known differences below.

Tests: 29 `test` blocks over 8 files, all passing (`lang test <file>.lang`).

## Resolved by the compiler update

- **`\u{...}` escapes**: the ANSI colors are plain literals
  (`"\u{1b}[${on}m"`), the sort separator is `"\u{1}"`, tests write
  control characters directly. Gone: the `Palette` record threaded through
  every print function, `Bytes([27]).text()`, `Bytes([1]).text()`.
- **Top-level `let`**: exit codes, the version, the usage text,
  `max_line`, `rune_error`, `max_depth`, and the colors (`pub let
  str_color = Color(on: "33", off: "0")`, like Go's
  `color.New(color.FgYellow)`) are fixed values instead of zero-argument
  functions.
- **Hex literals**: UTF-8 and Unicode constants read as what they are
  (`0xE2 0x80 0xA8`, `0xD800`, `0x10FFFF`) instead of 226/128/168/55296;
  the generated Unicode tables use them too.
- **Calling a function kept in a field** (`input.read_line()`): no more
  `let f = input.read_line` then `f()`. It also let JSON writing become one
  function with a `Style` record of functions (pretty for `--ungron -m`,
  colored for `--ungron`). repro/call_fn_field.lang now runs.
- **`-> Never`**: `fatal()` and `bad_flag()` end a branch, so
  `_ => bad_flag("invalid boolean value ...")` works as a `match` arm.
- **`eprint`**: error messages and the usage text (which has no final line
  break, so `eprint` adds exactly Go's). The `io_eprint` / `write_stderr`
  helpers around `with io.stderr()` are gone.
- **`files.info` / `files.is_dir`**: a directory as input now behaves as in
  Go (Go opens it and fails on read: `failed to form statements: read
  /tmp: is a directory`, exit 3; lang's `files.open` refuses it, so the
  port checks `files.is_dir` first).
- **Appending to a shared list no longer doubles its capacity**: the OOM
  on deep JSON is gone. `extended()` (a manual copy to dodge the bug) is
  replaced by `tokens.concat(...)`. repro/list_copy_append_oom.lang and
  repro/recursive_accumulator_oom.lang now run.
- **`Bytes[i]`** works now (not in the list, and not in `lang doc Bytes`):
  every `b.int_at(i, size: 1)` became `b[i]`, same speed.
- The "a `Stream` must be closed" help no longer suggests `try` for
  `io.stdout()`.
- `lang guide errors` shows the error record on several lines (the main
  `lang guide` still doesn't, see Docs).

## Open gaps

### Language

- **No comparator sort; `sorted_by` takes only Int/Float/String keys**
  (major). gron sorts with a custom `Less` (token by token, indexes as
  numbers, `=` first). Workaround: one sort-key string per statement
  (indexes zero-padded to 20, a U+0001 separator, `=` as empty text).
  repro/sort_key_list.lang
- **No code point <-> character conversion** (major for a lexer). Go's
  lexer walks runes; the port decodes and encodes UTF-8 by hand
  (`jsonraw.decode_rune`, `append_rune`, ~70 lines).
- **Enum variants named like built-in types (`String`, `Bool`) can't be
  built bare** (minor): `String(value: x)` fails; `Value.String(value: x)`
  works (bare names work in patterns). repro/variant_named_string.lang
- **No or-patterns in `match`** (minor): `"a" | "b" => true`. Workaround:
  `list.contains`, or one arm per value (12 arms for Go's boolean
  spellings in the flag parser). repro/or_patterns.lang
- **No `+` on strings** (minor, by design; the error suggests both
  alternatives).
- **No tuples** (minor, by design): `Rune { value, width }` for one helper.
- **No one-line records or enums** (minor): `type K { a: String  b: Int }`
  and `enum K { A  B }` don't parse. repro/one_line_record.lang

### Standard library

- **`json.Value.Number` is a `Float`** (major for gron, whose point is to
  print numbers as written: `1.0`, `1e5`, `12345678901234567890`). No raw
  number / `UseNumber` option. Workaround: my own JSON reader and writer,
  `jsonraw.lang` (~700 lines). repro/json_number_precision.lang
- **`json.parse` silently changes numbers** (major, data loss): `-0` ->
  `0`, `1e400` -> Infinity, which `encode` then writes as `null`.
  repro/json_parse_numbers.lang
- **No Unicode categories, and `String.is_letter` isn't the letter
  category** (major for identifier rules): it is true for U+0661 (a digit)
  and U+0301 (a mark), false for U+216B (a letter number). Workaround:
  `unicode.lang`, range tables generated from Go's `unicode` package by
  scripts/genunicode (275 lines). repro/is_letter_categories.lang
- **HTTP client: no proxy and no "insecure" option** (major for gron's
  `-x`, `--noproxy`, `-k`). The port fails clearly (`can't use proxy ...:
  lang's HTTP client has no proxy support`; `-k` on https is refused).
- **`http.ResponseStream` isn't an `io.Stream`** (minor): no shared reader
  interface, so "a file, stdin or a URL" is passed around as a record of
  two closures (`Input` in main.lang). repro/response_stream_not_stream.lang
- **File errors are text only** (minor): no NotFound / PermissionDenied
  kinds. Go's `open x: permission denied` is rebuilt from `files.exists`
  and a match on `": Permission denied"`. repro/file_error_kinds.lang
- **`read_line` can give a String that isn't UTF-8, but `Bytes.text()`
  refuses to make one** (minor): a line can't be cut up by byte offsets
  without replacing bad bytes with U+FFFD. repro/read_line_invalid_utf8.lang
- **No print without a line break except through a `with` stream**
  (minor), and the `with` makes the function `throws` (closing can fail).
  Not needed in gron any more (`eprint` fits). repro/std_streams_with.lang
- **`Duration` interpolates as `Duration(nanos: 1234000)`** (minor).
  repro/duration_interp.lang
- **Float formatting differs from Go/JS** (minor): `1.2345678901234567e+19`
  vs `12345678901234567000`. `statements.go_float` (50 lines) rebuilds
  Go's layout for `--ungron --json`. repro/json_number_precision.lang

### Compiler / runtime bugs

- *Fixed:* a stack overflow is now a panic ("stack overflow: too many
  nested calls"), and a task's stack is 8 MB like the main thread's.
  Was: **A stack overflow is a silent crash** (major): SIGBUS/SIGSEGV, exit
  138/139, no message. **And a program that uses http gets a much smaller
  stack**: without http, 100000 simple frames are fine; with an http call
  anywhere, even one never run, it dies below that. gron's recursive JSON
  reader died at ~2500 levels (Go allows 10000). Workaround: every walk
  over JSON is a loop with its own stack (parser, statement filler, merge,
  ungron, JSON writer, number check), ~120 lines more than recursion.
  repro/stack_overflow_silent/
- **`files.read` / `files.read_bytes` of a directory give empty text, not
  an error** (minor; `files.open` of it fails). repro/file_error_kinds.lang

### Performance

- *Mostly fixed:* the allocator was the cause (system malloc with tasks);
  now per-thread free lists: the repro went from 1.75x to 1.16x. Was:
  **Using http (or `spawn`, or `net`) anywhere slows the whole program
  1.7-2.4x** (major). The same gron with the URL branch removed: 53 MB in
  3.3 s instead of 5.9 s, `-v` in 5.1 s instead of 11.6 s. Presumably
  thread-safe reference counting or allocation for the whole program. No
  workaround short of dropping URL input. repro/http_slows_program/
- **A top-level `let` list is rebuilt on every use** (major for lookup
  tables): one read from a 2000-element table costs ~230 ns. The Unicode
  lookups made the lexer 5x slower; workaround: an ASCII fast path before
  the tables. repro/top_level_let_list_slow.lang
- **`String.slice` is O(n) per call** (major): 10 000 slices of a 2 MB
  string take ~4 s. All scanning is done on `Bytes`. repro/string_slice_slow.lang
- **Writing to `io.stdout()` is ~15x slower than `print`** (minor): 1M
  lines 800 ms vs 54 ms. The port uses `print`. repro/stdout_write_slow.lang
- **Reading `m[k]` copies the value** (minor, undocumented): a
  read-modify-write of a big map value is quadratic; with `m.take(k)` it
  isn't. repro/map_read_copies.lang

### Error messages

- **A cascade after a rejected call**: after `sorted_by(r => r)` fails, the
  next line gets "expected `String`, found `Int`". repro/sort_key_list.lang
- **`catch` in the wrong place inside a lambda**: `xs.map(x => h(try
  g(x)) catch err {...})` says "expected `)` to close the call, found
  `catch`"; outside a lambda the same mistake gets the good "`catch` needs
  `try` before the call it handles". repro/try_inside_call_catch.lang
- **A variant named `String` can't be built**, and "`String` can't be
  built this way" doesn't suggest `Value.String(...)`.
  repro/variant_named_string.lang
- **A project file named like a standard module** (`url.lang`): `import
  url` says "the main file can't be imported" instead of "this file
  shadows the standard `url` module". repro/module_shadow/

### Docs

- `lang guide` (section Errors) still shows `type NotFound implements
  Error { id: Int  fn message(self) ... }` on one line, which doesn't
  parse. repro/one_line_record.lang
- `lang doc Bytes` doesn't mention `b[i]`.
- `String.is_letter`'s "a letter, of any script" isn't what it does.
- Nothing says that reading `m[k]` copies (or that `take` doesn't), that
  a top-level `let` is rebuilt on each use, or that http makes the stack
  smaller and the program slower.
- `lang guide` doesn't list which patterns `match` has.

## Known differences from Go

- Bytes that aren't UTF-8 in `--ungron` input: Go quotes them raw in
  "ungron failed for ..."; the port shows U+FFFD (see `read_line` above).
- URL errors are worded by lang's client (`Failed to connect to localhost
  port 1 ...`) instead of Go's (`dial tcp [::1]:1: connect: connection
  refused`); exit code 4 in both.
- `-x` / `--noproxy` / `http_proxy` and `-k` on https fail (see above).
- Called as `ungron` (Go checks `os.Args[0]`): `process.args()` has no
  program name, so this isn't ported.
- `--no-sort` order: Go's follows Go's random map order, the port's the
  input order. Compared as sets of lines.

## Benchmarks

Apple M3 Max; best of 3, `/usr/bin/time -l`, output to /dev/null
(scripts/bench.sh, data from scripts/make_bench_data.py). "no http" is
the port built without the URL branch (see Performance). Go's gron is slow
on output because it makes a system call per line.

| case | Go | lang | lang, no http |
|---|---:|---:|---:|
| gron, 53 MB file | 7.1 s, 2671 MB | 5.9 s, 2221 MB | 3.3 s, 2095 MB |
| `--no-sort` | 5.1 s, 2604 MB | 4.5 s, 1748 MB | 2.3 s, 1642 MB |
| `--json` | 7.8 s, 2764 MB | 6.9 s, 2243 MB | 3.7 s, 2096 MB |
| `-c` (colors) | 13.9 s, 2657 MB | 7.0 s, 2258 MB | 3.9 s, 2095 MB |
| `-s`, 130k lines (54 MB) | 5.3 s, 18 MB | 5.4 s, 7 MB | 2.7 s, 2 MB |
| `-v`, 3.3M lines | 4.8 s, 18 MB | 11.6 s, 6 MB | 5.1 s, 1 MB |
| `--ungron`, 128k lines | 1.3 s, 9640 MB | 2.1 s, 132 MB | 2.0 s, 116 MB |
| `--ungron`, 510k lines | 88 s, 12.7 GB | 27 s, 528 MB | |

`--ungron` is quadratic in both (each `json.users[i]...` line makes an
array of i+1 elements to merge, as in Go); Go's footprint on 510k lines
peaked at 156 GB, so it was swapping.
