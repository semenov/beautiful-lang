# Design from scratch

A clean-slate redesign, started 2026-09-29. Linen (`drafts/linen/`) is the
previous attempt. None of its features carry over by default: each one
has to be justified again.

## Goal

- **Who:** AI agents write the code. People sometimes read it (review).
- **What:** CLI utilities, scripts, backend services.
- **Speed:** on par with Go. A garbage collector is fine.

## Test for every feature

A feature stays only if it **reduces what a reader has to keep in their
head**, or **rules out a class of bugs**. It has to do so by more than it
costs to learn, and by more than its interactions with other features cost.

What follows from the audience:

1. **Typing is free, ambiguity is expensive.** Keystroke-saving shortcuts
   have no value.
2. **Familiar syntax, strict meaning.** Agents write the "average of their
   training data." The surface looks mainstream, and the meaning is stricter.
3. **One way to write each thing.** A second way becomes a dialect that gets
   mixed within one file.
4. **Many checks, few concepts.** The compiler is the agent's reviewer in a
   loop, so every error must have one obvious fix.
5. **Compiler messages are part of the language.** They suggest the fix.
6. **Signatures tell the reviewer what matters:** whether a function can fail.
   A function never changes its arguments.
7. **A reader understands a construct without knowing the rules.** If a
   construct needs an explanation to be read (like `var` on a parameter), it's
   a candidate for removal.

## Decisions

### Memory: automatic, by reference counting

The programmer never manages memory. There's no `resource`/`weak`, and there
are no cycles. APIs are designed so a handle is rarely needed
(`files.read(path)`, `http.get(url)` open and close by themselves).

Implementation: reference counting inserted by the compiler (like Swift, Koka,
Roc), not a tracing GC (decided during implementation, then confirmed). It
fits the value model:
- Values can't form cycles (there are no references), so counting frees
  everything.
- Copy-on-write comes for free: if the counter is 1, a change happens in
  place, including `x = f(x)`.
- No GC pauses, and memory is freed immediately.
- With many cores, only values passed to another task need atomic counters
  (a flag on the object); a good parallel tracing GC would be years of work.

The only possible cycle is a `Shared` that holds itself, so a `Shared` can't
contain another `Shared` in its value (a compile error).

### Resources: `with`, enforced by the compiler

```
with conn = try db.open(path) {
  let users = try conn.query<User>("select …", [])
}
// conn is closed here, even if an error was thrown
```

A type that needs closing (files, connections, sockets) can only be received
through `with`. `let conn = db.open(...)` is a compile error with the
suggestion "use `with`". A long-lived resource (a pool for the whole server)
goes in a `with` around all of `main`.

- **Your own resources:** a record with a `close(self)` method is a resource
  like a file (the Redis and Postgres clients are such records). Building
  one may take other resources into its fields; it owns them.
- **Handing a resource over:** a function may return a new resource, and
  its caller then needs `with`. To set one up and close it only if the
  setup fails, return the `with` variable itself: `with c = Client(...) {
  try login(c); return c }`. An error closes it; the `return` moves it out
  open. No new keyword, and the reader sees both paths.

### Data: mutable values (Swift-style)

- All user-defined types are values. Assignment and passing are logical
  copies (copy-on-write underneath). A `var` can be changed in place.
- There are no classes and no user-defined reference types.
- Things with identity (connections, files, the HTTP server) are opaque
  handles from the standard library.
- Shared mutable state uses only `Shared<T>`, which synchronizes access.
- Graphs and back-links use IDs in a `Map`, not pointers.
- Changes through access paths (`orders[id].items.append(x)`) happen in
  place.
- Compiler error: "you changed a copy that is never used afterward" (with
  the suggested fix).
- Implementation (copy-on-write with reference counting or persistent
  structures) is decided later.

### Functions never change their arguments

A function that produces a changed value returns it, and the caller assigns
it:

```
products = restock(products, name: "Widget", amount: 3)
```

Any reader understands this line without knowing the rules. There is no `var`
on parameters and no `&`. The compiler may perform `x = f(x)` in place.
Changing a value in place is done by a `mutating` method on your own type:
`inventory.restock(name: "Widget", amount: 3)`.

### Types

- **Flow typing after `is`.** After `if err is InsufficientFunds`, `err` has
  that type inside the block (fields are accessible). After
  `if x is none { throw … }`, `x` is `T` for the rest of the block. A bare
  type test without parentheses (`err is InsufficientFunds`) is allowed.
- **Named function types:** `type Rule = fn(Decimal) -> Decimal` is a new
  type like any other `type X = …`. A lambda takes the expected type (as
  `1.5` becomes a `Decimal` where one is expected; other new types are
  written out: `UserId(7)`), and a `Rule` value is called directly:
  `rule(total)`.

- **Records, enums with data, exhaustive `match`.**
- **Automatic equality and hashing.** `==` and hashing are structural for
  every type except functions and handles. Any record can be a `Map` key or a
  `Set` element. Ordering (`<`) is built in only for numbers, `String`,
  `Decimal`, time (`Duration`, `Date`, `DateTime`), lists of them (element
  by element) and new types over them (`UserId`). Anything else is sorted
  with `sort_by(x => x.key)`.
- **Generics without bounds.** `<T>` means "any type, the same one
  everywhere." It doesn't need bounds: `==`, hashing and `sort_by` work for
  everything.
- **An interface used as a type** means "any type that implements it, and they
  can differ": `List<Describable>`. This doesn't overlap with generics.
- **Interfaces are implemented explicitly:** `type User implements
  Describable`. The compiler checks it.
- **`type UserId = Int` creates a distinct type**, not an alias. Mixing up
  `UserId` and `OrderId` is a compile error. There are no plain aliases.
- **Missing values:** `T?`, `none`, `??`, `if x is some(v)`, `match`, and
  `?.` (Vlad, 2026-09-29: agents kept writing it). `user?.address?.city` is
  none if anything on the way is none; the rest of the chain after `?.` runs
  only on a value, and the result is optional once, not twice. `?.` can't
  change the value inside (it would change a copy): that is an error.
- **Removed:** inheritance, overloading, operator overloading, tuples
  (multiple results are a record; iterating a `Map` gives `entry.key` /
  `entry.value`), default parameter values (record fields keep their
  defaults; a function with many settings takes a record of options).

### Methods belong to their type

- A method is declared together with its type, in the same module. There are
  no extension methods.
- `x.f()` is looked up in exactly one place: the type of `x`. Anything else
  is called as a function: `format_price(p)`.
- A method that changes its receiver is declared `mutating fn increment()`
  and can only be called on a `var`. The receiver isn't marked at the call
  site: the method name says what happens (`xs.append(x)`).
- Standard library chains (`xs.filter(...).map(...)`) work because these are
  methods of `List`.

### Paths through maps

`m[key]` as a value gives `V?`. Changing through a path
(`accounts[id].balance -= x`) is allowed; a missing key is a bug, like
`xs[i]` out of range.

### Results must be used

Ignoring a return value is a compile error. The explicit form for the rare
exception is `let _ = f()`. As a result, a line `x.method(...)` without a
used result is always a mutation or an action. The rule also catches the
`sort()` / `sorted()` confusion.

### Control flow

- Loops: `for x in xs` and `while`. Nothing else.
- Ranges: `0..<n` excludes the end, `1..=n` includes it. A bare `..` is an
  error.
- Indices and map entries are records: `for item in xs.indexed()` →
  `item.index`, `item.value`. `for entry in map` → `entry.key`,
  `entry.value`.
- `if` and `match` are expressions, written only as blocks. The value is the
  last line. There is no one-line `if … then … else`.

### Functions and lambdas

- **Argument names:** the receiver of `x.f(...)` doesn't count. A function
  with 3 or more parameters (besides the receiver) is called with every
  argument after the first named. `router.get(path, handler)` and
  `conn.query(sql, params)` stay positional.
- **No `return` inside a lambda.** The last line is the value.
- A line starting with `.` continues the expression above it (chains).

- Signatures always spell out their types. Inside a body, `let` can infer
  them.
- Lambdas are `x => expr` or an indented block. They capture values by copy.
  Changing a captured `var` is an error.
- `throws` passes through lambdas: `try users.map(u => try load(u))`.
  Higher-order functions declare this once in the standard library.

### Modules and packages

- **One file is one module.** The import path is the file's path from the
  project root, with dots: `import store.users` loads `store/users.plumb`;
  the code uses the last part: `users.find(...)`. Only root-relative paths,
  no `../` or relative imports (one way to write an import).
- An alias only when two imports would have the same name:
  `import billing.users as billing_users` (the compiler asks for it).
- Imports are always qualified: no `from x import y`, no `*`. Every name in
  the code is either declared in this file or has a module prefix.
- **Top-level `let` for fixed values** (`let max_retries = 5`,
  `pub let default_port = 8080`), and no global variables. The value is
  literals, other top-level `let`s, and lists, maps and records of them:
  no function calls, so nothing runs before `main`, and there is no
  initialization order to think about. `let` already means "can't change",
  so there is no second keyword like `const`. Shared state is still a
  `Shared<T>` made in `main`.
- **Private by default:** without `pub`, a function, type, method or
  top-level `let` is visible only inside its file. Fields are visible wherever the type is.
- **No cyclic imports:** the error shows the cycle (`a → b → a`) and
  suggests moving the shared part into a third module.
- The main file can't be imported.
- **Where a name comes from:** the standard library first, then packages
  from `plumb.toml`, then the project's files.
- **The project root** is the nearest directory with `plumb.toml`; without
  one, the directory of the file being run.
- **Packages are git repositories** (like Go modules; no central registry
  for now). `plumb.toml` lists them with a version tag:

  ```toml
  [package]
  name = "shop"
  version = "0.1.0"

  [dependencies]
  router = { git = "https://github.com/someone/router", version = "v1.2.0" }
  ```

  A package may also be a directory inside a repository (`path =
  "packages/redis"`), so one repository holds several, or a directory on
  this disk (`path` without `git`) while developing it.

  `plumb add <name> <git-url> --version <tag>` adds one; `plumb.lock` pins the
  exact commit of every package (including the packages' own
  dependencies), so builds are reproducible. Packages are downloaded once
  into `~/.plumb/packages/<name>/<commit>/`. `plumb fetch` downloads what the
  lock file pins; `plumb update` moves to the newest commits of the tags.
- **A package's main module** is the file named after the package
  (`router/router.plumb` → `import router`); its other files are
  `import router.middleware`. One version of each package per build: two
  different sources for the same name are an error.
- `plumb init` makes the current directory a project (`plumb.toml`, and
  `main.plumb` unless there is one); the name is the directory's.

### Numbers

- `Int` is 64-bit. Overflow stops the program.
- `Float` is 64-bit IEEE.
- `Decimal` is a built-in type for money: exact, up to 18 digits with a
  scale that's kept (`1.50` prints as `1.50`, and `1.50 == 1.5`). `+ - *`
  are exact; `/` is an error, because the result's digits must be chosen:
  `a.div(b, places: 2)`. Rounding is half away from zero (what Excel and
  SQL do, and what people expect on invoices). A literal is spelled as
  written (`19.99`, not the closest Float). JSON and SQL carry it exactly.
- No implicit conversions, including `Int → Float`.
- `/` on two `Int`s is a compile error. You write `a.div(b)` or
  `a.to_float() / b.to_float()`.
- A number literal takes the expected type: `let price: Decimal = 19.99`.

### Errors: checked, untyped `throws`

- A function that can fail says `throws` (without a list of types). Every call
  to it starts with `try`.
- `Error` is an interface (`fn message(self) -> String`) that any record can
  implement. Handling checks the type: `if err is NotFound`.
- **`try` covers the whole expression after it** (like Swift): in
  `try encoding.from_hex(t).text()` both calls may fail. The reader still
  sees at the start of the line that something in it can fail. A `try`
  over an expression where nothing can fail is an error.
- Two forms only:

  ```
  let text = try files.read(path)          # pass it up

  let user = try load(id) catch err        # handle it here
    if err is NotFound
      return none
    throw Wrapped("loading user {id}", cause: err)
  ```

  The `catch` block ends with a value, or with `return`, `break`, `continue`
  or `throw`. The same applies to `match` arms and the right side of
  `??`: `let user = users[id] ?? throw NotFound(id: id)`.
- An implementation may declare less than its interface: a method that never
  fails doesn't have to be `throws`. Nothing is swallowed silently. `cause` gives a chain of context
  for logs.

### Bugs: stop at the task boundary

- `assert`, overflow, an out-of-range `xs[i]` and `panic("…")` are bugs. Code
  can't catch them.
- A bug stops the nearest task. In a server that means one request gets a 500
  and a log entry, and the server keeps running. In a CLI tool it means the
  whole program. The runtime and the standard library set the boundaries.
- `m[key]` returns `V?` (a missing key is normal). `xs.first()` returns
  `T?`.

### Concurrency: sequential code plus `spawn`

- No `async`/`await` (no function coloring). Code that waits (network,
  database) is written sequentially; an HTTP server runs each request as its
  own task, and waiting doesn't block other requests.
- Parallel work is started with `spawn`, which returns a `Task<T>`:

  ```
  fn dashboard(id: Int) throws -> Dashboard {
    let user = spawn load_user(id)
    let orders = spawn load_orders(id)
    return Dashboard(user: try user.wait(), orders: try orders.wait())
  }
  ```

- **A task belongs to the function that started it.** The function doesn't
  finish until all its tasks have finished, so tasks can't leak.
- **A `Task` can't leave the function:** it can't be returned, stored in a
  record or captured by a lambda (compile errors). Local variables and lists
  are fine: `pending.append(spawn fetch(url))`.
- **Errors aren't lost.** An error arrives in `try task.wait()`. If a task
  fails and nobody waits for it, the other tasks are cancelled and the error
  comes out of the function, so `spawn` of a failing call requires `throws`.
- **Results must be used:** a `Task<T>` nobody waits for is an error.
  `spawn f()` for a function without a result is a plain statement (a
  background worker).
- **At the end of the function, unfinished tasks are waited for**, not
  cancelled: nothing is lost silently. A background worker is stopped
  explicitly, e.g. by closing its queue. On an early exit (`return` or an
  error before `wait`) unfinished tasks are cancelled, then waited for.
- For lists: `urls.parallel_map(limit: 10, transform: url => try fetch(url))`.
- **All cores are used:** tasks run on a pool of threads (like Go), so `spawn`
  and `parallel_map` speed up computation too, not only waiting.
- Cancellation is automatic: a waiting operation in a cancelled task throws
  `Cancelled`.
- Tasks share data only through `Shared<T>` or pass it through `Channel<T>`.
- Timeouts: `try time.timeout(time.seconds(5), () => try http.get(url))`
  runs the work as a task and cancels it at the deadline (its own tasks
  too); every wait is cancellable, including sockets and HTTP requests.
- **Ctrl-C cancels `main`'s task.** Waits stop with `Cancelled`, `with`
  blocks close their resources, and the program ends with 130. A second
  Ctrl-C ends it at once. A loop that never waits checks
  `process.interrupted()`. So cleanup on Ctrl-C needs nothing new: it's the
  same path as any error.
- **Rejected:**
  - a `parallel` block where every line runs at the same time (it looks
    sequential but isn't);
  - task groups through `with` (`tasks.group()`, `group.run`): `spawn` gives
    the same guarantees with one keyword instead of a group object;
  - `async`/`await`.

### Tests and tooling

- `test "name"` blocks live next to the code, in the same file, and can see
  private functions. `plumb test` runs everything, with no configuration.
- One check form, `expect cond`. On failure, the compiler prints the value of
  each side of the expression (`left: 0`, `right: 5`). No `assertEqual`
  family.
- `let err = expect throws f(x)` returns the error for inspection. `assert`
  and `panic` can't be tested: they mark bugs, not errors.
- Tests run in parallel. They're isolated automatically, because there's no
  global mutable state. External things use `with` and temporary resources
  (`files.temp_dir()`).
- No mocking framework (no reflection). Anything replaceable is passed as an
  interface-typed parameter (`Clock`, `Mailer`). The standard library ships
  test implementations: a fixed clock, an in-memory file system, calling an
  HTTP handler without a network.
- One binary: `plumb run`, `plumb build`, `plumb test`, `plumb fmt`, `plumb doc`,
  `plumb guide`. The formatter has no settings.
- **The tool teaches the language.** An agent that has never seen it is
  told "run `plumb help`": `plumb guide` is the whole language in one read
  (with fixes for the common errors), and `plumb doc http.Router` or
  `plumb doc --search gzip` look things up in the sources that are running.
  Tested with fresh agents: they wrote working programs on the first
  compile.

### Standard library

- **Batteries included for the domain.** A typical CLI tool or backend is
  written without third-party packages.
- **The most predictable names.** When mainstream languages disagree, pick
  the one agents are most likely to guess. One name per concept. The compiler
  suggests the closest match.
- **All external data enters through `decode<T>`.** The compiler generates
  the conversion into a record:

  ```
  let opts = try cli.decode<Options>()     # flags from fields, generated --help
  let config = try env.decode<Config>()
  let user = try json.decode<User>(body)
  let users = try conn.query<User>("select * from users where age > ?", [18])
  ```

  A missing required field is an error that names the field. A `T?` field or
  a field with a default is optional. There are no annotations: name mapping
  is a decode option (`keys: CamelCase`). Anything unusual is decoded by hand
  through `json.Value`. Its numbers are kept as written (`json.Number`:
  `1.50`, `1e5` and 20-digit ids survive a round trip) and converted when
  asked (`n.to_int()`, `n.to_float()`, `n.to_decimal()`), as Go's
  `UseNumber`; decoding into a Float that can't hold a number is an error.
- **SQL:** the query text must be a string literal, and parameters are passed
  separately. Concatenation or interpolation is a compile error. The rule
  lives in the `sql` module (`sql.Query` accepts only a literal; `sql.Value`
  takes plain values), so database packages get it too, not only the
  built-in SQLite `db`.
- **Streams:** everything read or written piece by piece is one type,
  `io.Stream`: files, sockets, standard input/output, gzip files, HTTP
  bodies. Same methods everywhere (`read`, `read_line`, `read_all`,
  `write`, `write_text`), so there's nothing to learn per source. There is
  no stream framework (pipes, transforms): `io.copy` and a loop cover it.
- **Designed for "results must be used":** `conn.execute(sql, params)`
  returns nothing (`execute_counting` returns the row count), `map.remove(k)`
  returns nothing (`map.take(k) -> V?` removes and returns). A result is
  returned only when it's usually needed.
- **Shared state: `Shared<T>` is accessed only through a lock with `with`:**

  ```
  with views = app.views.lock() {
    views[id] = (views[id] ?? 0) + 1
  }
  ```

  Inside the block the value is changed like an ordinary variable; other
  tasks wait at the entrance; the lock is released at the end of the block,
  even on an error. There is no `update`. A `lock()` directly inside another
  `lock()` is a compile error. A deadlock through function calls is
  reported at runtime only when every task waits and nothing else (no
  timer, no socket) could wake one; in a server, a lock cycle between two
  requests just hangs them, so keep one lock at a time.
  Readers use `with v = s.read() { ... }`: `v` can't be changed, and
  readers run at the same time (a readers-writer lock; a read-mostly cache
  is the most common shared state in a backend). Readers count themselves
  per core, so they don't contend on one word; a waiting writer holds back
  new readers, and the readers that waited go before the next writer.
- **Processes:** `process.run("git", ["log", "-n", "5"])` takes a list of
  arguments, not a shell string.
- **Time:** `Instant` and `Date` are different types. Time zones are always
  explicit.
- **Modules:** `files`, `path`, `io`, `process`, `env`, `cli`, `log`,
  `time`, `json`, `http` (client and server), `net` (TCP, UDP, TLS), `sql`,
  `db` (SQLite), `crypto`, `encoding`, `random`, `regex`, `csv`, `xml`,
  `url`, `zlib`, `math` (reference: `STDLIB.md`).
- **What goes in the standard library:** what most CLI tools and services
  need, and protocols that have one obvious API (HTTP, JSON, TLS, gzip).
  Clients for particular servers (Postgres, Redis, LLM APIs) are packages:
  each has many ways to be used, changes with its server, and is written
  in the language on top of `net`, `crypto` and `sql`. SQLite is the
  exception, being a file format rather than a server.
- **Static programs:** `plumb build --static` (Linux) links everything into
  one file that runs anywhere, like Go. On macOS the system libraries are
  always present, so it isn't needed.
- **HTTP server:** a handler is `fn(Request) throws -> Response`. An error or
  a bug becomes a 500. Handlers can be tested without a network.
- **Third-party packages** work like Go's (see Modules and packages).

### Data formats and text

- **A `String` is always valid UTF-8** (Vlad, 2026-09-29). Text from
  outside (files, standard input, streams, process output, the network,
  the environment, arguments, databases, JSON escapes) is checked on the
  way in, and each invalid sequence becomes U+FFFD (�), so reading text
  never fails and every string function can rely on it. Bytes that must
  survive exactly stay `Bytes`; `Bytes.text()` is the strict conversion
  (an error on invalid UTF-8), `Bytes.text_lossy()` the replacing one.
  Go lets a string hold any bytes, which means each function needs its own
  rule for invalid ones; Rust, Swift, Java and JS guarantee valid text.
- **JSON keys in any case:** decoding matches a field by its exact name,
  then ignoring case and `_`/`-` (`createdAt` fills `created_at`), so
  camelCase APIs need no options; `json.encode_camel` writes them. A key
  written twice, or two keys that would fill the same field (`name` and
  `NAME`), is an error: parsers disagree on which one wins (first or
  last), and a proxy and a service that disagree are a security hole.
- **Templates are Handlebars-style** (`{{name}}`, `{{#each}}`, `{{#if}}`,
  partials): the most widely known template language that isn't a
  programming language itself. Values are HTML-escaped in HTML templates,
  and a name missing from the data is an error with its line, not empty
  text: a typo can't hide.
- **YAML, Markdown, JWT, semver, SMTP, S3** are packages written in the
  language (see `packages/`), not the standard library: each has
  dialects or evolves on its own.

### Syntax: C family

Experiment `experiments/run3`: braces and indentation gave no difference (0
syntax slips in 40 files), so braces stay for the reasons below.


- Braces for blocks, with a mandatory formatter, so indentation always
  matches them. Reasons: most backend training data uses braces; wrapping a
  block in `with`/`if` doesn't re-indent it (safer string-replacement edits,
  cleaner diffs); truncated output is caught by an unbalanced brace.
- `${x}` interpolation (backends put JSON in strings, so `{x}` would clash).
  A type's text there is its own `fn to_string(self) -> String` if it has
  one (so `Money` prints `$12.50`, a `Duration` `1.5s`), an error's is its
  `message()`, and otherwise the value as written (`User(id: 1, ...)`).
  `to_string` has exactly that shape, so it can't be confused with other
  conversions.
- `and` / `or` / `not` (a reviewer misses `!` easily).
- `//` comments, `name: Type`, `-> Result`, `<T>`, no semicolons.
- `match x { Circle(r) => … }` without `case`.

### Rejected

- **`var` parameters with `&` at the call site** (`add_tag(&tags, "x")`).
  A reader can't understand them without knowing the rule, whatever the
  keyword (`var`, `mut`, `inout`). Rust's `&mut` also suggests references and
  borrowing, which we don't have.
- **`io` marker in signatures.** It's a second function color. Most of its
  value is already covered by `throws`, and in a backend it would be on most
  functions, so the signal would disappear. Tooling can infer purity if
  needed.

## Compiler (`compiler/`)

Written in Rust. Pipeline:
`lexer → parser → check (types, inference, all language rules) → lower (MIR,
generics instantiated per type) → rc (reference counting from liveness) →
cgen (C) → clang`.

- **Memory:** reference counting inserted by the compiler from liveness:
  a value is dropped right after its last use, and moved (not copied) into
  its last use. Copy-on-write when the count is 1, so `x = f(x)` and
  `xs.append(x)` change in place. Small objects come from per-size free
  lists; large blocks go straight to the OS.
- **Errors:** a failing function returns an error value; no unwinding.
- **One C file** per program with the runtime (`src/runtime/rt.h`), so clang
  inlines across everything.
- `plumb run | build | test | check`, `--debug` (AddressSanitizer + leak
  count), `--emit-c`. Tests: `compiler/tests/run.sh`.
- **Decided while building:** bit operations are `Int` methods
  (`bit_and`, `bit_or`, `bit_xor`, `shift_left`, `shift_right`), not
  operators. The prelude marks higher-order functions `rethrows`, so
  `xs.map(x => try f(x))` throws only when the lambda can.

**Not implemented yet:** the items in `TODO.md`.

### After the critic's review (Vlad, 2026-09-29)

`research/critique.md` found places where the language silently does the
wrong thing; Vlad chose (see `research/critique-response.md`):

- **New compile errors:** an optional value inside text (`"${x}"` with
  `x: String?`: write `${x ?? ""}`); `m[k] op= v` on a map of plain values
  (write `m[k] = (m[k] ?? 0) + v`); a lambda using a `var` that is
  assigned after the lambda is made (it would see the old value); an
  unused `let` or import (as in Go: they hide forgotten results).
- **Generic bounds:** `fn largest<T: Ordered>(xs: List<T>)` and an
  interface as a bound (`fn total<T: Shape>(xs: List<T>)`), monomorphized.
  The prelude's `max`, `sum`, `sorted` use them too: no powers users
  don't have.
- **Fields are private unless `pub`**, like functions: a field without
  `pub` is visible only in its file, so a type can guard its values
  (`email.parse` is the only way to make an `Email`).
- **`is` looks through `cause`:** `err is files.NotFound` is true when the
  error or any error in its `cause` chain is one (Go's `errors.Is`).
- **SQL from literal pieces:** a query may be chosen or joined from
  literals (`if`/`match` over literals, a top-level `let` of a literal),
  and a `List` parameter expands `in (?)` to `in (?, ?, ?)`. Still no
  injection: every piece is a literal.
- **Name every argument after the first**, whenever there are two or more
  parameters (the 3+ rule for all): one way to call, and calls explain
  themselves; stdlib parameter names are chosen to read well
  (`max(a, or: b)`).
- **No nested functions**: functions live at the top level; the error for
  `fn` inside a function shows a typed lambda for small local helpers.
- **Ranges stay as they are** (only in `for`, counting up by one). Vlad
  first chose `.reversed()`/`.step(n)` on them, then dropped it: that
  borrows Rust's and Swift's syntax, where ranges are values, without
  making them values (`let r = 0..<n` would still be an error). Counting
  down is rare; it's a `while` loop, as in Zig. Walking a list backwards
  is `for x in xs.reversed()`.

## Open questions

- **Changing a value behind an interface** without `var` parameters: a
  function `copy(from: Storage + Listable, to: Storage) -> Storage` returns
  the destination as `Storage`, so the caller loses its concrete type
  (`FileStorage`). Options: generics with interface bounds (removed earlier),
  or accept it.
- **String representation:** `split`/`lines`/`words` allocate every piece
  (5 million small allocations in the `words` benchmark, where Go returns
  views into the original text). Making `String` a view (pointer + length +
  owner) would make slicing free, at the cost of 24 bytes per value.

