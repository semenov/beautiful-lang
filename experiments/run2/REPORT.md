# Run 2: after the documentation update

**Setup.** The same 8 tasks as run 1, plus task 9 (storages: interfaces and
resources) and task 10 (functions as values and a mixed list of types).

| Agent | Documentation |
|---|---|
| `haiku-a`, `haiku-b` (Haiku 4.5) | `README.md` + `STDLIB.md` |
| `sonnet` (Sonnet 5) | `README.md` + `STDLIB.md` |
| `haiku-cheatsheet` (Haiku 4.5) | **only** `AGENTS.md` |

There is no compiler, so all 40 files were reviewed by hand.

## What the changes fixed (compared with run 1)

| Run 1 problem | Run 2 |
|---|---|
| `1..lines.length` read past the end of the list (2 of 3 agents) | **0 cases.** Everyone used `1..<n` or `drop(1)`. |
| Failure tests that passed silently | **All 4** used `expect throws`, including typed errors (`expect err is InsufficientFunds`). |
| Three different guesses about `group_by` | **All 4** wrote the same thing: `count_each().entries()` + `(word, n) =>`. |
| Invented names (`push`, `skip`, `to_string`, `Float(x)`) | **0 cases.** |
| `Date()`, `https.get`, `node-fetch` | `js.Date()`, `time.now()`, `web.get`, `date_fns`, all as documented. |
| Wrapper records to return two values | Tuples with destructuring (3 of 4). |
| `catch` with control flow | `parse_int(x) catch continue` (sonnet, cheatsheet). |
| Fallback inside `parallel` | `fetch(x) catch none` (sonnet, cheatsheet). |

**Sonnet is almost perfect.** It has two argument-rule violations and a
redundant `try` before `catch` (see below). Everything else is correct,
including `copy_with_prefix<S: Storage + Listable, D: Storage>` and a
`List<Describable>` with two different types.

**The cheat sheet alone is nearly enough.** The Haiku that saw only
`AGENTS.md` did about as well as the Haiku agents that saw the full
documentation. On `catch continue` and `catch none` it did better. Its
mistakes come from specific gaps in the cheat sheet (see section 3).

## 1. Remaining problems in the design

| # | Problem | Who | Proposal |
|---|---|---|---|
| 1 | **`try f() catch x`**: a redundant `try` in front of `catch` | **all 4** (sonnet ×4) | Make it the rule. `try` marks *every* call that can fail, and `catch` says what to do instead of passing the error up: `let port = try parse_int(t) catch 8080`. Models do this naturally, and the rule gets simpler. |
| 2 | **The argument rule** | haiku-b ≈14 calls, sonnet 2, cheatsheet 1, haiku-a 0 | Stop treating it as a compile error. The formatter (`linen fmt`) inserts the names. Code still reads well, and agents never trip on it. |
| 3 | **`expect throws` on a failed `require`** | haiku-a ×2, cheatsheet ×1 (haiku-b noticed and was unsure) | Inside `test` blocks, let `expect throws` also catch contract failures. Outside tests, contracts still can't be caught. Testing contracts is a legitimate need. |
| 4 | **`catch` + an indented block** | haiku-a (task5, task9). Both Haiku did this in run 1 too. | Allow it: `f() catch err` followed by a block, where the last line is the value or `return`/`continue`/`throw`. It's the same pattern as `match` arms and multi-line lambdas. |
| 5 | **Type aliases** `type Rule = fn(Decimal) -> Decimal` | haiku-a, haiku-b | Add them. Both invented exactly this syntax. |
| 6 | **Multi-line `if … then … else`** | haiku-b, cheatsheet | Allow `if` as an expression with indented blocks, like a `match` arm. |
| 7 | **`parallel` over a list** | sonnet (worked around it with three parameters) | Add `xs.parallel_map(f)`. |

## 2. Generics vs a list of interfaces

The most common *semantic* mistake:

- `copy_prefix<S: Storage + Listable>(from: S, to: S)` requires both storages
  to have the same type, so copying from memory to files is impossible
  (haiku-a, cheatsheet).
- `print_descriptions<T: Describable>(items: List<T>)` for a list with two
  different types (haiku-b).
- The cheatsheet agent wrapped the types in an `enum` because it "didn't know
  how to make a mixed list".

Only Sonnet got it right. The documentation needs an explicit example with
two type parameters, and a mixed list:
`let items: List<Describable> = [Point(…), User(…)]`.

## 3. Gaps in the cheat sheet (`AGENTS.md`)

| Mistake | Cause |
|---|---|
| `fn f() -> T throws` (×4) | The text says "before `->`", but there's no example in the code. |
| No `import web` / `import json` / `import files` | Imports aren't stressed. |
| Interface methods without `self` | Only one interface example. |
| `extern` with a renamed function (`left_pad` instead of `leftPad`) | It doesn't say that the name must match the JavaScript export. |
| Enum instead of `List<Describable>` | There's no mixed-list example. |

## 4. Small things the compiler would catch

- `let people = []` followed by `append` (haiku-a), and reassigning a `let`
  (haiku-a, task10).
- Mixing `Float` and `Decimal` (`percent: Float` × `total: Decimal`, haiku-a).
- `some(x)` as a constructor (haiku-a).
- Indexing a tuple, `top_three[0][0]` (haiku-a).
- `'no name'` in single quotes (haiku-b).
- A `throws` call without `try` inside a `try` block (haiku-b).

## Conclusion

The changes after run 1 worked. Every problem they targeted went away. What's
left is:
- a few rules that fight habits (points 1–4), which are better adopted than
  fought;
- three small constructs to add (5–7);
- one example that's missing from the documentation (section 2).

Syntax borrowed from other languages still didn't show up in 40 files.
