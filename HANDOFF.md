# Handoff: where the work stands (2026-09-29)

For the next session. The full backlog is in `TODO.md`; this file is the
short version: what's in flight, what waits for Vlad, and what to do next.

## How we work

- Vlad sends ideas while the work goes on. Put each one in `TODO.md` and
  keep going. When he says "keep going", clear the backlog, then decide
  yourself what to improve next.
- Decide technical details yourself. Changes to the language model (new
  syntax, new semantics) must be raised with Vlad before building them.
- After each milestone:
  - run `compiler/tests/run.sh`;
  - run the Linux suite:
    `docker run --rm -v "$PWD":/src -w /src lang-linux sh -c 'cd compiler && CARGO_TARGET_DIR=target-linux cargo build --release && LANG_BIN=/src/compiler/target-linux/release/lang tests/run.sh'`;
  - run `python3 tools/stdlib_doc.py` when the stdlib changed;
  - commit and push.
- Every commit ends with the `Co-Authored-By` and `Claude-Session` lines
  (see `git log`).
- `lang fmt` must pass: the test suite checks it.

## Next, in this order

The three ports are done and live in `ports/` (hey, httpbin, gron):
sources, `GAPS.md` gap logs, `repro/` programs, and comparison scripts
against the Go originals. `tests/run.sh` keeps them compiling and runs
their tests. What they left open is in `TODO.md` under each port. The most
important:

1. **Stack overflow kills the program silently** (SIGBUS/SIGSEGV, exit 138
   or 139, no message). Programs that use http or spawn get a much smaller
   stack (task stacks). Give a clear "stack overflow" panic with a guard
   page and an alternate signal stack, and give tasks bigger stacks.
   Repro: `ports/gron/repro/stack_overflow_silent/`.
2. **Using http, spawn or net anywhere makes the whole program 1.7–2.4x
   slower.** It's probably the multi-threaded runtime (atomic refcounts
   everywhere?). Measure it, and pay the cost only where values are shared.
   Repro: `ports/gron/repro/http_slows_program/`.
3. **A top-level `let` holding a list, map or interpolated text is rebuilt
   on every use.** Build it once, as a static value.
4. **`json.parse` changes numbers:** `-0` becomes `0`, and `1e400` becomes
   Infinity and then null. Also: Unicode `is_letter` isn't the letter
   category, and there's no conversion between code points and characters.
5. The rest of the ports' open items in `TODO.md`.

## Waiting for Vlad's decision (language-model changes)

1. **A type's own text form.** `fn to_string(self) -> String` used by
   `"${x}"`. Today a `Duration` prints as `Duration(nanos: 1234000)`.
2. **JSON field names and omitting empty fields.** This needs some syntax
   for field attributes (like Go's struct tags). httpbin needed
   `user-agent`.
3. **Exact JSON numbers.** `json.Value.Number` is a Float, so `1.0`, `1e5`
   and 20-digit integers don't round-trip. gron wrote its own parser.
   Decide after reading gron's report.
4. Older proposals: `?.`, and channel `select`.

## Next, without a decision needed

- **Server timeouts** (slow or idle clients). First replace the deadline
  list with a timer wheel: the list sits behind a mutex and is O(n), so it
  can't be armed per request.
- **HTTP:** repeated headers, trailers, cookie `Expires`.
- **Performance:** a top-level `let` of a list, map or interpolated string
  is rebuilt at every use; cache it if it shows up in profiles. Also: the
  `words` benchmark is slower than Go, and strings aren't views.
- **The scheduler I/O path:** ideas from the fasthttp study are in
  `benchmarks/http/README.md`.
- **Packages:** mysql.
- **Reviews:** the stdlib against Go and Node, and the language's rough edges
  (TODO "Then" section).
- **Another port** of a different kind (for example a CLI with subcommands
  and SQLite, or a small web app with templates). Keep one newcomer agent
  per port.

## Useful to know

- Wire tests (a real server driven by curl) are in `compiler/tests/wire/`,
  and `tests/run.sh` runs them. The Docker image `lang-linux` has curl.
- The runtime's C headers are in `compiler/src/runtime/`:
  - `std.h` is always included;
  - `http.h` is included only when a server or the client is used;
  - so helpers that tests use without a server belong in `std.h`.
- Temporary test programs go in `/tmp/st`.
