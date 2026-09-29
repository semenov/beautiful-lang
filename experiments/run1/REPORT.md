# Run 1: can weaker models write Linen from the README alone?

**Setup.** Three agents each got only `README.md` and the 8 tasks in `TASKS.md`:
two Haiku 4.5 samples (`haiku-a`, `haiku-b`) and one Sonnet 5 (`sonnet`).
There is no compiler, so every file was reviewed by hand.

## Headline

- **Syntax held up.** In 24 files there's not one trailing `:`, `elif`,
  `None`, `null`, `const`, `===`, `await`, `function`, `{ }` block or Kotlin
  `it`. Lambdas, `match`/`case`, `.` continuation lines, `if … then … else`,
  `T?`/`??`, `.with(...)` and `throws`/`try` were all used as documented. The
  predicted "Python/TypeScript dialect" slips mostly didn't happen.
- **The trouble is what the README doesn't say,** mainly the standard library.
  Where the docs were silent, the three agents guessed differently.
- **Three design choices caused real bugs:** the inclusive `..` range, the
  "3+ arguments must be named" rule, and the unclear split between `require`
  and `throw`.
- **Sonnet vs Haiku.** Sonnet stayed inside the documented features, flagged
  every guess and broke the argument rule once. Haiku invented more (tuples,
  `catch`-blocks, `new`) and broke rules more often, but its syntax was just
  as clean.

## 1. Design traps (real bugs)

| Problem | Seen in | Detail |
|---|---|---|
| `..` is inclusive | haiku-a, haiku-b | Both wrote `for i in 1..lines.length` and index `lines[i]`, which reads one past the end. haiku-a even wrote in its notes that `..` is inclusive, and still made the mistake. |
| 3+ args must be named | all three | `leftPad(text, 5, "0")` was left unnamed by every agent. `transfer(a1, a2, 30)` (haiku-a ×4) and `transfer(alice, bob, amount: 30)` (haiku-b ×3) were also wrong. Sonnet avoided the problem on `transfer` by reordering the parameters: `transfer(50, from: alice, to: bob)`. |
| `require` vs `throw` | haiku-a, haiku-b | haiku-a used `require balance >= amount` for the business rule "not enough money", which the task says must be a catchable error. Both Haiku samples tested contract violations with `try/catch`. |
| `try` forgotten | haiku-a | Four calls to a `throws` function without `try`. The compiler would catch these, so this is minor. |

## 2. Missing constructs that models reached for

| Construct | Who | Notes |
|---|---|---|
| Tuples `(A, B)` + `let (a, b) = …` | haiku-a, haiku-b | Sonnet wrote small wrapper records instead (`Transfer`, `PopResult`) and noted the extra work. Every agent needed "return two things". |
| `catch` with control flow | haiku-a, haiku-b | Both wrote `let age = try parse_int(x) catch` + an indented `continue`. |
| Testing that something fails | all three | There is no `expect … throws`. Haiku's `try … catch` tests **pass silently** when nothing throws. Sonnet added `expect false` inside the `try` block. |
| Per-branch fallback in `parallel` | all three | haiku-b wrote `return` inside `parallel` (which exits the function). haiku-a relied on implicit optionals. Sonnet moved the `catch` into a helper function. |

## 3. Things the docs don't specify (the agents' guesses differed)

| Question | Guesses |
|---|---|
| Wrapping a value in `T?` | `some(x)` (haiku-b) · plain `x` (sonnet) · mixed (haiku-a) |
| Generic function syntax | `fn pop<T>(…)` (haiku) · `fn pop(stack: Stack<T>)` without `<T>` (sonnet) |
| What `group_by` returns and how to iterate it | `entry.key/entry.value` · `pair[0]/pair[1]` · `.map((k, v) => …)` |
| Dropping the first n items | `drop(1)` · `skip(1)` · `take(len - 1)` |
| Sorting in descending order | `.reverse()` ×2 · `sort_by(x => -x.count)` |
| Emptiness | `is_empty()` · `length == 0` |
| Growing a list | `push` on a `let` (haiku-b, breaks immutability) · `var` + `+=` |
| `Int / Int` | integer or float, left unspecified; `Float(x)` invented |
| Int → Text | `"{n}"` · `n.to_string()` |
| HTTP | `web.fetch` · `https.get` from `node:https` (wrong API) · `fetch` from `node-fetch` |
| JavaScript globals | `Date()` ×2, `date_fns.new Date()` ×1 |
| Name for a hyphenated package | `date_fns` (all three agreed) |
| Shadowing (`let s = push(s, 1)` repeated) | haiku-a only; allowed or not? |
| Where `ensure` goes | haiku-a put it after the computation, with `result[0]` |

## Recommendations

**Design changes**
1. Ranges: `a..<b` is exclusive and `a..=b` is inclusive, and a bare `..` is a
   compile error with a fix. Also document `for (i, x) in list.indexed()`.
2. Argument rule: keep it, but have `linen fix` insert the missing names
   automatically, since it's purely mechanical. Apply it to `extern`/JS
   functions too, so the rule has no exceptions.
3. Contracts: say explicitly that a failed `require`/`ensure` is a *bug*.
   The program stops, and a `catch` can't intercept it. Anything the caller
   should handle uses `throw`. `ensure` has to come right after `require`.
4. Add tuples with destructuring: `(A, B)`, `let (a, b) = …`.
5. Allow `catch continue`, `catch break` and `catch return x`.
6. Add `expect throws <call>` (and `expect throws "text"`).
7. `T` automatically becomes `T?`: `return x` works where `T?` is expected,
   and there's no `some(...)` constructor. `some` stays only as a pattern.
8. Generic functions always declare their parameters: `fn f<T>(…)`.

**Documentation**
9. A standard library reference: `List`, `Text`, `Map`, numbers,
   conversions, `web`, `files`, `json`. Use the names the agents reached for
   most: `length` (field), `is_empty()`, `split`, `lower`, `contains`,
   `sort_by`, `reverse`, `take`, `drop`, `sum`, `group_by` → `Map`,
   `map.entries()`, `to_text()`.
10. Interop: how a hyphenated package gets its name (`date-fns` → `date_fns`),
    and how to reach JavaScript globals (`import js global` or a built-in
    `js.Date`).
11. An `AGENTS.md` cheat sheet containing everything above, compactly.

Then run the same tasks again (run 2) and compare.
