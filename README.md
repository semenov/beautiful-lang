# Plumb

A programming language for code that **AI agents write and people review**:
CLI tools, scripts and backend services. It compiles to native programs that
run about as fast as Go.

(Named after a plumb line: code that reads straight down, with nothing
hidden. The command is `plumb`, files end in `.plumb`.)

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

`plumb build notes.plumb` gives one native binary. It uses all cores, and every
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
`xml`, `template`, `url`, `zlib`, `archive` (tar, zip), `term`, `math`.

All external data comes in through `decode<T>`: the compiler generates the
conversion into your record (`json.decode<User>(body)`,
`env.decode<Config>()`, `cli.decode<Options>()`, `conn.query<User>(...)`).

The full reference, generated from the sources, is in
[`STDLIB.md`](STDLIB.md).

## Packages

A package is a git repository (or a directory inside one), pinned by commit
in `plumb.lock`:

```
plumb new shop && cd shop
plumb add postgres https://github.com/semenov/plumb --path packages/postgres
```

This repository has several, all written in the language itself:

- [`packages/postgres`](packages/postgres): the PostgreSQL wire protocol,
  SCRAM login, TLS, typed rows;
- [`packages/redis`](packages/redis): commands, pipelines, pub/sub;
- [`packages/yaml`](packages/yaml): YAML configs into records (checked
  against PyYAML), and writing YAML;
- [`packages/s3`](packages/s3): Amazon S3 and compatible storage (R2,
  MinIO, ...), SigV4 signing, presigned links;
- [`packages/smtp`](packages/smtp): sending email (STARTTLS, attachments);
- [`packages/semver`](packages/semver): versions and npm-style ranges;
- [`packages/jwt`](packages/jwt): JSON Web Tokens (HS256, RS256, ES256,
  provider key sets), checked against PyJWT;
- [`packages/markdown`](packages/markdown): Markdown to HTML (CommonMark
  basics, GitHub tables and strikethrough), safe for user-written text;
- [`packages/llm`](packages/llm): OpenAI-compatible chat APIs (OpenAI,
  OpenRouter, Ollama, ...): typed answers from a record's JSON Schema, tool
  calls, streaming, embeddings.

## Using it

The compiler is written in Rust and produces C, which the system's C
compiler turns into a native program.

```
cd compiler && cargo build --release      # the `Plumb` binary: target/release/plumb

plumb run app.plumb [args]     # compile and run
plumb build app.plumb -o app   # an optimized binary
plumb build --static app.plumb # Linux: one file with no library dependencies
plumb test app.plumb           # run the `test` blocks
plumb check app.plumb          # only check for errors
plumb fmt                     # lay out every .plumb file the standard way
plumb run --debug app.plumb    # with memory checking and a leak count
plumb new / add / fetch / update   # projects and packages
```

It needs `cc` (clang or gcc), plus libcurl, SQLite and zlib for the modules
that use them. macOS has them all. On Linux, TLS uses OpenSSL.
[`tools/linux`](tools/linux) has an Alpine image for static builds and for
running the tests on Linux.

## Speed

Programs compile to C and then to native code. Memory is managed by
reference counting that the compiler inserts, with no garbage collector.

| benchmark | Plumb | Go |
|---|---|---|
| records (allocation-heavy) | 0.43 s, 156 MB | 0.55 s, 396 MB |
| binary trees | 2.76 s, 130 MB | 4.70 s, 137 MB |
| sort (3M records by key) | 1.17 s, 151 MB | 1.79 s, 97 MB |
| word count | 1.42 s, 226 MB | 1.01 s, 211 MB |
| JSON encode + decode | 1.55 s, 264 MB | 2.35 s, 247 MB |
| map of Ints | 1.17 s, 42 MB | 1.49 s, 43 MB |
| CSV-like text processing | 0.61 s, 138 MB | 0.99 s, 164 MB |
| n-body (floating point) | 0.23 s, 2 MB | 0.21 s, 4 MB |
| writing and reading a file by lines | 0.96 s, 2 MB | 0.96 s, 11 MB |
| channel, producer and consumer | 0.07 s, 2 MB | 0.13 s, 4 MB |
| spawning 200k small tasks | 0.06 s, 7 MB | 0.06 s, 14 MB |
| HTTP file server, 2 KB files | 75-93k req/s, 7 MB | 55k req/s, 27 MB |

(`benchmarks/`: `ONLY=Go,Plumb python3 run.py`; median of 3 runs on an
Apple M-series laptop, 2026-09-29. Where we lose, `TODO.md` says why.)

## How tasks run

`spawn f(x)` starts a task: a function running on its own stack, in
parallel with the others. Tasks run on a pool of OS threads, one per core
(an M:N scheduler, like Go's). Everything that waits (a task's result, a
channel, a lock, a timer, a socket) parks the task and lets its thread run
another one; there is no `async`/`await`, and code reads top to bottom.

- **Switching** between tasks saves and restores only the callee-saved
  registers (a few dozen instructions of assembly).
- **Stacks:** each task reserves 8 MB, like a program's main thread, but
  only the top 256 KB is open at first and only touched pages cost memory
  (16 KB for a small task). A task that goes deeper faults once, and the
  fault handler opens the rest; past 8 MB it's a clear "stack overflow"
  panic. Stacks are reused.
- **Run queues:** each worker thread has its own queue. A task made ready on
  a worker goes into that worker's queue; a worker with nothing to do takes
  from a global queue (used by the timer and I/O threads), then steals half
  of another worker's queue, spins briefly, and sleeps. A task woken by a
  channel runs next on the worker that woke it, so a producer and a
  consumer stay on one core.
- **I/O:** sockets and pipes are non-blocking. A task that would block
  registers the descriptor with kqueue (macOS) or epoll (Linux) and parks;
  a poller thread wakes it when the descriptor is ready.
- **Timers:** `sleep`, `time.timeout` and tickers are served by a timer
  thread (on macOS through a kqueue timer, which isn't delayed for
  background processes the way a condition variable's timeout is).
- **Structured:** a function waits for the tasks it started before it
  returns, so no task outlives its caller. An error from a task that
  nobody waited for comes out of the function. Cancelling a task (Ctrl-C,
  `time.timeout`, a failed sibling) cancels its tasks too: their waits
  throw `Cancelled`, and `with` blocks close their resources.
- **Deadlocks:** if every task waits for another and nothing else (no
  timer, no socket) can wake one, the program stops with "deadlock"
  instead of hanging.
- **Memory:** each thread has its own free lists for small objects; with
  tasks, reference counts change atomically.

What it's good at: sequential-looking code for servers and pipelines,
cheap tasks (a small one costs about 16 KB and a microsecond), channels as
fast as Go's, no leaked tasks, and cancellation that always reaches the
bottom of the call.

Weak spots, known:

- **No preemption.** A task that computes for a long time without waiting
  keeps its thread; the other threads keep running tasks, but if every
  thread is busy computing, a task whose socket became ready waits.
- **Blocking calls hold a thread:** SQLite and DNS lookups are marked, and
  when a thread has been stuck in one for 10 ms while other tasks wait, a
  spare thread is started (like Go's hand-off, but coarser). Reading files
  isn't marked yet.
- **The I/O path** re-registers a descriptor on every wait (one system call
  each time) and hands ready tasks over from a separate poller thread.
  Go polls from the idle workers themselves.
- **`time.timeout` deadlines** are a list behind a mutex: fine for a few,
  not for thousands at once. (HTTP server timeouts don't use them: each
  connection keeps its deadline, and the timer thread sweeps them once a
  second, so they're up to a second late.)
- **Atomic reference counts** in any program that uses tasks cost about 10%
  on allocation-heavy code, even for values that never leave one task.
- Each task reserves 8 MB of address space. That's nothing on 64-bit
  systems, unless the kernel is set up to refuse overcommitting memory.

## Status

A working language and toolchain, used for real programs and packages. The
test suite (`compiler/tests/run.sh`) runs every program under
AddressSanitizer with a leak check, on macOS and Linux. Not done yet: the items in
[`TODO.md`](TODO.md).

Earlier designs are kept in [`drafts/`](drafts).
