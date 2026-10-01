# Findings while running

Things the writers' reports and the checks turned up, as they come in.

## Plumb standard library gaps (from the writers' reports)

- 08-dup-finder: `files.walk` fails the whole walk on one unreadable
  directory and lists symlinks/FIFOs with regular files; there is no
  streaming SHA-256. The agent shelled out to `find` and `sha256sum`.
- 01, 02, 03, 04, 09, 14: every agent parsed arguments by hand because
  `cli.decode` uses its own exit codes and doesn't allow options after the
  positional arguments.
- 13-markdown-toc: no Unicode category tests beyond letters (`is_digit` is
  ASCII-only, regex has no `\p{}`); the agent hand-wrote tables for M* and Nd.
- 12-batch-rename: no way to tell a symlink from a file (the agent ran
  `find -type f`); regex supports at most 9 groups.
- 02-csv-report: the `csv` module doesn't report line numbers or bad
  records, so the agent wrote its own parser.
- 04-json-query: `json.parse` loses key order and the agent wrote its own
  JSON reader (numbers as written, key order).

## Ideas for Plumb from the run

- **Bug:** `time.parse_date("2025-02-30")` succeeds and the date prints as
  `2025-02-30`. Found by 09-todo-sqlite (Plumb, Haiku), which had to write
  its own calendar check.

- SQL is always a literal in the call, but the compiler doesn't check it:
  `plumb check` accepts `conn.execute("selec * form t", [])`. Checking the
  syntax at compile time (SQLite's parser on the literal) would catch a
  bug class Go can't (09-todo-sqlite, Go with Haiku). README says "the
  compiler can check it", which today isn't true.
- `cli.decode`: a way to set the exit code and message prefix for usage
  errors, and options after positional arguments.
- `process.interrupted()` polled from a spawned task never turns true on
  SIGTERM (22-job-queue, Sonnet); only `main` is cancelled. Check whether
  that's intended and document it.
- Missing vs null in `json.decode<T>` (HANDOFF's open RealWorld #4): a
  Haiku agent's PATCH couldn't clear a field with `null`. Evidence for
  deciding it.
- Is `req.query(...)` already decoded? An agent decoded it again and
  `%` became a 500. The doc should say, or `url.decode` of a decoded
  value is a common slip.
- `csv.encode` + `print` gives a doubled final newline.

## Method

- Ceiling effect: with exact specs, Sonnet 5.5 passes every hidden test in
  both languages, even with the first version that compiled. A weaker
  model (Haiku 4.5) is added as a second pass to see whether the compiler
  catches more of its mistakes.

## Bugs that got past the compiler (classified)

| task | writer | version | bug | class | would the other language's compiler catch it? |
|---|---|---|---|---|---|
| 01-wordfreq | Go (Haiku) | first and final | `bufio.Scanner` stops at a 64 KB line; `scanner.Err()` never checked, so big input silently prints `total 0` | ignored error | yes: in Plumb a failing read is `throws` and needs `try` |
| 01-wordfreq | Go (Haiku) | first and final | Go's `flag` package prints its own line before `usage:` | library default vs spec | no |
| 10-business-days | Plumb (Sonnet) | first | `total += 1` instead of `-= 1` for holidays | logic | no (fixed by the agent's own testing) |
| 03-log-stats | Go (Haiku) | first | error rate printed with float `%.2f`: 0.125% becomes 0.12% | float rounding | no |
| 03-log-stats | Go (Haiku) | first | `flag` package: exit 2 and Go's own help text instead of `usage:` and 64 | library default vs spec | no |
| 03-log-stats | Go (Haiku) | first and final | a quote inside the target accepted (`/o"k`) instead of counted as malformed | parsing strictness | no |
| 01-wordfreq | Plumb (Haiku) | first | a run of only apostrophes counted as a word | logic | no (fixed by the agent's own testing) |
| 02-csv-report | Go (Haiku) | first; 2 left in final | used `encoding/csv`: its line numbers, quote rules and messages differ from the spec; empty input not detected | library default vs spec | no |
| 02-csv-report | Go (Haiku) | first | `flag` package: exit 2 instead of 64 | library default vs spec | no |
| 02-csv-report | Plumb (Haiku) | first and final | `print(csv.encode(rows))`: `csv.encode` already ends with a newline, `print` adds another, so every output has an extra empty line (15 of 23 cases) | library ergonomics | no; worth a lint or a `csv.write` |
| 02-csv-report | Plumb (Haiku) | first and final | line numbers ignore line breaks inside quotes; a closing quote followed by text treated as fatal | parsing / spec | no |
| 03-log-stats | Plumb (Haiku) | first and final | `cli.decode`: exit 1 and its own messages instead of `usage:` and 64; first version also refused FILE arguments | library default vs spec (same as Go's `flag`) | no |
| 03-log-stats | Plumb (Haiku) | first | line parser rejected every valid line (all counted malformed) | parsing / logic | no (fixed by the agent's own testing, 21 builds) |
| 03-log-stats | Plumb (Haiku) | final | some malformed lines accepted | parsing strictness | no |
| 05-config-merge | Go (Haiku) | first and final | output lacks the final newline (most cases); top-level array reported as "invalid JSON" | output format / spec | no |
| 04-json-query | Go (Haiku) | first and final | `encoding/json` into `map[string]interface{}`: key order lost, numbers re-printed, escapes differ; `flag` messages; `null` vs missing confused | library default vs spec | no |
| 06-line-diff | Go (Haiku) | first and final | `-U 0` hunk starts wrong; file read as Latin-1-ish bytes (`hÃ©llo`); `flag` messages (fixed) | logic / encoding | no |
| 09-todo-sqlite | Go (Haiku) | first | `CREATE SEQUENCE` (PostgreSQL syntax) in SQLite: every command fails at run time with a syntax error | SQL text unchecked | **not today**: Plumb doesn't check SQL either (`plumb check` accepts `selec * form t`), though the SQL is a literal and could be checked |
| 05-config-merge | Plumb (Haiku) | first and final | reports only the first invalid key instead of all; empty-key env names applied (fixed) | logic / spec | no |
| 04-json-query | Go (Haiku) | final | hand-rolled re-printer keeps input spacing, drops `--select` nested values | logic | no |
| 04-json-query | Plumb (Haiku) | first and final | `{"a":01}` (leading zero) accepted as valid JSON | parsing strictness | no |
| 07-money-split | Plumb (Haiku) | first | balance lines printed without the name; invalid amounts (`1.234`, `.5`) accepted (partly fixed) | output / validation | no |
| 08-dup-finder | Plumb (Haiku) | first and final | one unreadable entry makes the whole run fail as "not a directory" (exit 2) | error handling + stdlib (`files.walk` aborts on the first unreadable directory) | no |
| 08-dup-finder | Plumb (Haiku) | first | symlinks followed; `cli.decode` exit codes (fixed) | stdlib gap (no symlink test) / library default | no |
| 09-todo-sqlite | Plumb (Haiku) | first and final | ids reused after `rm` (no AUTOINCREMENT); overdue filter and invalid dates (fixed) | logic / SQL | no |
| 06-line-diff | Plumb (Haiku) | first | assumed `args[0]` is the program name (Go's `os.Args` habit); every run was a usage error (fixed) | API habit from other languages | no |
| 08-dup-finder | Go (Haiku) | first | two DIR arguments accepted (fixed) | argument checking | no |
| 10-business-days | Go (Haiku) | first and final | extra positional argument accepted | argument checking | no |
| 11-parallel-fetch | Go (Haiku) | first; redirects left in final | `flag.Duration`-style parsing rejected `0.5`; FILE argument refused; 10th redirect counted wrong; dropped connection classed as `connect` | library default / logic | no |
| 11-parallel-fetch | Plumb (Haiku) | first; usage codes left in final | `cli.decode` never filled the FILE field, so the program read stdin; `cli.decode` exit codes | library ergonomics | no |
| 12-batch-rename | Go (Haiku) | first | options after DIR not parsed (`flag` stops at the first positional): every run was a usage error | library default vs spec | no |
| 13-markdown-toc | Plumb (Haiku) | first | option value taken as FILE; fences and slugs wrong (partly fixed) | argument parsing / logic | no |
| 14-notes-api | Go (Haiku) | first and final | search done with SQL `LIKE` and escaping that doesn't fully work; Go's default 404 page | SQL / library default | no |
| 15-auth-sessions | Go (Haiku) | final | Go's `404 page not found` text before the JSON body; validation messages | library default | no; also hashed passwords with 1000 rounds of SHA-256 (not tested; Plumb has `crypto.hash_password`) |
| 22-job-queue | Plumb (Sonnet) | first | SIGTERM didn't stop the server; the agent found that a task polling `process.interrupted()` never sees the signal and moved shutdown into `main` (fixed) | runtime API surprise | no |
| 10-business-days | Plumb (Haiku) | first and final | trusted `time.parse_date`, which accepts `2023-02-29` (rolls to 03-01/03-02): invalid dates and bad holiday lines accepted | **Plumb stdlib bug** | n/a; Go's `time.Parse` rejects these |
| 12-batch-rename | Plumb (Haiku) | first and final | renamed a symlink (no way to tell links from files in the stdlib); `--match` option missing; usage errors exit 1 | stdlib gap / spec | no |
| 13-markdown-toc | Go (Haiku) | first; fences/slugs left in final | fences, slug rules, duplicate numbering; `flag` messages | logic / library default | no |
| 14-notes-api | Plumb (Haiku) | first and final | PATCH: `json.decode<UpdateNoteRequest>` can't tell a missing `tag` from `"tag": null`, so null never clears it; query decoded twice (`url.decode` on an already-decoded value), so `?q=%25` is a 500 | **language gap** (missing vs null in decode, the open RealWorld #4 question) / API ambiguity | no |
| 19-reservations | Go (Haiku) | first and final | decoding into a typed struct turns a wrong field type (`"name": 7`) into "invalid JSON" instead of "invalid name" | typed decode vs per-field errors (Plumb's `decode<T>` has the same shape; 15-auth-sessions failed the same way in both languages) | no |
| 20-rate-limiter | Go (Haiku) | first | `w.WriteHeader(429)` before `w.Header().Set("Retry-After", ...)`: headers set after `WriteHeader` are silently dropped, so every denial lacked `Retry-After` (fixed) | Go API ordering trap (mutable response writer) | **yes, by design**: in Plumb a response is a value (`http.json(...)` with headers), there's no order to get wrong |
| 17-csv-import-api | Go (Haiku) | first and final | check-then-act race: the "email already stored?" check runs outside the mutex, the insert inside it, so parallel imports of one email give 500s (UNIQUE violation) instead of one 201 and 422s; a malformed CSV accepted | concurrency logic (TOCTOU against the database) | no: Plumb's "no shared mutable state" doesn't cover a database either |
| 20-rate-limiter | Plumb (Haiku) | first | `try json.decode<...>` in a handler without `catch`: bad bodies propagate and become 500 instead of 400 (fixed) | error handling (visible `try`, wrong default) | no; the `try` was visible, a reviewer would see it |
| 16-paginated-catalog | Plumb (Haiku) | first and final | 405 vs 404 for some paths | routing / spec | no |
| 16-paginated-catalog | Go (Haiku) | first | a nil slice encodes as `"items": null` instead of `[]` when nothing matches (fixed) | nil (Go's nil slice in JSON) | **yes, by design**: Plumb has no nil; an empty list is `[]` |
| 18-file-store | Plumb (Haiku) | first and final | temp file named `.tmp_<name>` (not unique): concurrent PUTs to one name rename each other's temp file away, 500s | concurrency logic on a shared external resource (filesystem) | no: tasks share the filesystem, not memory |
| 18-file-store | Go (Haiku) | first and final | `http.ServeMux` cleans `/files/..` and answers 307 instead of 400 | library default (router path cleaning) | no |
| 17-csv-import-api | Plumb (Haiku) | first and final | the same check-then-act race as Go (Haiku): parallel imports of one email give 500s; a malformed CSV accepted | concurrency logic (TOCTOU against the database) | no, same as Go |
| 19-reservations | Plumb (Haiku) | first and final | `json.decode<T>` failure reported as "invalid JSON" instead of per-field errors (same as Go); idempotency key not found after restart; router's plain-text 404/405 bodies | typed decode / persistence / library default | no |
| 25-metrics-window | Go (Haiku) | first and final | some invalid samples accepted instead of rejecting the whole request | validation | no |
| 21-kv-counter | Go (Haiku) | first and final | values kept as `interface{}` with JSON numbers as `float64`: integers near 2^63 lose precision, so overflow checks and `by` parsing are wrong; `cas` with `expected: null`; snapshot | untyped JSON (`float64` for all numbers) | **partly, by design**: Plumb's `json.Value.Number` keeps the exact text and `to_int()` checks range |

## Harness notes

- 20-rate-limiter/plumb once failed one case with "connection refused"
  while ~16 agents were running; two reruns passed. To watch: if it
  repeats, it may be the server's accept backlog under a burst of 64
  parallel connections.
- Two failures (20-rate-limiter/plumb "connection refused", 17-csv-import-api/go
  "server exited with -15") were the harness, not the programs: writer
  agents testing their own servers kill processes named `app`, which hit
  the check's server. Checks now run a renamed copy; both were re-run and
  passed (5/5 and 3/3 reruns passed before that too).
- The 20-25 author changed its checks to send `Connection: close` (client
  port exhaustion on macOS after ~7000 connections). 20 and 21 were
  re-checked with the final check files for both languages.
