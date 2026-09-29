# Handoff: where the work stands (2026-09-29, evening)

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

Done on 2026-09-29: stack overflow panic, the tasks slowdown (allocator),
top-level `let` built once, json numbers, Unicode, all four of Vlad's
decisions (`to_string`, `json.encode_with`, `json.Number`, `?.`), the small
gron items, benchmarks against Go with memory (`benchmarks/`, `ONLY=Go,Lang
python3 run.py`), the scheduler rewrite (per-worker queues, runnext), the
README scheduler section.

1. **A realistic backend vs Go** (Vlad): a service shaped like a real app
   (JSON API, routing, middleware, auth header, validation, SQLite, logging)
   in both languages; load it, compare req/s, latency, CPU per request,
   memory; fix where we lose.
2. **Where the benchmarks still lose** (TODO "More benchmarks"): words
   (strings are heap objects per piece: needs a text representation change),
   spawn (3x Go), memory in sort / json / maps.
3. **Question for Vlad:** is a String always UTF-8 (bad bytes replaced on
   the way in) or any bytes like Go? (`read_line` vs `Bytes.text()`.)
4. Server timeouts (timer wheel first), HTTP repeated headers / trailers /
   cookie Expires, the I/O path (poll from idle workers), mysql, reviews,
   another port.

## Decided by Vlad (2026-09-29), built

1. **A type's own text form: yes.** `fn to_string(self) -> String` on a type
   is used by `"${x}"`; stdlib types (Duration, errors, Date) get readable text.
2. **JSON names: options on the call, no field attributes.**
   `json.encode(x, keys: "kebab", omit_empty: true)`; attributes only if a
   port needs different names for fields of one object.
3. **Exact JSON numbers: yes.** `json.Value.Number` holds the number as
   written; `n.to_int()`, `n.to_float()`, `n.to_decimal()`; `"${n}"` prints
   it as written.
4. **`?.`: yes** (`user?.address?.city`). Channel `select`: later, when a
   program needs it.

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
