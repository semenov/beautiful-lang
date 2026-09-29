# Handoff: where the work stands (2026-09-29, evening)

The project lives in ~/Dev/plumb (renamed from ~/Dev/beautiful-lang;
the GitHub repo is github.com/semenov/plumb). The language is Plumb:
command `plumb`, files `.plumb`.

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
    `docker run --rm -v "$PWD":/src -w /src lang-linux sh -c 'cd compiler && CARGO_TARGET_DIR=target-linux cargo build --release && PLUMB_BIN=/src/compiler/target-linux/release/plumb tests/run.sh'`;
  - run `python3 tools/stdlib_doc.py` when the stdlib changed;
  - commit and push.
- Every commit ends with the `Co-Authored-By` and `Claude-Session` lines
  (see `git log`).
- `plumb fmt` must pass: the test suite checks it.

## Next, in this order

State at the end of 2026-09-29 (all pushed unless noted):
- Done today: stack overflow panic; tasks slowdown (allocator, then the
  heap found inline from the thread register: 1.75x -> ~1.04x); top-level
  `let` built once; json numbers; Unicode; Vlad's four decisions
  (`to_string`, `json.encode_with`, `json.Number`, `?.`); gron's small
  items; benchmarks vs Go with memory (`ONLY=Go,Plumb python3
  benchmarks/run.py`); scheduler rewrite (per-worker queues, runnext, one
  wake per burst); realistic backend vs Go (benchmarks/backend: Plumb wins);
  HTTP server timeouts; cookie Expires; README scheduler section.
- **The last commit (inline heap lookup) passed the macOS suite but the
  Linux suite wasn't run yet: run it first** (the Linux path uses
  tpidr_el0/%fs:0 + a local-exec TLS offset).
- (Merged: ports/realworld; its gaps are in TODO under the ports.) Was: the RealWorld "Conduit"
  API (ports/realworld, with GAPS.md and newman API tests), by a newcomer
  agent. Its worktree: `.claude/worktrees/agent-a314d3b5bbeb9cea7`, branch `worktree-agent-a314d3b5bbeb9cea7` (`git worktree list`); review
  it, merge ports/realworld into main, and fix the gaps it lists (as with
  gron and httpbin).

1. Merge and work through the RealWorld port's GAPS.md.
2. **Where benchmarks still lose** (TODO "More benchmarks"): words (map
   layout / short strings), spawn (1.6x), memory in sort and maps; atomic
   reference counts (~5-10% in task programs).
3. **Question for Vlad:** is a String always UTF-8 (bad bytes replaced on
   the way in) or any bytes like Go? (`read_line` vs `Bytes.text()`.)
4. db: blocking SQLite calls block a worker thread; a pool; repeated
   request headers / trailers; the I/O path (poll from idle workers);
   mysql; reviews; another port.

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
- Profiling: `sample`/`vmmap` hang on this Mac (a permission prompt). Use
  perf on Linux: `docker run --rm --privileged -v "$PWD":/src -w /src
  lang-linux sh -c 'apk add perf; ... perf record -g ./prog; perf report'`.
