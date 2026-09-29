# Run 3: the new design, braces vs indentation

**Setup.** 10 tasks for CLI tools and backends (`TASKS.md`). Each agent saw
only one cheat sheet and the tasks. The two cheat sheets differ only in block
syntax: `CHEATSHEET-indent.md` was generated from `CHEATSHEET-braces.md`.
There is no compiler, so all 40 files were reviewed by hand.

| Agent | Cheat sheet |
|---|---|
| `haiku-braces`, `haiku-indent` (Haiku 4.5) | braces / indentation |
| `sonnet-braces`, `sonnet-indent` (Sonnet 5) | braces / indentation |

## 1. Braces vs indentation: no difference

Neither variant produced a single syntax slip in 40 files:
- no `:` after `if` and no stray braces;
- no `&&`, `||` or `!`;
- no `+` on text and no `{x}` interpolation;
- no `elif` and no semicolons.

Multi-line lambdas, blocks inside `match` arms and `catch` blocks were
written correctly in both variants.

**Conclusion:** with a good cheat sheet, block syntax isn't where agents make
mistakes. We keep braces for the reasons that don't depend on the
experiment:
- editing by string replacement doesn't require re-indenting;
- diffs are cleaner;
- truncated output is caught by an unbalanced brace.

## 2. What worked

| Rule | Result |
|---|---|
| Path mutation `products[i].stock += n` | 4/4 |
| `&` at the call site | 4/4 |
| `/` on `Int` | 1 violation in 40 files (the compiler would catch it); everywhere else `to_float()` |
| `catch err { continue }`, `catch err { none }`, rethrow with `throw err` | 4/4 |
| `with` for the database, files, `temp_dir`, task groups | 4/4 |
| `expect throws` + `err is Type(_)` | 4/4 |
| New types `AccountId(1)`, `id.value` | 4/4, no mix-ups |
| `List<Describable>` for mixed types | 4/4 (the main mistake in run 2) |
| `source: Storage + Listable` for the storage task | 3/4 (haiku-indent used concrete types) |
| `tasks.group()` + `run` + `wait`, `Channel` + `Shared` for workers | Sonnet perfect, Haiku partly |
| An interface for a fake in tests (task 6) | sonnet-indent came up with it on its own |

**Sonnet was almost flawless in both variants.** The remaining problems are
listed below.

## 3. Rules that fight habits

### 3.1 Where `var` goes in a parameter
Haiku wrote `fn restock(var products: List<Product>)` **in every case** (both
variants, 6 functions). Sonnet wrote `products: var List<Product>`
everywhere.

**Decision (2026-09-29):** `var` parameters are removed entirely. Discussing
this point showed that a reader doesn't understand what `var` does on a
parameter, whatever the keyword. Now a function returns the changed value
(`products = restock(products, …)`), and in-place changes go through
`mutating` methods on your own type.

### 3.2 The argument-naming rule: the cheat sheet contradicts itself
The rule says the receiver counts, so `conn.query(sql, params)` and
`router.get(path, handler)` have 3 arguments and need names. But the cheat
sheet itself writes them positionally. Both Sonnet agents noticed the
contradiction, and each resolved it with a different compromise. Haiku
simply called `transfer(&accounts, a, b, amount)` positionally.

**Proposal:** the receiver **doesn't count**, and names are required when a
function has 3 or more parameters besides the receiver.
- `router.get(path, handler)`, `conn.query(sql, params)` and
  `store.put(key, value)` stay positional.
- `move(book, from: shelf, to: box)` is still named.
- `text.replace(old:, new:)` would become `text.replace("-", "_")`. The
  standard library can keep names where the order isn't obvious.

### 3.3 "Results must be used" vs `conn.execute(...)`
Haiku ignored the result of `execute` 11 times. Sonnet obeyed and wrote
`let _ = try conn.execute(...)` 19 times, plus `let _ = self.data.remove(key)`.
The rule is right, but the standard library isn't designed around it.

**Proposal:** keep the rule and fix the standard library:
- `execute` returns nothing, and a separate `execute_counting` returns the
  row count;
- `map.remove(k)` returns nothing, and `map.take(k) -> V?` removes and
  returns the value.

## 4. Design gaps (the most valuable part)

### 4.1 State in HTTP handlers
- Haiku didn't know how to give handlers shared state:
  - haiku-indent changed a captured `var` (forbidden);
  - haiku-braces used global `var`s (forbidden).
- Sonnet found the right pattern: `Shared<State>` in `main`, with lambdas
  `req => try handle(state, req)`.
- But both Sonnet agents noted a **race**: `update` returns nothing, so
  "take the next id and increment it" is two separate operations.

**Proposal:** `update` receives a **mutable** value and returns whatever the
lambda returns:
```
let id = state.update(s => {
  s.next_id += 1
  s.next_id
})
```
This also removes the boilerplate `var updated = current; …; updated`, which
all four agents wrote. The cheat sheet needs a stateful handler example.

### 4.2 Named function types
We removed aliases, but the tasks needed a name for `fn(Decimal) -> Decimal`:
- haiku-indent wrote a forbidden alias;
- haiku-braces wrapped it in a record and called it with `rule.apply(x)`,
  which clashes with "a method is looked up only in the type";
- Sonnet repeated `fn(Decimal) -> Decimal` 5 times per file.

**Proposal:** `type Rule = fn(Decimal) -> Decimal` is a new type, like any
other `type X = …`, with two additions:
- a lambda **takes the expected type**, the same way a number literal does:
  `fn percent_off(p: Decimal) -> Rule { return total => … }`;
- a `Rule` value can be called directly: `rule(total)`.

### 4.3 `m[key]` gives `V?`, but `accounts[id].balance -= x` is natural
3 of 4 agents wrote a field change through `m[key]`. Only sonnet-indent
unwrapped, changed a copy and wrote it back.

**Proposal:** writing through a path, `m[key].field = …` or
`m[key].field -= …`, is allowed. A missing key is a bug, just like `xs[i]`
out of range. Reading `m[key]` still gives `V?`.

### 4.4 Flow typing after `is`
Sonnet asked for it directly, and all agents needed it:
- after `if x is none { throw … }`, `x` should be `T`;
- after `if err is InsufficientFunds`, the fields should be accessible.

Today you have to write `match` everywhere, or `InsufficientFunds(_, _, _)`
with one `_` per field. The two Sonnet agents even wrote this differently
(`(_)` and `(_, _, _)`).

**Proposal:**
- `err is InsufficientFunds` without parentheses is allowed, and inside that
  `if`, `err` has type `InsufficientFunds`.
- The same applies to `T?` after an `is none` check that exits the block.
- This works like smart casts in Kotlin and TypeScript, which agents already
  know.

### 4.5 Smaller points
- **`return` inside a lambda.** It's unclear whether it exits the lambda or
  the enclosing function. **Proposal:** it's forbidden, and the last line is
  the value.
- **`throw` / `return` / `continue` in `match` arms and `??`.** Sonnet
  generalized the `catch` rule on its own. **Proposal:** make it official.
- **Multi-line chains** (a line starting with `.`) weren't documented, so
  Sonnet broke every chain into `let`s. **Proposal:** document them.
- **An interface method is `throws`, but the implementation doesn't fail**
  (`MemoryStorage`). **Proposal:** an implementation may declare less than
  the interface. It doesn't have to be `throws` or `var self`.
- **An error in a task handled in `wait()`.** It's unclear whether the group
  still throws it. **Proposal:** yes, as the cheat sheet says, and the
  documentation should point more clearly to `catch` inside the task.
- **Descending sort.** Agents wrote `.reversed()` (fine) and `0 - x` (awkward).
  **Proposal:** no change, but add a documentation hint.
- **The `message` field clashes with the `message()` method** in an error
  type. **Proposal:** a compile error with a hint to use `Failure`.
- **Not in the cheat sheet:**
  - the names `db.Connection` and `tasks.Task<T>`;
  - `\"` inside strings;
  - regular expressions (haiku-indent invented `r"..."`);
  - number formatting in interpolation.

## 5. Task-level mistakes (not the design's fault)

- haiku-indent: task 8 never printed the result. Task 9 used concrete types
  instead of interfaces.
- haiku-braces: task 6 put a timeout on each `wait()` instead of the whole
  job. Task 8 read the whole file instead of reading it line by line.
- Both Haiku agents called interface methods that throw from methods that
  aren't `throws`.
