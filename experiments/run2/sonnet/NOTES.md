# Notes on guesses made while writing these Linen programs

The documentation (`README.md`, `STDLIB.md`) doesn't cover every situation
these tasks needed. Below is, per task, what I had to guess.

## task1.ln — Inventory

- Assumed `Product` should be an ordinary `type` (value), so `restock` returns
  a *new* `Product` via `.with(stock: ...)` rather than mutating in place.
  Nothing in the docs suggests inventory needs shared/resource semantics.
- Added `require amount > 0` on `restock` — not asked for explicitly, but it
  matches the doc's own guidance ("a buggy caller" passing a bad amount).

## task2.ln — CSV

- Assumed `var people: List<Person> = []` is legal — the docs only show a type
  annotation on `let` (`let price: Decimal = 19.99`), never on `var`.
- Assumed `line.split(",")` on a malformed line (wrong number of fields) is
  simply skipped by checking `parts.length == 2`, and that
  `parse_int(...) catch continue` correctly skips a row whose age isn't a
  number (both used exactly as shown in the docs, just combined).
- Assumed comparing a `Text` line to a literal header string with `!=` works
  as expected (operator is listed, but not shown compared against `Text`).

## task3.ln — Shapes

- Added a third enum variant (`Triangle`) beyond the two shown in the
  `Shape` example in the README; the case syntax for a 2-field variant was
  assumed to generalize directly.
- Used the **statement** form of `if / else if / else` with explicit
  `return`s in each branch for `describe`, since the docs only show the
  ternary **expression** form (`if … then … else …`) for two-way choices and
  never demonstrate chained `else if … then` inside an expression.

## task4.ln — Bank transfer

- Modeled `Account` as a `type` (value), matching the README's own
  `withdraw` example, and had `transfer` return a `(Account, Account)` tuple
  of the two updated accounts rather than mutating anything.
- Assumed a `try / catch` block is allowed to `return` a value from inside
  both the `try` and the `catch` arms (the README's own `try/catch` example
  only performs side effects, never returns).

## task5.ln — GitHub users

- Assumed `parallel` only supports a fixed, written-out set of `let` branches
  (as in the README's Paris/Tokyo example), so `print_github_users` takes
  three named parameters instead of looping over a `List<Text>` of
  usernames inside `parallel`.
- Assumed `if user is some(u)` is a valid way to branch on an `Optional`
  value produced by `... catch none` (the docs show `is some(...)` only in
  an `if` used as a boolean check, and `catch none` only inside `parallel`
  branches — I combined the two).
- Assumed GitHub's JSON field names (`login`, `name`, `bio`) need no
  `@json(...)` annotation since they already match the field names.

## task6.ln — Interop

- Assumed `date_fns.format(date, formatStr)` is the correct call shape for
  the real `date-fns` npm package (the docs never show a concrete
  `date-fns` call, only that the package is available as `date_fns`).
- Assumed `time.now()` (type `Time`) is an acceptable argument for whatever
  `date-fns`'s `.d.ts` declares as its date parameter.
- Copied the `left-pad` `extern` block verbatim from the README's own
  example and assumed no separate `import js "left-pad"` line is needed
  when an `extern` block already names the package.
- Assumed `date_fns.format` throws (so needs `try`) since it's an untyped
  boundary call unless declared `nothrow`, per "JavaScript can throw
  anywhere."

## task7.ln — Word stats

- Assumed "average word length" means the average over *all* words in the
  text, not just the ones longer than 3 letters used for the top-3 count —
  the task sentence is ambiguous about scope.
- Assumed `List<(Text, Int)>` supports `==` comparison element-by-element
  (the docs only state that `==` compares records/enums/tuples field by
  field, not lists of them explicitly).
- Assumed ties in `sort_by` are stable enough that testing exact order for
  distinct counts is safe, but avoided asserting an order for a tie (two
  words with equal counts) in the second test.

## task8.ln — Stack

- Modeled `Stack<T>` as a **generic `resource`** (`var items: List<T> = []`)
  rather than a `type`, so `push`/`pop` can mutate it in place using the
  already-`var` `List.append`/`List.pop`. The docs only show generics on
  `type` (`Stack<T>`) and `enum`/`interface`, never explicitly on
  `resource` — this is the main structural guess in the whole task set.
- Assumed a resource/type with only defaulted fields can be constructed with
  an explicit type argument and no arguments, e.g. `let nums: Stack<Int> =
  Stack()`, by analogy with `User(...)` omitting a field that has a default.

## task9.ln — Storage

- Assumed a `resource` can mix a plain field (`FileStore`'s `dir: Text`)
  with the `var` fields shown in the docs' own `MemoryStore` example — only
  `var` fields are documented as changeable, but nothing said a resource
  can't also have a non-`var`, non-changing field.
- Assumed `files.list(dir)` returns names that are usable directly as
  storage keys (i.e., no further path manipulation needed) for the
  file-backed store.
- Assumed the `catch err => throw Unavailable(reason: err.message)`
  translation pattern from the README's `Storage` example generalizes to
  every I/O call in `FileStore`, not just `put`.

## task10.ln — Pricing rules

- Represented a "pricing rule" directly as a function value
  `fn(Decimal) -> Decimal`, per the "Functions as values" section, rather
  than wrapping it in a named record type — the task never specifies a
  concrete `PricingRule` type, so I treated the function type itself as the
  rule.
- Assumed `fold`'s `step` parameter is an ordinary two-argument lambda
  `(acc, rule) => ...`, not tuple-unpacking syntax (the docs describe it as
  "`f(acc, x)` for every element", and tuple-unpacking lambdas are shown
  only for lambdas receiving an actual tuple argument, e.g. from
  `.entries()`).
- Assumed a `List<Describable>` can be built directly from a literal mixing
  two unrelated concrete types (`Point`, `Temperature`) when the variable's
  declared type is `List<Describable>`, per "Lists convert."
