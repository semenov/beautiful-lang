# Notes on guesses

The README (`/Users/semenov/Dev/beautiful-lang/README.md`) doesn't specify a
standard library in any detail — it shows a handful of collection functions
(`filter`, `map`, `sort`, `sort_by`, `take`, `group_by`, `max_by`, `sum`) and a
couple of free functions (`parse_int`, `print`, `PI`) in passing, but there's
no reference for strings, list indexing, list construction/concatenation, or
what modules like `web` expose. Everything below is a guess I made to fill
those gaps, organized by task.

## General, cross-task guesses

- **List indexing with `list[i]`.** Never shown as an operation you perform,
  but the decode-error example `items[2].price: expected Decimal, got "12"`
  implies this bracket syntax exists and matches source syntax, so I used
  `xs[0]`, `xs[1]`, etc.
- **`.length` is a field, not a call.** The tour shows `words.max_by(w =>
  w.length)` — no parens — so I used `.length` (not `.length()`) everywhere,
  including on `List` and on `Error`'s `.message` (also assumed to be a
  plain field), per the "no parens = field" resolution rule.
- **`Text.split(sep)`** — assumed to exist and behave like the common
  split-on-separator function, returning `List<Text>`. Not documented.
- **`List<T>.skip(n)`** — assumed to exist alongside the documented `.take(n)`.
- **List concatenation with `+`** (`[item] + stack.items`, `people += [x]`) —
  `+` is a listed operator but only shown for numbers/strings; I guessed it
  also works for lists (Python/JS-like semantics).
- **`Error("message")`** — used as shown in the README's own example
  (`throw Error("port must be positive, got {port}")`), so this one is
  confirmed, not a guess.

## task1.ln — Inventory

- Nothing especially uncertain beyond the general list guesses above.
  `sort_by` is assumed to sort ascending by the key (matches "sorted from
  lowest stock to highest" directly), based on the one example in the README
  (`orders.sort_by(o => o.total)`), which doesn't state a direction either.

## task2.ln — CSV

- `Text.split(",")` / `Text.split("\n")` — see general notes.
- List indexing (`fields[0]`, `fields[1]`, `lines[0]`) — see general notes.
- `var people: List<Person> = []` plus `people += [person]` inside a `for`
  loop — the empty list literal `[]` and `+=` on a list are both guesses.
- Wrapped `parse_int` in an inner `try`/`catch` (returning `none` on failure)
  rather than guessing at a `catch none` shorthand, since the README only
  showed `try`/`catch` as a block form and `catch <value>` as a same-type
  fallback (`parse_int(text) catch 8080`), which wouldn't type-check against
  `Int?`.

## task3.ln — Shapes

- Low risk: `PI`, `match`/`case ... if ...`, and enum variants are all shown
  directly in the README's own examples. `Rectangle(3, 4)` positional
  construction mirrors the README's own `Rectangle(3, 4)` example.

## task4.ln — Bank transfer

- No `tuple` type is documented, so a transfer returning two updated accounts
  needed a small dedicated record type (`Transfer { from, to }`) — a guess at
  how you'd model "return two things" in Linen, since only single return
  values are shown.
- Trailing comma after the last named argument in a multi-line call — the
  README says the formatter *adds* trailing commas to multi-line lists, which
  I read as implying they're also legal in multi-line calls.

## task5.ln — GitHub users

- **`import web` and `web.fetch(url)`.** The README's Modules section lists
  `import web` as an example import but never says what it contains. I
  guessed a `fetch(url) throws -> Text` function returning the raw response
  body, analogous to `files.read(path)` shown for the `files` module.
- Assumed `json.decode<T>(text)` (shown in the README) works the same way for
  an HTTP response body as for any other JSON text.
- Used string interpolation for the URL: `"https://api.github.com/users/{username}"`.
- Rather than letting the `parallel` block cancel siblings on one failure (as
  the README says it does for `throws` functions), I made `describe_user`
  catch its own error internally so it never throws — this was necessary to
  satisfy "if one fetch fails, print a fallback line for that user and still
  show the others," since a throwing `parallel` cancels every branch.

## task6.ln — Interop

- **`date_fns.format(...)`.** The README shows `import js "marked"` being
  used as `marked.parse(...)` — i.e., the module namespace is derived from
  the package name. For `"date-fns"` (not a valid identifier due to the
  hyphen) I guessed the namespace becomes `date_fns`. This is unconfirmed.
- **`Date()` as a global constructor.** The README shows foreign classes
  called like constructors (`Database("app.db")`), but doesn't say whether
  plain JS globals like `Date` are available without an explicit `import js`.
  I assumed built-in JS globals are reachable directly; this is the shakiest
  guess in the file. An alternative (not taken) would be to `import js
  "node:process"` or similar for a clock source, but nothing in the README
  suggests that either.
- The `left-pad` `extern` block is copied essentially verbatim from the
  README's own example, so that part is confirmed syntax.
- Padding a number: `leftPad` takes `Text` as its first argument, so I
  converted the `Int` via interpolation (`"{n}"`) before calling it, since no
  `int_to_text`/`to_text` function is documented.

## task7.ln — Word stats

- Splitting on `" "` only — the README gives no tokenizer, so punctuation
  attached to words (e.g. "cat." vs "cat") is not stripped. Flagged as a
  simplification rather than a real parser.
- **`Map.map((key, value) => ...)`.** `group_by` is shown once
  (`people.group_by(p => p.city)`) with no indication of what type it returns
  beyond "grouped", and no example of iterating/transforming a `Map`. I
  guessed it returns `Map<Text, List<Text>>` and that `Map` supports a
  two-argument `.map((k, v) => ...)` to project each entry into a new list
  element. This is the biggest guess in the whole exercise — there may be a
  completely different documented (or intended) way to do frequency counts.
- Used `sort_by(wc => -wc.count)` (unary minus) to sort descending, since no
  `.reverse()` or descending-sort flag is documented; unary `-` itself isn't
  shown either, only binary `-`, but seemed like the safest way to avoid
  inventing a second unknown function.

## task8.ln — Stack

- No `Stack`/generic-container example exists beyond the syntax
  `type Stack<T>` mentioned once as a name in passing ("Generic types use
  angle brackets: `type Stack<T>`, ..."), so the whole implementation (fields,
  push/pop/peek behavior, and the `PopResult<T>` helper type for returning
  both the popped item and the new stack) is original design work, not
  something the docs described.
- `pop` needs to return both "the top item, or nothing" *and* the resulting
  stack (since values are copied, never mutated) — modeled with the same
  kind of small wrapper record used in task4, for the same reason (no tuples
  documented).
- `empty_stack() -> Stack<T> = Stack(items: [])` uses the single-expression
  `fn ... = expr` form with an inferred generic `T`; whether a bare `[]`
  list literal can appear without any contextual type nearby and still
  infer `List<T>` is a guess.
