# Language cheat sheet

A statically typed language for CLI tools, scripts and backend services.
Files end in `.lang`. This page is everything you need to write correct code.

**Blocks use braces** `{ }`. There are no semicolons; a statement ends at the
end of the line. `lang fmt` indents with 2 spaces.

## Shape of a program

```
import files
import json

// A record. Records are values: assigning or passing one makes a copy.
pub type User {
  id: UserId
  name: Text
  email: Text? = none          // optional field with a default
}

type UserId = Int              // a NEW type, not an alias: UserId and Int don't mix

enum Shape {
  Circle(radius: Float)
  Rectangle(width: Float, height: Float)
}

interface Describable {
  fn describe(self) -> Text
}

type Point implements Describable {
  x: Int
  y: Int

  fn describe(self) -> Text {
    return "(${self.x}, ${self.y})"
  }
}

fn area(shape: Shape) -> Float {
  return match shape {
    Circle(r) => 3.14159 * r * r
    Rectangle(w, h) => w * h
  }
}

fn load(path: Text) throws -> User {
  let text = try files.read(path)
  return try json.decode<User>(text)
}

test "rectangle area" {
  expect area(Rectangle(width: 3.0, height: 4.0)) == 12.0
}

fn main() throws {
  let user = try load("user.json")
  print("hello, ${user.name}")
}
```

A program starts at `fn main()`. There is no top-level code.

## Rules

### Values and names

- `let` can't change. `var` can. No shadowing: a name is declared once per
  scope.
- **Everything is a value.** `var b = a` makes an independent copy. Changing
  `b` never changes `a`. This holds for records, lists and maps.
- Change a field of a record stored in a `var`: `user.name = "Ada"`.
  Change it inside a collection **through the path**: `users[0].name = "Ada"`,
  `orders[id].items.append(item)`.
  ```
  var u = users[0]
  u.name = "Ada"        // ERROR: changes a copy that is never used. Write users[0].name = "Ada"
  ```
- The only shared mutable state is `Shared<T>` (see Concurrency).

### Types

- **Records:** `type Name { field: Type ... }`. Build them with **named**
  fields: `User(id: UserId(1), name: "Ada")`. Fields with defaults can be
  left out.
- **Enums:** variants with or without data. Build with named fields:
  `Circle(radius: 2.0)`. A variant without data: `Status.Draft` or just
  `Draft` when the type is known.
- **New types:** `type UserId = Int`. Build: `UserId(42)`. Unwrap: `id.value`.
  There are no plain aliases.
- **Generics:** `type Stack<T> { items: List<T> = [] }`,
  `fn first_or<T>(xs: List<T>, fallback: T) -> T`. Type parameters have
  **no bounds**. `==`, hashing and `sort_by` work for every type anyway.
- **Interfaces:** declared with `interface`. A type implements them
  **explicitly**: `type Point implements Describable, Other { ... }`. Methods
  live inside the type body.
- **Interface as a type** = "any type that implements it, and they can
  differ": `let items: List<Describable> = [point, user]`. Several interfaces
  at once: `source: Storage + Listable`.
- **Equality** `==` is structural and automatic for every type except
  functions and handles. Any record can be a `Map` key or in a `Set`.
- **Ordering** `<`, `>` exists only for numbers, `Text`, `Instant` and `Date`.
  Sort anything else with `sort_by(x => x.age)`.
- **No:** classes, inheritance, overloading, operator overloading, default
  parameter values, tuples, type aliases.
- **Several results:** return a record.

### Missing values

- `T?` is a value that may be missing. The empty value is `none`. A plain `T`
  is accepted wherever `T?` is expected: `return user`, never `some(user)`.
- Tools: `x ?? fallback`, `if x is some(v) { ... }`, `match x { some(v) => ...
  none => ... }`.
- There is no `?.`. Unwrap first.

### Methods

- A method is declared inside its type's body. `x.f()` finds `f` only in the
  type of `x`. You can't add methods to someone else's type. Anything else is
  a plain function: `format_price(p)`.
- `self` is always the first parameter. A method that changes its receiver
  declares `var self` and can only be called on a `var`:
  ```
  type Counter {
    count: Int = 0
    fn increment(var self) {
      self.count += 1
    }
  }
  var c = Counter()
  c.increment()
  ```

### Functions

- Signatures always spell out their types: `fn f(a: Int, b: Text) -> Bool`.
  No return type means no result.
- **Argument names:** with 1–2 parameters call positionally. With 3 or more,
  name every argument after the first: `move(book, from: shelf, to: box)`.
  The receiver of `x.f(...)` counts as the first:
  `text.replace(old: "-", new: "_")`.
- **Changing an argument:** the parameter is `var`, and the caller writes `&`:
  ```
  fn add_tag(tags: var List<Text>, tag: Text) {
    if not tags.contains(tag) {
      tags.append(tag)
    }
  }
  var tags = ["new"]
  add_tag(&tags, "urgent")
  ```
- **Results must be used.** Ignoring a return value is a compile error. If you
  really don't need it: `let _ = f()`. So `xs.sorted()` alone on a line is an
  error: you meant `xs.sort()`.
- **Lambdas:** `x => x * 2`, `(a, b) => a + b`, `() => work()`. A multi-line
  lambda is a block; its last line is the result:
  ```
  let labels = users.map(u => {
    let n = u.name.upper()
    "${n} <${u.email ?? "-"}>"
  })
  ```
  Lambdas capture variables **by copy**. Changing a captured `var` is an
  error.
- **Function types:** `fn(Int) -> Text`, `fn(T) throws -> R`, `fn()`.
- A named function can be passed as a value: `prices.map(round_price)`.

### Control flow

```
if age >= 18 {
  ...
} else if has_guardian {
  ...
} else {
  ...
}

for user in users { ... }
for i in 0..<count { ... }            // excludes count
for n in 1..=10 { ... }               // includes 10
for item in names.indexed() { print("${item.index}: ${item.value}") }
for entry in ages { print("${entry.key} is ${entry.value}") }   // Map iteration
while fuel > 0 { fuel -= 1 }
```

- A bare `..` doesn't exist. `break` and `continue` work as usual.
- `if` and `match` are expressions. The value is the last line of each block:
  ```
  let fee = if member { 0 } else { 5 }
  ```
  There is no `a ? b : c` and no `if … then … else`.
- `match` must cover every case. `_ =>` is the catch-all. A guard:
  `Circle(r) if r > 10.0 => "big"`. A multi-line arm uses a block:
  `Circle(r) => { ... }`.
- `x is Pattern` is a `Bool`: `if shape is Circle(r) { ... }`.
- Operators: `and`, `or`, `not`. `==`, `!=`. There is no `&&`, `||`, `!`.

### Numbers

- `Int` (64-bit, overflow stops the program), `Float`, `Decimal` (exact, for
  money).
- **No implicit conversions**, not even `Int` to `Float`:
  `count.to_float()`, `price.to_decimal()`, `x.to_text()`.
- **`/` on two `Int`s is a compile error.** Write `a.div(b)` (whole-number
  division) or `a.to_float() / b.to_float()`. `%` is the remainder.
- A literal takes the expected type: `let price: Decimal = 19.99`.
- `f.round()` gives an `Int`. `d.round(2)` rounds a `Decimal` to 2 places.
  `x.pow(2)`, `x.abs()`.
- Parsing text: `try text.to_int()`, `try text.to_float()`,
  `try text.to_decimal()`.

### Text

- Double quotes only. Interpolation: `"${name} is ${age}"`, any expression
  inside `${}`.
- There is no `+` on text. Use interpolation, or `parts.join(", ")`.
- Multi-line: `"""` … `"""`.

### Errors

- A function that can fail declares `throws` **before** `->`:
  `fn load(path: Text) throws -> User`.
- **Every** call to a `throws` function starts with `try`.
- `Error` is an interface: `fn message(self) -> Text`. Any record can
  implement it. The built-in `Failure(message: Text, cause: Error? = none)`
  is for simple cases.
- Two forms only:
  ```
  let text = try files.read(path)                 // pass the error up

  let port = try raw.to_int() catch err {         // handle it here
    8080
  }

  let user = try load(id) catch err {
    if err is NotFound(_) {
      return none
    }
    throw Failure(message: "loading user ${id}", cause: err)
  }
  ```
  The `catch` block ends with a value, or with `return`, `break`, `continue`
  or `throw`. To rethrow an unrecognized error, `throw err`.
- `throw NotFound(id: id)` raises an error.
- **Bugs** (`assert(cond)`, `panic("…")`, overflow, `xs[i]` out of range)
  can't be caught. A bug stops the current task (an HTTP request gets a 500)
  or the whole CLI program. Use `throw` for anything a correct caller could
  cause.
- If `main` throws, the program prints the message and exits with code 1.

### Resources: `with`

Things that must be closed (open files, database connections, task groups,
temporary directories) can **only** be obtained with `with`. They close at
the end of the block, even on an error:

```
with conn = try db.open(config.database_url) {
  let users = try conn.query<User>("select * from users", [])
}
```

`let conn = try db.open(...)` is a compile error. The `try` on the `with`
line covers both opening and closing.

### Concurrency

No `async`/`await`. Concurrency is visible where it starts:

```
with group = try tasks.group() {
  let user = group.run(() => try load_user(id))          // Task<User>, starts now
  let orders = group.run(() => try load_orders(id))      // Task<List<Order>>
  show(try user.wait(), try orders.wait())
}
// the block doesn't exit until every task has finished
```

- `group.run(f)` returns a `Task<T>`. `try task.wait()` returns its result or
  throws its error. `group.start(f)` starts a task that returns nothing.
- If a task throws, the others are cancelled and the `with` line throws that
  error. To keep going when one task fails, handle the error **inside** the
  task: `group.run(() => try fetch(x) catch err { none })`.
- No `return`, `break` or `continue` inside a task's lambda.
- For a list: `try urls.parallel_map(limit: 10, transform: url => try http.get(url))`.
- Tasks share mutable state **only** through `Shared<T>`:
  ```
  let hits = Shared<Int>(0)
  hits.update(n => n + 1)
  let now = hits.get()
  ```
- Queues: `let jobs = Channel<Job>(capacity: 100)`, `try jobs.send(job)`,
  `for job in jobs { ... }` receives until `jobs.close()`.
- Timeouts: `try time.timeout(time.seconds(5), () => try http.get(url))`
  throws `Timeout`.
- A waiting operation in a cancelled task throws `Cancelled` by itself.

### Modules

- One file is one module. Everything is private unless marked `pub`.
- `import json` then `json.decode(...)`. Always qualified. No
  `from x import y`, no `*`.
- Standard modules need `import`: `files`, `json`, `http`, `db`, `cli`, `env`,
  `time`, `tasks`, `process`, `log`. The prelude (`print`, `min`, `max`,
  `assert`, `panic`, `Failure`, `Shared`, `Channel`, `Decimal`) doesn't.

### Tests

```
test "a fixed discount never goes below zero" {
  expect apply(Fixed(amount: 50), 30) == 0
}

test "loading a missing file fails" {
  let err = expect throws load("nope.json")      // no try here
  expect err is NotFound(_)
}
```

- One check form: `expect cond`. On failure both sides are printed.
- `expect throws f(x)` replaces `try` and returns the error.
- Tests live in the same file as the code and can see private functions.
- There are no mocks. Anything you want to replace in a test is passed as a
  parameter of an interface type.

## Not this → this

| Not this | This |
|---|---|
| `None`, `null`, `nil` | `none` |
| `some(x)` as a value | `x` |
| `a?.b` | `if a is some(v) { v.b }` or `match` |
| `&&`, `\|\|`, `!x` | `and`, `or`, `not x` |
| `fn f() -> T throws` | `fn f() throws -> T` |
| `f()?`, `await f()` | `try f()` |
| `"a" + b` | `"a${b}"` |
| `"{x}"`, `f"{x}"` | `"${x}"` |
| `a / b` with two `Int`s | `a.div(b)` or `a.to_float() / b.to_float()` |
| `int(x)`, `parseInt(x)` | `try x.to_int()` |
| `str(x)`, `toString()` | `x.to_text()` or `"${x}"` |
| `len(xs)`, `xs.size()` | `xs.length` |
| `push`, `add` | `append` |
| `t.0`, `(a, b) = f()` | return a record |
| `for i in 0..n` | `for i in 0..<n` |
| `user.copy(age: 3)`, `{...user, age: 3}` | `var u = user` then `u.age = 3` |
| `class`, `extends` | `type` + `implements` |
| `fn f<T: Describable>(xs: List<T>)` for mixed types | `fn f(xs: List<Describable>)` |
| `let f = open(...)` + `close()` | `with f = try files.open(...) { }` |
| `go f()`, `spawn`, `Promise.all` | `with group = try tasks.group() { group.run(...) }` |
| `xs.sorted()` alone on a line | `xs.sort()` |
| `x ? a : b` | `if x { a } else { b }` |

## Standard library

**Prelude:** `print(text)`, `min(a, b)`, `max(a, b)`, `assert(cond)`,
`panic(text)`.

**Text:** `length` (a field), `is_empty()`, `lower()`, `upper()`, `trim()`,
`split(sep)`, `lines()`, `words()`, `contains(t)`, `starts_with(t)`,
`ends_with(t)`, `replace(old:, new:)`, `slice(from:, to:)`,
`pad_start(width:, fill:)`, `to_int()`, `to_float()`, `to_decimal()` (these
three throw).

**List<T>:**
- Read: `length` (a field), `is_empty()`, `xs[i]` (a bug if out of range),
  `first()` / `last()` (give `T?`), `contains(x)`, `find(f) -> T?`, `any(f)`,
  `all(f)`, `count(f)`.
- New list: `map(f)`, `filter(f)`, `sorted()`, `sorted_by(key)`,
  `reversed()`, `take(n)`, `drop(n)`, `indexed()`, `concat(other)`.
- Summaries: `sum()`, `min()`, `max()` (give `T?`), `max_by(key)`,
  `min_by(key)`, `fold(start:, step:)`, `join(sep)`,
  `group_by(key) -> Map<K, List<T>>`, `count_each() -> Map<T, Int>`.
- Change in place (only on a `var`): `append(x)`, `append_all(xs)`,
  `insert(item: x, at: i)`, `remove_at(i)`, `pop() -> T?`, `sort()`,
  `sort_by(key)`, `reverse()`, `clear()`.
- Empty list with a type: `var xs: List<Int> = []`.

**Map<K, V>:** literal `{"ada": 36}`, empty `var m: Map<Text, Int> = {}`.
`m[key] -> V?`, `m[key] = v` (on a `var`), `contains_key(k)`, `keys()`,
`values()`, `entries()` (records with `key`, `value`), `remove(k)`, `length`.
**Set<T>:** `Set<Text>()`, `add(x)`, `contains(x)`, `remove(x)`, `length`.

**Modules:**
- `files`: `read(path) -> Text`, `write(path, text)`, `append(path, text)`,
  `exists(path) -> Bool` (doesn't throw), `list(dir) -> List<Text>`,
  `delete(path)`, `open(path)` (use with `with`; `f.lines()` reads line by
  line), `temp_dir()` (use with `with`; `dir.path`). All throw except
  `exists`.
- `json`: `decode<T>(text)` (throws), `encode(value) -> Text`. Field names
  map exactly; `json.decode<T>(text, keys: json.CamelCase)` maps `user_name`
  ↔ `userName`.
- `http` client: `get(url) -> http.Response`, `post(url, body)`. Both throw.
  `response.status: Int`, `response.body: Text`.
- `http` server:
  ```
  fn get_note(req: http.Request) throws -> http.Response {
    let id = try (req.param("id") ?? "").to_int()
    ...
    return http.json(200, note)             // or http.text(404, "not found")
  }

  var router = http.Router()
  router.get("/notes/:id", get_note)
  router.post("/notes", create_note)
  try http.serve(router, port: 8080)          // runs until stopped
  ```
  `req.method`, `req.path`, `req.body: Text`, `req.param(name) -> Text?`,
  `req.query(name) -> Text?`. An error thrown by a handler becomes a 500.
  In tests: `let res = try router.handle(http.Request(method: "GET", path: "/notes/1", body: ""))`.
- `db`: `with conn = try db.open(url)` (`"sqlite::memory:"`,
  `"sqlite:app.db"`, `"postgres://..."`).
  `try conn.query<T>(sql, params) -> List<T>` (columns map to fields by name),
  `try conn.execute(sql, params) -> Int` (rows changed).
  **`sql` must be a string literal**, with `?` placeholders. `params` is a
  list whose items can mix `Int`, `Float`, `Decimal`, `Text`, `Bool`,
  `Instant`, new types over those, and optional values.
- `cli`: `try cli.decode<Options>()`: each field becomes a flag
  (`output_dir` → `--output-dir`). A field with a default or a `T?` field is
  optional. A field `args: List<Text>` collects positional arguments.
  `--help` is generated.
- `env`: `try env.decode<Config>()` (`database_url` ← `DATABASE_URL`),
  `env.get(name) -> Text?`.
- `time`: `now() -> Instant`, `today() -> Date`, `seconds(n)`,
  `millis(n) -> Duration`, `try sleep(duration)`, `try timeout(duration, f)`,
  `instant.format("yyyy-MM-dd HH:mm")`, `a - b -> Duration` for two instants.
- `tasks`: `group()` (with `with`).
- `process`: `try process.run("git", ["log", "-n", "5"])` → `status: Int`,
  `stdout: Text`, `stderr: Text`. There is no shell string.
- `log`: `log.info(text)`, `log.warn(text)`, `log.error(text)`.
