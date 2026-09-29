# lang

A programming language for code that **AI agents write and people review**:
CLI tools, scripts and backend services. It compiles to native programs that
run about as fast as Go.

```
import db
import http
import json

type Note {
  id: Int
  title: String
  done: Bool = false
}

type NewNote {
  title: String
}

fn list_notes(conn: db.Connection) throws -> http.Response {
  let notes = try conn.query<Note>("select id, title, done from notes order by id", [])
  return http.json(200, notes)
}

fn add_note(conn: db.Connection, req: http.Request) throws -> http.Response {
  let input = try json.decode<NewNote>(try req.text())
  if input.title.trim().is_empty() {
    return http.text(400, "the title is empty")
  }
  try conn.execute("insert into notes (title) values (?)", [input.title])
  return http.json(201, Note(id: conn.last_id(), title: input.title))
}

fn main() throws {
  with conn = try db.open("notes.db") {
    try conn.execute("create table if not exists notes (id integer primary key, title text, done bool default false)", [])
    var router = http.Router()
    router.get("/notes", req => try list_notes(conn))
    router.post("/notes", req => try add_note(conn, req: req))
    try http.serve(router, port: 8080)
  }
}
```

`lang build notes.lang` gives one native binary. It uses all cores, and every
request runs in its own lightweight task.

## Why another language

When agents write most of the code, the language should make their mistakes
impossible or loud, and make the result easy to review. What that means
here:

- **One way to write each thing.** The syntax is the mainstream C-family
  one, and the rules are stricter than in mainstream languages.
- **The compiler is the reviewer.** Every error names the fix. Ignoring a
  result, building SQL from strings, forgetting to close a file, leaking a
  task: all are compile errors.
- **What matters is visible.** A function that can fail says `throws`, and
  every call to it starts with `try`. A function never changes its
  arguments: `products = restock(products, name: "Widget", amount: 3)`.
- **Values, not references.** Records, enums and lists are values with
  copy-on-write. There is no `null`, no inheritance and no shared mutable
  state except `Shared<T>`, which you reach only through a lock.
- **Concurrency without colors.** Code is sequential; `spawn f(x)` runs work
  in parallel and returns a task that belongs to the function that started
  it. There's no `async`/`await`.
- **Batteries included.** A typical service or tool needs no third-party
  packages.

The reasoning behind every decision is in [`DESIGN.md`](DESIGN.md).

## A short tour

```
// Types: records, enums with data, new types, optional values
type UserId = Int

enum Shape {
  Circle(radius: Float)
  Rect(width: Float, height: Float)
}

fn area(s: Shape) -> Float {
  return match s {
    Circle(radius) => 3.14159 * radius * radius
    Rect(width, height) => width * height
  }
}

// Errors: `throws`, `try`, `catch`
fn load(path: String) throws -> Config {
  let text = try files.read(path) catch err {
    log.warn("no config, using defaults: ${err.message()}")
    return Config()
  }
  return try json.decode<Config>(text)
}

// Resources are closed by `with`, even on an error
with f = try files.create("out.txt") {
  try f.write_text("hello\n")
}

// Parallel work
let user = spawn load_user(id)
let orders = spawn load_orders(id)
let page = Dashboard(user: try user.wait(), orders: try orders.wait())

// Lists
let adults = users.filter(u => u.age >= 18).map(u => u.name)

// Tests live next to the code
test "area of a square" {
  expect area(Rect(width: 2.0, height: 2.0)) == 4.0
}
```

A compact guide for agents (and humans) writing the language is in
[`AGENTS.md`](AGENTS.md).

## The standard library

`files`, `path`, `io` (streams: files, stdin/stdout, sockets, programs),
`process`, `env`, `cli`, `log`, `time`, `json`, `http` (server and client:
routing, files with sendfile, streaming, HTTPS), `net` (TCP, UDP, TLS),
`sql` + `db` (SQLite), `crypto`, `encoding`, `random`, `regex`, `csv`,
`xml`, `url`, `zlib`, `math`.

All external data comes in through `decode<T>`: the compiler generates the
conversion into your record (`json.decode<User>(body)`,
`env.decode<Config>()`, `cli.decode<Options>()`, `conn.query<User>(...)`).

The full reference, generated from the sources, is in
[`STDLIB.md`](STDLIB.md).

## Packages

A package is a git repository (or a directory inside one), pinned by commit
in `lang.lock`:

```
lang new shop && cd shop
lang add postgres https://github.com/semenov/beautiful-lang --path packages/postgres
```

This repository has four, all written in the language itself:

- [`packages/postgres`](packages/postgres): the PostgreSQL wire protocol,
  SCRAM login, TLS, typed rows;
- [`packages/redis`](packages/redis): commands, pipelines, pub/sub;
- [`packages/markdown`](packages/markdown): Markdown to HTML (CommonMark
  basics, GitHub tables and strikethrough), safe for user-written text;
- [`packages/llm`](packages/llm): OpenAI-compatible chat APIs (OpenAI,
  OpenRouter, Ollama, ...): typed answers from a record's JSON Schema, tool
  calls, streaming, embeddings.

## Using it

The compiler is written in Rust and produces C, which the system's C
compiler turns into a native program.

```
cd compiler && cargo build --release      # the `lang` binary: target/release/lang

lang run app.lang [args]     # compile and run
lang build app.lang -o app   # an optimized binary
lang build --static app.lang # Linux: one file with no library dependencies
lang test app.lang           # run the `test` blocks
lang check app.lang          # only check for errors
lang fmt                     # lay out every .lang file the standard way
lang run --debug app.lang    # with memory checking and a leak count
lang new / add / fetch / update   # projects and packages
```

It needs `cc` (clang or gcc), plus libcurl, SQLite and zlib for the modules
that use them. macOS has them all. On Linux, TLS uses OpenSSL.
[`tools/linux`](tools/linux) has an Alpine image for static builds and for
running the tests on Linux.

## Speed

Programs compile to C and then to native code. Memory is managed by
reference counting that the compiler inserts, with no garbage collector.

| benchmark | lang | Go |
|---|---|---|
| records (allocation-heavy) | 0.38 s, 156 MB | 0.46 s, 396 MB |
| binary trees | 2.17 s | 2.63 s |
| sort | 1.07 s | 1.62 s |
| word count | 1.29 s | 0.93 s |
| HTTP file server, small files | 93k req/s, 7 MB | 100k req/s, 23 MB |

(`benchmarks/`, an Apple M-series laptop.)

## Status

A working language and toolchain, used for real programs and packages. The
test suite (`compiler/tests/run.sh`) runs every program under
AddressSanitizer with a leak check, on macOS and Linux. Not done yet: the items in
[`TODO.md`](TODO.md).

Earlier designs are kept in [`drafts/`](drafts).
