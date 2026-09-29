# Notes on writing these tasks against CHEATSHEET-indent.md

Overall the cheat sheet is dense but clear for common cases. Most uncertainty
came from combining two documented features in a way the sheet never shows
together, or from a handful of small internal inconsistencies. Listed by
where they bit hardest.

## The single biggest ambiguity: the "name every argument after the first" rule

"With 3 or more, name every argument after the first" is stated as a general
rule, and "the receiver of `x.f(...)` counts as the first" is given with
`text.replace(old: "-", new: "_")` as the example (receiver + 2 args = 3,
both named). But several of the cheat sheet's own stdlib examples have the
same arity (receiver + 2 = 3) and are shown fully positional:
`conn.query<User>("select * from users", [])`, `conn.execute(sql, params)`,
and `router.get("/notes/:id", get_note)`. I couldn't reconcile this, so I
made a judgment call: copy stdlib call sites exactly as shown (positional),
and apply the literal rule to my own functions. This shows up as:
- Task 1: `restock(&products, name: "Gizmo", amount: 10)` (3 params).
- Task 3: `transfer(&accounts, from: ..., to: ..., amount: ...)` (4 params).
- Task 9: `copy_with_prefix(memory, destination: &disk, prefix: "user:")`
  (3 params) — this also guesses that a named argument and a `&`-reference
  compose as `name: &value`; the only `&` example in the sheet
  (`add_tag(&tags, "urgent")`) has just 2 positional params, so the
  combination itself is untested by the sheet.
- Task 5: I sidestepped the problem for `create_note`/`get_note` by bundling
  the two pieces of shared state into one `NotesStore` record so each
  handler stays at 2 params and can be called positionally — a workaround
  rather than a resolution.

## Optional-narrowing and `??` are more limited than they first look

- The sheet shows `if x is some(v)` introducing a binding, but never shows
  whether a guard like `if x is none: throw ...` narrows `x` from `T?` to
  `T` for the rest of the block (i.e. no documented flow-typing after a
  negative check). To avoid relying on this, every place I needed to unwrap
  an `Optional` after checking it, I used the fully-documented
  `match ... some(v) / none` form instead, even when it's more verbose.
  This affects Task 2 (header check), Task 3 (`require_account` helper —
  originally wanted `accounts[id] ?? throw AccountNotFound(...)`), Task 5,
  Task 6, and Task 9.
- Relatedly, I never found out whether `??`'s fallback side can be a
  control-flow expression like `throw ...` (only plain values, e.g. the
  `8080` example, are shown). I assumed no and wrote a small
  `require_account` helper in Task 3 instead of a one-line `??`/`throw`.

## Things the sheet simply doesn't name

- **No name for the value `with conn = try db.open(url)` binds.** Every
  example just uses `conn` inline inside the `with` block. Because I don't
  know the type to write in a function signature, Task 7 could not factor
  "create table + seed + query" into a shared helper called from both
  `main()` and the test — the logic is duplicated instead.
- **No documented escape sequence for `"` inside a normal `"..."` string**,
  and multi-line `"""..."""` strings' quoting rules aren't spelled out
  either. Task 5 needs a JSON string body containing double quotes
  (`{"title": "Groceries", ...}`); I used a `"""..."""` string for it on the
  assumption triple-quoted strings can contain bare `"` safely, since I had
  no confirmed escape syntax to fall back on. More generally, I avoided
  ever nesting a quoted literal inside `${}` interpolation (e.g. never wrote
  `"${x ?? "fallback"}"`) — I precompute such fallbacks into a `let` first
  (see Task 6's `display_name`/`bio_line`).

## Enums vs. records vs. interfaces

- `implements` is only ever shown on `type`, never on `enum`. I couldn't
  tell whether an enum can implement an interface, so Task 3's three
  distinct error kinds (`AccountNotFound`, `InsufficientFunds`,
  `InvalidAmount`) are three separate `type ... implements Error` records
  rather than one enum, even though an enum would have been more compact
  and is arguably what "a distinct kind" suggests.
- Whether an `interface` method can declare a `var self` receiver (for a
  mutating method) isn't shown — only plain-type methods use `var self` in
  the examples. Task 9's `Storage` interface declares `put`/`delete` with
  `var self`, assumed by extension from the type-method rule.
- Task 9, awkwardness: `Storage`'s `get`/`put`/`delete` must all declare
  `throws` because `FileStorage` genuinely needs it (file I/O), which forces
  `MemoryStorage`'s implementations to also be `throws` (and every call site
  to use `try`) even though the in-memory version can never actually fail.
  Mildly awkward to write and read, but seems to be a direct, unavoidable
  consequence of one interface serving both implementations.
- Whether `is Type(_)` pattern-matching works uniformly regardless of the
  matched type's actual field count is inferred purely by analogy to the
  sheet's own `err is NotFound(_)` example (NotFound's field count isn't
  shown either). Used on `Failure(_)` throughout tasks 1, 2, 3, 6.

## Concurrency primitives are read-only-ish

- `Shared<T>` only exposes `get()` and `update(f)` — there's no atomic
  "increment and return the new/old value" or compare-and-swap. Task 5's
  `create_note` needs to allocate a new integer id *and* hand it back to
  build the HTTP response, which the API doesn't support atomically. I did
  `let id = store.next_id.get()` then `store.next_id.update(n => n + 1)` as
  two separate calls — under real concurrent load two requests could race
  and get the same id. I couldn't find a documented primitive that avoids
  this, so I accepted it as a known gap in this toy example rather than
  inventing an undocumented one.
- Task 6 and Task 8 both nest a `with group = try tasks.group()` block, and
  in Task 6 the whole thing sits inside a lambda passed to
  `time.timeout(...)`, whose last line (a bare variable `outcomes`) is
  presumably the lambda's result. The sheet documents "a multi-line lambda's
  last line is the result" and separately documents `with`/`tasks.group()`,
  but never shows a `with` block used as a lambda body this way. Plausible
  composition, not a shown one.
- Task 8: whether starting a fixed number of worker tasks with `group.start`
  that each loop `for line in jobs` (receiving until a producer in the same
  `with` block calls `jobs.close()`) is a valid combination is likewise
  assembled from two separate examples (`Channel`, and `tasks.group()`),
  not shown together.

## Smaller things

- Sort stability (`sorted_by`, `sort_by`, `.reversed()`) for equal keys
  isn't documented. Task 1's report test has two products tied at
  `stock: 2` and asserts a specific resulting order, assuming a stable sort
  keeps their original list order.
- No documented behavior for `.words()` / `.lines()` on an empty string.
  Task 4's "empty file" test assumes both yield an empty list. Task 2's
  "no header line" test is written to pass either way (empty-list-from-
  `lines()` throws "missing header", non-empty-with-one-blank-line throws
  "wrong header" — both are `Failure`, which is all the test checks).
- No spec for how `${}` interpolation renders numbers — in particular
  whether a whole-number `Float`/`Decimal` prints with a trailing `.0`. I
  avoided asserting on interpolated-string output for numbers anywhere
  ambiguous; where a test does check an interpolated string containing a
  number (Task 10's `describe()` tests), I deliberately used non-whole
  values (`21.5`, `9.99`) to sidestep the question entirely.
- Whether multi-line method-chain continuations (a line starting with `.`)
  are legal is never shown — every stdlib example chains on one line. I
  never used a leading-dot continuation; where a chain got long (Task 1,
  4, 8) I either kept it on one (long) line or split it across intermediate
  `let` bindings instead.
- Whether a bare `try f() catch err` (no enclosing `let`) is legal, and
  whether `test` bodies act as an implicit `throws` context so a bare `try`
  inside a test can fail the test — neither is directly demonstrated (every
  `catch` example in the sheet is attached to a `let`, and every `test`
  example uses `expect`/`expect throws` rather than a bare `try`). Used in
  Task 3's `main()` (bare `try transfer(...) catch err`) and throughout
  Tasks 7 and 9's tests (`with conn = try db.open(...)`, `with dir = try
  files.temp_dir()`, `let users = try conn.query<User>(...)` inside `test`
  blocks).
- Whether an empty collection literal (`{}` / `[]`) infers its type from a
  surrounding function-call argument position, not just from a `let x: T =`
  annotation (the only form shown), and likewise whether a bare numeric
  literal picks up an expected `Decimal`/`Float` parameter type in a
  function call the same way it does for an annotated `let`. Relied on both
  throughout — e.g. `Shared<Map<Int, Note>>({})` (Task 5),
  `combine([])` (Task 10), and passing `80.0` to a `fn(Decimal) -> Decimal`
  (Task 10).
- Map literals with non-string-literal keys (e.g.
  `{AccountId(1): Account(...), AccountId(2): Account(...)}` in Task 3, or
  `{"auth": 1, "billing": 3, "cache": 2}` used with a `Text`-keyed report in
  Task 8) are only ever shown in the sheet with plain string keys
  (`{"ada": 36}`); assumed the same literal syntax generalizes to any key
  expression of the declared key type.
