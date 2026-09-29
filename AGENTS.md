# Writing lang: a guide for agents

Everything needed to write correct code on the first try. The standard
library reference is [`STDLIB.md`](STDLIB.md); the reasons behind the rules
are in [`DESIGN.md`](DESIGN.md).

## Files and programs

- A file is a module. A program is a file with `fn main()` (or
  `fn main() throws`). Run it: `lang run app.lang`. Tests: `lang test app.lang`.
- `import json`, `import store.users` (the file `store/users.lang` from the
  project root). Use names with the module prefix: `json.decode<T>(text)`,
  `users.find(id)`. No `from`, no `*`, no relative paths.
- Without `pub`, a function or type is private to its file.
- A local variable can't have the name of an imported module (`let path = ...`
  with `import path` is an error).

## Syntax at a glance

```
// comments with //
let x = 5                      // constant
var total = 0                  // variable
total += x
let name: Text = "Ada"         // types: Int, Float, Bool, Text, Bytes,
                               // List<T>, Map<K, V>, Set<T>, T?
let greeting = "Hi ${name}!"   // interpolation is ${...}
let ok = a > 0 and not done or b // and / or / not, never && || !

if x > 3 {
  ...
} else if x == 3 {
  ...
} else {
  ...
}

for item in items { ... }
for i in 0..<10 { ... }        // 0..<n excludes n, 1..=n includes it
while running { ... }
for entry in map { print("${entry.key}: ${entry.value}") }
for item in list.indexed() { print("${item.index}: ${item.value}") }

let size = if n > 100 { "big" } else { "small" }   // if and match give values
let label = match shape {
  Circle(radius) => "circle ${radius}"
  Rect(width, height) => "rect"
}
```

No semicolons. Braces always. `if` conditions have no parentheses.

## Types

```
type User {                    // a record: a value, copied on assignment
  id: Int
  name: Text
  email: Text? = none          // optional, with a default
  active: Bool = true          // default: may be left out when building
}
let u = User(id: 1, name: "Ada")   // fields are always named

enum Status {                  // variants, with or without data
  Draft
  Published(at: Text)
}
let s = Published(at: "today")     // variants of your own enums: bare names
let v = json.Value.Null            // from another module: module.Enum.Variant

type UserId = Int              // a NEW type, not an alias: UserId(5), id.value

type User implements Describable { ... }   // interfaces are explicit

interface Describable {
  fn describe(self) -> Text
}
```

- Methods go inside the type: `fn describe(self) -> Text { ... }`. A method
  that changes the value is `mutating fn add(x: Int) { self.items.append(x) }`
  and needs a `var`.
- No classes, inheritance, overloading, operator overloading, tuples or
  default parameter values. Several results make a record. Many options
  make an options record with defaults.
- Generics: `fn first<T>(xs: List<T>) -> T?`. No bounds.

## Missing values

```
let email: Text? = user.email
let shown = email ?? "(none)"                 // a fallback
if email is some(e) { send(e) }               // unwrap
if email is none { return }
let user = users[id] ?? throw NotFound(id: id)
```

**There is no `?.`**. Unwrap first with `if x is some(v)`, or use `??`.
`m[key]` gives `V?`; `xs[i]` out of range is a bug (it stops the task).

## Errors

```
fn load(path: Text) throws -> Config {          // can fail: `throws`
  let text = try files.read(path)               // every failing call: `try`
  return try json.decode<Config>(text)
}

let text = try files.read(path) catch err {     // handle it here
  if err is NotFound { return none }            // flow typing after `is`
  throw Failure(message: "can't load ${path}", cause: err)
}
```

- `catch` always follows `try`: `try f() catch err { ... }`. Never
  `f() catch ...`.
- A `catch` block ends with a value, or with `return`, `throw`, `break` or
  `continue`.
- `try` covers the whole expression after it: `try (try f()).text()` is
  needless; `try f().text()` checks both calls.
- Your own errors: `type NotFound implements Error { id: Int  fn message(self)
  -> Text { return "no ${self.id}" } }`, or `Failure(message: "...")` from
  the prelude.
- Bugs (`panic("...")`, `assert`, overflow, a bad index) can't be caught.

## Functions

```
fn price(item: Item, count: Int) -> Float { ... }
price(item, count: 3)            // with 3+ parameters, name every argument
                                 // after the first: f(a, b: 1, c: 2)
xs.map(x => x * 2)               // lambdas: x => expr, or x => { ... }
xs.map(x => try parse(x))        // a failing lambda needs `try` inside,
                                 // and the call is `try xs.map(...)`
```

- **A function never changes its arguments.** Return the new value and
  assign it: `items = add_item(items, item: x)`.
- **Results must be used.** A call whose result you don't need is written
  `let _ = f()`. `list.sort()` changes the list in place; `list.sorted()`
  returns a new one.
- No `return` inside a lambda: its last line is its value.

## Numbers

- `Int` is 64-bit and `Float` is 64-bit. There are no implicit conversions:
  `n.to_float()`, `f.round()`, `f.floor()`, `f.ceil()`.
- **`/` on two `Int`s is an error.** Write `a.div(b)` (whole numbers) or
  `a.to_float() / b.to_float()`.
- Bits: `a.bit_and(b)`, `bit_or`, `bit_xor`, `shift_left`, `shift_right`.
- Text to numbers: `try text.to_int()`, `try text.to_float()`.

## Resources: `with`

Files, connections, processes and anything else with a `close()` must be
opened with `with`. It's closed at the end of the block, even on an error.

```
with f = try files.create("out.txt") {
  try f.write_text("hello\n")
}

with conn = try postgres.connect(url) catch err {     // handle failure to open
  log.error(err.message())
  return
} {
  ...
}
```

- `let f = try files.open(p)` is an error: use `with`.
- A record with a `close(self)` method is a resource too. Its fields may
  take other resources: `Client(conn: try net.connect(host, port))`.
- A function may return a new resource (`fn open_db() throws -> db.Connection`).
  To set it up first and close it only on failure, return the `with`
  variable itself:

  ```
  with c = Client(conn: try net.connect(host, port)) {
    try login(c)          // an error closes it
    return c              // success gives it to the caller, open
  }
  ```

## Concurrency

```
let a = spawn fetch(url1)                 // a Task<T>, running in parallel
let b = spawn fetch(url2)
let pages = [try a.wait(), try b.wait()]
let sizes = try urls.parallel_map(limit: 8, transform: u => try fetch(u).length)

let counter = Shared<Int>(0)              // shared state: only through a lock
with n = counter.lock() {
  n += 1
}
let jobs = Channel<Text>(capacity: 100)   // passing data between tasks
```

A task can't be returned, stored or captured. A function waits for its tasks
before it returns. No `async`/`await`: waiting code is written sequentially.

## Data in and out: `decode<T>`

```
let user = try json.decode<User>(body)          // missing fields: an error
                                                // naming the field
let text = json.encode(user)
let config = try env.decode<Config>()           // CONFIG fields from env
let opts = try cli.decode<Options>()            // --flags, --help generated
let rows = try conn.query<User>("select id, name from users where age > ?", [18])
```

SQL is always written right in the call, with `?` (SQLite) or `$1`
(Postgres) for values. Building SQL from text is a compile error. Parameters
are plain values: `[name, 36, true]`.

## HTTP

```
fn get_note(req: http.Request) throws -> http.Response {
  let id = try (req.param("id") ?? "").to_int()
  return http.json(200, try load_note(id))
}

var router = http.Router()
router.get("/notes/:id", get_note)
router.files("/static", dir: "public")
try http.serve(router, port: 8080)

let res = try http.get("https://example.com")               // client
let api = try http.send(http.ClientRequest(url: u, method: "POST",
  headers: {"authorization": "Bearer ${key}"}, body: json.encode(x).bytes()))
```

Bodies are `Bytes`: `try req.text()`, `try res.text()`. A handler's error
becomes a 500 and a log line.

## Tests

```
test "discount never goes below zero" {
  expect apply(Fixed(50.0), total: 30.0) == 0.0
}
```

`expect` prints both sides on failure. Tests go in the same file and can
see private functions.

## Common compiler errors and their fixes

| error | fix |
|---|---|
| `a \`Stream\` must be closed: get it with \`with\`` | `with f = try ... { }` |
| `/` on two `Int`s is not allowed | `a.div(b)` or `a.to_float() / b.to_float()` |
| there is no `?.` | `if x is some(v) { v.field }` or `??` |
| `catch` needs `try` before the call | `try f(x) catch err { ... }` |
| this call can fail | put `try` in front |
| the result is not used | `let _ = f()` |
| `f` has 3 or more parameters: name every argument after the first | `f(a, b: 1, c: 2)` |
| the SQL must be written right here | put values in the parameter list |
| expected `Text`, found `Int` | `"${n}"` or `n.to_text()` |
