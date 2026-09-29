# Notes on writing these tasks

General cross-cutting uncertainties first, then task-specific ones.

## Cross-cutting

1. **Multi-line method chains.** "There are no semicolons; a statement ends
   at the end of the line" made me nervous about writing chains like
   `xs.map(f).filter(g).take(n)` split across several lines. The cheat sheet
   never shows a chain continuing after a newline (only multi-line lambda
   *blocks*, which are explicitly a block with braces). To be safe I never
   split a method chain across lines anywhere — every chained
   transformation gets its own `let` (see task4's `word_counts`/sorting
   pipeline, task8's sort-and-print). This makes the code more verbose than
   I'd like; I genuinely don't know if inline chaining across lines is legal.

2. **`??` with a control-transfer fallback.** The cheat sheet shows
   `x ?? fallback` only with a plain value on the right. I wanted to write
   `accounts[from] ?? throw AccountNotFound(...)` or `... ?? continue` in
   several places (tasks 2, 3, 4, 9) but backed off every time and used an
   explicit `if`/`match` instead, since it's unclear whether `throw`,
   `continue`, `return` are valid *expressions* on the right of `??` (as
   opposed to being valid as the last line of a block).

3. **Generalizing "the catch block ends with a value, or with `return`,
   `break`, `continue` or `throw`."** This sentence is written specifically
   about `catch` blocks. I generalized it to `match` arms and `if` blocks
   used as expressions (e.g. task3's
   `match accounts[from] { some(acc) => acc, none => throw AccountNotFound(id: from) }`,
   and task9's `if value is some(v) { try destination.put(...) }` as a bare
   statement match). I'm fairly confident this generalization is intended
   (it's the obvious "never type" pattern), but it's not literally stated
   for anything but `catch`.

4. **`try` as a sub-expression.** Every cheat sheet example puts `try` either
   at the very start of a `let`/`with` line, as a whole statement, or as the
   entire body of a single-expression lambda (`() => try load_user(id)`).
   It's never shown nested inside a larger expression like
   `expect (try f()) == x`. I avoided that shape everywhere by always
   binding the `try` call to its own `let` first (see task9's storage test,
   which pulls `disk.get(...)` results into `user1`/`user2`/`order1` before
   comparing).

5. **Argument-naming rule vs. the stdlib's own examples.** The stated rule
   is "with 3 or more \[total arguments, receiver included], name every
   argument after the first," matching `text.replace(old:, new:)` (receiver
   + 2 = 3, both named). But the cheat sheet's own worked examples for
   `conn.query<T>(sql, params)` / `conn.execute(sql, params)` are shown
   fully positional (receiver + 2 = 3 total) throughout the "Resources" and
   "Standard library" sections. That's a direct contradiction with the
   stated rule as I read it. I resolved it by copying the literal stdlib
   examples verbatim where the cheat sheet shows them (task7's
   `conn.execute(...)`, `conn.query<User>(...)`, both positional), and by
   applying the *stated rule* for my own custom methods where no
   counter-example exists (task9's `Storage.put`, called as
   `memory.put(key: ..., value: ...)` since receiver + 2 = 3). I'm not
   confident these two choices are actually consistent with each other in
   the real language — they're my best guess reconciling a contradiction.

6. **Compound assignment through a path.** The cheat sheet shows plain `=`
   through a path (`users[0].name = "Ada"`) and `+=`/`-=` only on simple
   local `var`s (`fuel -= 1`, `self.count += 1`), never both combined. I
   assumed `products[i].stock += amount` (task1) and
   `accounts[from].balance -= amount` (task3) are legal by composing the
   two rules. Reasonably confident, but it's never shown directly.

7. **Literal type inference outside of annotated `let`/field/param
   positions.** `let price: Decimal = 19.99` is the only documented case of
   "a literal takes the expected type." I leaned on the same idea for
   things like `amount <= 0.0` where `amount: Decimal` (task3), and
   `1.0 - percent / 100.0` inside a function returning `Decimal` (task10).
   I never used a bare unadorned numeric literal in a spot where I couldn't
   point to a concrete expected type driving the inference, and I avoided
   unary minus entirely (no operator table lists it) by using
   `.sorted_by(key).reversed()` instead of a negated sort key for
   descending order (tasks 4 and 8).

8. **`with` blocks as expressions.** `if`/`match` are explicitly documented
   as expressions whose value is the last line. `with` is never described
   that way — its examples only use it for side effects inside `main`. I
   never wrote `let x = with r = ... { ... }`; instead I declare a `var`
   before the `with` and mutate it inside the block, using the `with` block
   purely for scoping/cleanup (task6's `fetched` list, task8's
   `process_log`, which returns via a `let ... return` after the `with`
   rather than from inside it).

## Task-specific

- **Task 1.** `restock` has 3 parameters (`products`, `name`, `amount`), so
  per the naming rule I named `name:`/`amount:` at the call site and kept
  `products` (with `&`) positional first — I initially wrote it fully
  positional and had to fix it.

- **Task 2 / 3.** Custom error types (`MissingHeader`, `BadHeader`,
  `AccountNotFound`, `InsufficientFunds`, `InvalidAmount`) are plain
  `type ... implements Error` records, by analogy with the doc's own
  `NotFound(id: id)` / `err is NotFound(_)` example. But the doc never shows
  (a) a *zero-field* error record's construction/pattern syntax — I guessed
  `MissingHeader()` for both, mirroring `Counter()`; or (b) a pattern for a
  record with *more than one* field — I guessed one wildcard per field,
  positionally, mirroring the enum pattern `Rectangle(w, h)`, hence
  `InsufficientFunds(_, _, _)` for its three fields (`id`, `balance`,
  `amount`) in task3. Both are inferences from the enum-variant pattern
  syntax, not confirmed for plain record types.

- **Task 3.** `AccountId`/`CustomerId` are new types over `Int`, per the
  "Account ids and customer ids are both numbers, but must never be mixed
  up" requirement. `CustomerId` ends up unused in the final code (the task
  only asked to keep the two kinds of id from mixing; there was no natural
  place to use a customer id once I settled on an account-only transfer
  function), so it's declared but otherwise decorative — flagging in case
  that reads as odd.

- **Task 4 / 7 / 8.** `cli.decode<Options>()`'s `args: List<Text>` field —
  the cheat sheet says it "collects positional arguments" but doesn't say
  whether it needs a default. I gave it `= []` everywhere so a missing
  argument produces a clean, custom error message from my own code (a
  `Failure`) rather than relying on unknown built-in `cli.decode` failure
  behavior. Task 4's "average word length": ambiguous whether it's the
  average over *all* words in the file or just the words that passed the
  `--min-length` filter; I computed it over all words, since the sentence
  reads as a separate, whole-file statistic.

- **Task 5.** "No top-level code" rules out a module-level
  `let notes_state = Shared<NotesState>(...)`, which is the obvious way to
  share state between two independently-registered route handlers. I
  worked around this by creating the `Shared<NotesState>` inside `main()`
  and writing the two handlers as plain functions that take the state as an
  explicit first parameter, then registering `req => try handle_x(state, req)`
  lambda adapters with the router (closures capturing `state` — since
  `Shared<T>` is a handle/reference type, I'm assuming "captured by copy"
  copies the handle, not a deep copy of its contents; this is never stated
  explicitly but seems like the only sensible reading given `Shared<T>` is
  billed as "the only shared mutable state"). Separately: `Shared<T>.update(f)`
  has no documented return type, and the one example
  (`hits.update(n => n + 1)`) never captures a result — combined with
  "ignoring a return value is a compile error," this implies `update`
  returns nothing at all. That means there's no atomic "get old value, then
  update" primitive, so `handle_create_note`'s id assignment
  (`state.get().next_id`, then a separate `state.update(...)`) has a benign
  but real race under concurrent requests. I left it as the best fit for
  the documented API and called it out here rather than silently hiding it.

- **Task 6.** Assumed `json.decode<T>` treats a JSON object that's missing a
  key as `none` for an optional (`T?`) field, the same way `cli.decode`
  treats a missing flag — this is stated for `cli.decode` but not for
  `json.decode`, though the task explicitly requires it ("`name` and `bio`
  may be missing").

- **Task 7.** Guessed the type name `db.Connection` for the value bound by
  `with conn = try db.open(...)`, purely so I could give `seed_users` and
  `users_older_than` an explicit parameter type — the cheat sheet never
  names this type anywhere, it only ever appears inline. Also relied on
  "params is a list whose items can mix `Int`, `Float`, `Decimal`, `Text`,
  ... " to write literal mixed lists like `["Ada", 36]` directly as a
  `List` literal, even though every other list in the cheat sheet is
  homogeneous (`List<T>`) — there's no visible union/`Any` type that would
  explain the underlying mechanism.

- **Task 8.** Chose a `Channel<Text>` + `Shared<Map<Text, Int>>` worker-pool
  design (four `group.start` workers pulling lines off one channel) over a
  simpler "split into 4 chunks, `group.run` each, merge" design, since the
  task says "four workers process lines concurrently," which reads more
  literally as a persistent pool than as four one-shot parallel chunks.
  This is a judgment call about which stdlib pattern the task intends.

- **Task 9.** Assumed `interface` methods can declare `var self` (never
  shown in the cheat sheet — only concrete-type methods like
  `Counter.increment` demonstrate `var self`), and that once an interface
  fixes a method's self-mutability, every implementer must match it exactly
  — so `FileStorage.put`/`delete` take `var self` even though they never
  actually mutate any field of `self` (they just perform filesystem I/O).
  Also gave `Storage.get/put/delete` a `throws` signature (needed for
  `FileStorage`'s real I/O failures) even though `MemoryStorage`'s
  implementations never actually throw — assumed a `throws`-declared method
  is allowed to simply never throw in a given implementation. Finally,
  `destination: var Storage` combines an interface type with the `var`
  parameter modifier; that combination isn't shown anywhere (the `var`
  examples all use concrete types like `List<Text>`).

- **Task 10.** Straightforward; the one small guess is that a rule
  ("percent off", "cap") is naturally represented as a value of function
  type `fn(Decimal) -> Decimal`, and that a `List<fn(Decimal) -> Decimal>`
  parameter (task's "combine a list of rules") is legal — the cheat sheet
  shows function types as return types and field-less examples but never a
  `List` of function values.
