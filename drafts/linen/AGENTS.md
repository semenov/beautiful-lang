# Linen cheat sheet for agents

Linen is a statically typed language with indentation blocks, similar to
Python/TypeScript/Kotlin/Swift. It compiles to native binaries. This page is
enough to write correct code. Details are in `README.md` and `STDLIB.md`.

## Shape of a program

```
import files                                 # standard modules need an import too:
import json                                  # files, web, json, time, env, random

type User                                    # record (a value, never shared)
  name: Text
  age: Int
  email: Text? = none                        # optional field with a default

enum Shape                                   # variants
  Circle(radius: Float)
  Rectangle(width: Float, height: Float)

type Rule = fn(Decimal) -> Decimal           # type alias

resource Cache                               # shared state (a handle)
  var data: Map<Text, Text> = {}

interface Describable                        # satisfied automatically
  fn describe(self) -> Text                  # self = the value itself, always first

fn describe(user: User) -> Text = "{user.name} ({user.age})"   # now User is Describable

fn area(shape: Shape) -> Float
  return match shape
    case Circle(r) => PI * r ^ 2
    case Rectangle(w, h) => w * h

fn load(path: Text) throws -> User           # throws goes BEFORE ->
  let text = try files.read(path)            # every call that can fail starts with try
  return try json.decode<User>(text)

fn first_or<T>(xs: List<T>, fallback: T) -> T = xs.first() ?? fallback

test "areas"
  expect area(Rectangle(3, 4)) == 12

test "load fails on a missing file"
  let err = expect throws load("nope.json")
  expect err.message.contains("nope")

print("hello")                               # top-level code runs as the program
```

## Rules that are easy to get wrong

1. **Blocks.** No `:` after `if`/`for`/`fn`, no braces, 2-space indent.
   Write `else if`, not `elif`.
2. **Imports.** `files`, `web`, `json`, `time`, `env` and `random` need
   `import`. The prelude (`print`, `parse_int`, `min`, `max`, `PI`, `Error`)
   doesn't.
3. **Arguments.** With 1–2 parameters, call positionally. With 3 or more,
   name every argument after the first: `move(book, from: shelf, to: box)`.
   The receiver in `x.f(...)` counts as the first argument:
   `store.put(key: k, value: v)`. `linen fmt` adds missing names, but write
   them yourself.
4. **Method syntax.** `x.f(y)` means `f(x, y)` for any function. `x.name`
   without parentheses is always a field.
5. **Ranges.** `1..=n` includes `n`. `0..<n` excludes `n`. A bare `..` doesn't
   exist. For indices, use `for (i, x) in xs.indexed()`.
6. **Missing values.** A value that may be missing has type `T?`. The empty
   value is `none`. Write `return user`, not `return some(user)`. Use
   `some(x)` only in patterns: `if x is some(v)`. Other tools: `x ?? fallback`,
   `x?.field`.
7. **Errors.** `fn f() throws -> T` or `throws MyError`, always **before**
   `->`. **Every** call to such a function starts with `try`:
   - `try f()` passes the error up.
   - `try f() catch 0` gives a fallback value.
   - `try f() catch continue` / `catch break` / `catch return none` changes
     control flow.
   - `try f() catch err => throw MyError(err.message)` translates the error.
   - `try f() catch err` followed by an indented block, whose last line is the
     value.
   - A `try` … `catch err` block handles a whole section.
   - Raise an error with `throw Error("message")`.
8. **Contracts are for bugs only.** `require cond` and `ensure cond` (where
   `result` is the returned value) come first in a function body. A failed
   contract stops the program, and code can't catch it. Anything a correct
   caller could cause (bad input, not enough money, a network error) uses
   `throw`.
9. **Tests.** `expect cond`. To check a failure, use
   `let err = expect throws f(x)`. That also catches a failed `require`. Then
   `expect err.message.contains("…")` or `expect err is NotFound(_)`.
10. **Immutability.** `let` can't change, `var` can. No shadowing. A record is
    changed by making a copy: `user.with(age: 37)`. A list is changed in
    place only through a `var`: `xs.append(x)` or `xs += [x]`.
11. **Tuples.** `(A, B)` holds two or three values and is only unpacked:
    `let (a, b) = f()`. There's no `t.0` or `t[0]`.
12. **Lambdas.** `x => expr` or `(a, b) => expr`. A lambda that receives a
    tuple unpacks it: `entries().map((k, v) => …)`. For a multi-line lambda,
    indent the lines. The last line is the result.
13. **Interfaces vs generics.** Use the interface as a type directly:
    - `items: List<Describable>` can hold different types:
      `let items: List<Describable> = [user, point]`.
    - Several interfaces at once: `from: Storage + Listable, to: Storage`.
      Here `from` and `to` can have different types.

    Use `<T: Describable>` **only** when the types must be the same.
14. **`if` as a value.** Short form: `if c then a else b`. Long form: `if c` /
    `else` with indented blocks, where the last line of each block is the
    value.
15. **Numbers.** `Int / Int` gives a `Float`. Whole-number division is
    `a.div(b)`. An `Int` widens to `Float`/`Decimal` automatically.
    `Float` and `Decimal` don't mix. Text uses double quotes only.
16. **Parallel work.**
    - Every `let` inside `parallel` runs at the same time.
    - For a list, use `xs.parallel_map(x => …)`.
    - Handle an error in one branch locally with `try f() catch none`.
    - Don't use `return`, `break` or `continue` inside `parallel`.
17. **No `await`, `async`, `new`, `null`, `this` or classes.**

## Not this → this

| Not this | This |
|---|---|
| `if x > 0:` / `elif` | `if x > 0` / `else if` |
| `None`, `null`, `nil` | `none` |
| `const x =`, `let mut x` | `let x =` / `var x =` |
| `x === y`, `&&`, `\|\|`, `!x` | `x == y`, `and`, `or`, `not x` |
| `function f() {}`, `def f():` | `fn f()` + an indented block |
| `fn f() -> T throws` | `fn f() throws -> T` |
| `parse_int(t) catch 0` (without `try`) | `try parse_int(t) catch 0` |
| `f()?` (Rust), `f()!` | `try f()` |
| `await fetch(url)` | `try web.get(url)` |
| `new Date()` | `time.now()` |
| `{ it.age }` | `u => u.age` |
| `some(x)` as a value | `x` |
| `t.0`, `pair[1]` | `let (a, b) = t` |
| `for i in 0..n` | `for i in 0..<n` |
| `len(xs)`, `xs.size()` | `xs.length` |
| `push`, `add` on a list | `append` (only on a `var`) |
| `skip` | `drop` |
| `str(x)`, `toString()` | `x.to_text()` or `"{x}"` |
| `int(x)`, `Int(x)` | `try parse_int(x)` |
| `reduce` | `fold(start:, step:)` |
| `includes` | `contains` |
| `'single quotes'` | `"double quotes"` |
| `fn f<T: A>(a: T, b: T)` for different types | `fn f(a: A, b: A)` |

## The most useful standard library functions

**Text:** `length` (a field), `is_empty()`, `lower()`, `upper()`, `trim()`,
`split(sep)`, `split_words()`, `lines()`, `contains()`, `starts_with()`,
`replace(old:, new:)`, `slice(from:, to:)`, `pad_start(width:, fill:)`,
`bytes()`.

**List:**
- Reading: `length` (a field), `is_empty()`, `first()`, `last()`, `get(i)`,
  `[i]`, `contains()`, `find()`, `any()`, `all()`, `count()`.
- Transforming: `map`, `filter`, `parallel_map`, `sort()`, `sort_by(key)`
  (ascending; add `.reverse()` for descending), `take(n)`, `drop(n)`,
  `indexed()`, `zip()`.
- Summarizing: `sum()`, `min()`, `max()`, `max_by()`, `average()`,
  `fold(start:, step:)`, `group_by(key) -> Map<K, List<T>>`,
  `count_each() -> Map<T, Int>`, `join(sep)`.
- Changing a `var` in place: `append`, `insert(item:, at:)`, `remove_at`,
  `pop`, `clear`.

**Map:** `m[key] -> V?`, `contains_key()`, `keys()`, `values()`,
`entries() -> List<(K, V)>`, `for (k, v) in m`, `m[key] = v` (on a `var` or
a resource field), `remove(key)`.

**Optional values:** `??`, `?.`, `is some(v)`, `is none`,
`try x.or_throw("message")`.

**Modules:**
- `files`: `read`, `write`, `read_bytes`, `write_bytes`, `exists`, `list`,
  `delete`.
- `web`: `get(url)`, `post(url, body)`.
- `json`: `decode<T>(text)`, `encode(v)`.
- `time`: `now()`, `today()`, `t.format("yyyy-MM-dd")`.
- `env`: `get(name)`.

**Interop:**
- **C:** an `extern c "lib"` block declares functions whose names match the C
  names **exactly**. `opaque T closed by f` declares a C pointer, and `out`
  marks an output parameter. C calls don't throw. Wrap them in a small module
  with an ordinary Linen API.
- **JavaScript (only with `--target js`):**
  - `import js "pkg"` makes the package available under its name, with `-`
    replaced by `_`.
  - `extern js "pkg"` declares functions under their exact JavaScript names.
  - Foreign calls need `try`, and `any` arrives as `Dynamic`, which you turn
    into a type with `try decode<T>(raw)`.
