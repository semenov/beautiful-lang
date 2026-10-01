# Run 4: do agents make fewer mistakes in Plumb than in Go? (2026-10-01)

Setup in `README.md`; every bug classified in `FINDINGS.md`; raw log in
`results/events.jsonl`; the last table in `STATUS.md`.

25 tasks (CLI tools, text processing, SQLite, HTTP services, concurrency),
each with a spec, hidden black-box tests and a Python reference that
passes them. One fresh agent per task and language, same prompt. Every
build was logged and snapshotted, so the hidden tests ran on the **first
version that compiled** (before the agent tried its program) and on the
**final** one. Go's "compiler" here is `go build` + `go vet`.

Stopped early to save quota: Haiku didn't finish 22, 23, 24 (both
languages) and 25 (Plumb). The Haiku numbers below are over the 21 tasks
finished in both languages.

## Results

| | tasks | hidden tests, first compiled version | final version | all tests pass | builds (failed) | lines of code |
|---|---|---|---|---|---|---|
| Go, Sonnet 5.5 | 25 | 438/438 (100%) | 438/438 | 25/25 | 28 (0) | 7278 |
| Plumb, Sonnet 5.5 | 25 | 429/438 (98%) | 438/438 | 25/25 | 87 (45) | 6109 |
| Go, Haiku 4.5 | 21 | 279/383 (73%) | 330/383 (86%) | 4/21 | 108 (18) | 5879 |
| Plumb, Haiku 4.5 | 21 | 255/383 (67%) | 316/383 (83%) | 2/21 | 191 (105) | 4922 |

## What it shows

1. **No fewer bugs in Plumb.** With Sonnet both languages are perfect in
   the end; with Haiku Go is slightly ahead (73% vs 67% first, 86% vs 83%
   final). The difference is within the noise of one run per cell, but
   it doesn't favor Plumb.
2. **The bug classes Plumb removes are rare in agent code.** Of about 55
   classified bugs, the design would have prevented 3, plus 1 partly, all
   from Go with Haiku:
   - an ignored read error (`bufio.Scanner` stops at 64 KB, `Err()` never
     checked, big input silently gives `total 0`);
   - `w.WriteHeader` before `w.Header().Set`, so `Retry-After` was dropped;
   - a nil slice encoded as `"items": null`;
   - partly: JSON numbers as `float64` in `interface{}`, so integers near
     2^63 lose precision.
   No nil-pointer crash, data race in memory, goroutine leak, SQL
   injection or leaked file appeared in either language, even in the
   concurrency tasks.
3. **What does cause bugs, in both languages:** logic errors, misread
   specs, library defaults that don't fit the spec (Go's `flag`,
   `encoding/json`, `ServeMux`; Plumb's `cli.decode`, `csv.encode` +
   `print`), and races on shared **external** state (the database, the
   filesystem). The same check-then-act race on the database appeared in
   both languages in the same task. Plumb's "no shared mutable memory"
   doesn't reach there.
4. **Less code: Plumb is 16-17% shorter** for the same behavior (6109 vs
   7278 lines with Sonnet, 4922 vs 5879 with Haiku).
5. **More failed builds: 45 vs 0 (Sonnet), 105 vs 18 (Haiku).** All from
   not knowing the language (`catch` without `try`, `+` on text, assuming
   `args[0]` is the program name). The compiler catches these and the
   agent fixes them, so they cost time and tokens, not correctness.

## What to do in Plumb (from the run)

- **Bug:** `time.parse_date("2023-02-29")` succeeds and rolls into March.
  It broke one Haiku solution directly.
- **Check SQL at compile time.** SQL is always a literal, but `plumb
  check` accepts `selec * form t`; the README claims the compiler can
  check it. A Go agent wrote PostgreSQL `CREATE SEQUENCE` for SQLite: the
  one bug class here that Plumb could catch and Go can't.
- **Missing vs null in `json.decode<T>`** (the open RealWorld #4): a PATCH
  couldn't clear a field with `null`.
- **Per-field decode errors.** `decode<T>` turning `"name": 7` into one
  "invalid JSON" failed spec cases in both languages.
- **`cli.decode`:** settable exit code and `usage:` prefix, options after
  positional arguments, the FILE positional actually filled. Nearly every
  agent wrote its own argument parser instead.
- Smaller: a symlink test (`files.info` follows links); `files.walk`
  aborting on one unreadable directory; `csv.encode` + `print` doubling the
  newline; `process.interrupted()` not seen from a spawned task; regex
  limited to 9 groups; no Unicode digit/mark tests; say whether
  `req.query(...)` is already decoded.

## Limits

- One run per task and language: small differences are noise.
- Specs are exact and tests are black-box. They don't see a race unless
  it shows, a leak, or a weak design (one Go agent hashed passwords with
  1000 rounds of SHA-256; nothing tested that).
- How easy the code is to review isn't measured, only its length.
- Plumb writers could read the whole repository (guide, stdlib reference,
  examples); Go writers had their training. Plumb's failed builds are the
  cost of that gap.
- Cost: about 120 agents. A rerun should start smaller (Haiku only,
  fewer tasks) and grow if the signal is there.
