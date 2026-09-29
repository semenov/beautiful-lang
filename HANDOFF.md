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

## In flight

- **The gron port** (tomnomnom/gron, source in `/tmp/port-src/gron`). A
  background agent was finishing it in `/tmp/port-gron`.
  - Its gap log is `/tmp/port-gron/GAPS.md`, with repros in
    `/tmp/port-gron/repro/`.
  - If the port isn't finished, start an agent to finish it. It must learn
    the language only through `lang help`, `lang guide` and `lang doc`.
  - Then fix what it found. Already fixed from its log:
    - the list capacity bug;
    - O(n) `String.slice`;
    - slow `io.stdout()`;
    - Float text;
    - or-patterns;
    - list sort keys;
    - building a variant named `String`;
    - the guide example.
- When gron is finished, copy it into `ports/gron` without binaries, the
  way `ports/hey` and `ports/httpbin` were copied. Update the table in
  `ports/README.md`. `tests/run.sh` checks that ports compile.
- The go-httpbin port is done: see the TODO section "mccutchen/go-httpbin".
  Its code is in `/tmp/port-httpbin`, with `GAPS.md` there.

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
