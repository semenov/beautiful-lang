# A critic's review of Plumb (2026-09-29)

## What I did

I read `README.md`, `AGENTS.md`, `DESIGN.md` (with "Rejected" and "Open
questions"), skimmed `STDLIB.md`, the RealWorld, httpbin and gron ports
and their `GAPS.md`, the backend benchmark server, the examples, and
`research/stdlib-review-2026-09.md`. Then I wrote about 70 small programs
in `/tmp/critic/` and ran them with the release compiler (`plumb run`,
`check`, `test`, `fmt`, `build`, `--debug`). I wrote what a newcomer from
Go, Rust, Swift, TypeScript or Python would write.

Every example below was run. The output shown is the real output, trimmed
to the relevant lines. I skipped things the GAPS files and the stdlib
review already cover, unless I had something new to add.

Overall: the language is coherent and the error messages are usually good.
Most of my complaints fall into three groups:

1. **Value semantics without the safety net that DESIGN.md promises.**
   Changes to copies are silently lost. Some copies are silently quadratic.
2. **"One way to do it" and "the compiler is the reviewer" are not yet
   true.** There are two call styles, lax parsers, and missing checks.
3. **Some performance cliffs depend on the data**, so tests pass and
   production is slow.

Severity: **major** means a real user will lose data, time or
performance, or the design has a hole. **Medium** means real friction or
a real bug class, with a workaround. **Minor** means a papercut.

---

## 1. Changing a copy is silently lost, and the promised error doesn't exist

**Severity:** major. **Kind:** semantics.

DESIGN.md promises this: "Compiler error: 'you changed a copy that is
never used afterward' (with the suggested fix)." The error doesn't exist.

```
type Acc {
  balance: Int
}
fn main() {
  var accounts: Map<String, Acc> = {"a": Acc(balance: 10)}
  if accounts["a"] is some(acc) {
    var a = acc
    a.balance -= 5          // lost
  }
  var list = [Acc(balance: 1)]
  var first = list[0]
  first.balance = 99        // lost
  print("${accounts} ${list}")
}
```
```
{"a": Acc(balance: 10)} [Acc(balance: 1)]
```

It compiles with no warning, and both changes vanish.

**Why it matters.** This is *the* bug that Go, Java, TypeScript and Python
programmers bring with them. In those languages `list[0]` of an object is a
reference. Agents trained on that code will write `var x = list[i];
x.field = ...` all the time. Swift has the same semantics and the same
trap. That is why DESIGN.md planned this check. Without it, value
semantics removes aliasing bugs and adds lost-update bugs.

**What others do.** Swift warns "variable was never used" in some of these
cases. Rust's borrow checker makes you write `&mut list[0]`. The
check that DESIGN.md describes (a `var` that is changed and never read
afterward) is simple liveness information the compiler already computes
for reference counting.

A related case: the loop variable can't be changed, and the hint is wrong.

```
for it in list {
  it.qty = 100
}
```
```
error: can't change `it`: it's declared with `let`
  = help: declare it with `var it = ...`
```

`for var it in list` is a parse error, and "declare it with var" would
only bring back the lost-update bug above. The right hint is `for i in
0..<list.length { list[i].qty = 100 }`.

---

## 2. Enum patterns bind by position, but values are built by name

**Severity:** major. **Kind:** semantics.

```
enum Shape {
  Rect(width: Float, height: Float)
}
fn main() {
  let r = Rect(width: 2.0, height: 10.0)
  match r {
    Rect(height, width) => print("height=${height} width=${width}")
  }
}
```
```
height=2.0 width=10.0
```

The pattern uses the real field names, in the wrong order, and gets the
wrong values without a word. Building a variant requires names
(`Rect(width:, height:)`), so a reader naturally assumes the pattern binds
by name too.

**Why it matters.** It passes review, because it *reads* correctly. It
also breaks silently when someone reorders fields in the enum.

**What others do.** Rust: `Rect { height, width }` binds by name, and a
wrong name is an error. Swift checks the labels:
`case .rect(width: let w, height: let h)`. The fix here: when a pattern
binding has the same name as a field, bind that field. When the name
matches a *different* field of the variant, make it an error.

---

## 3. String indexing is fast for ASCII and quadratic for everything else

**Severity:** major. **Kind:** performance.

`length`, `slice` and `index_of` count characters (code points). For
ASCII text there is a fast path. For any string that has one non-ASCII
character, each `slice` scans from the start.

```
let s = "${"a".repeat(80000)}é"     // one é at the very end
for i in 0..<s.length {
  if s.slice(from: i, to: i + 1) == "a" { n += 1 }
}
```

| text (loop over every character with `slice`) | time |
|---|---|
| 80,000 ASCII characters | ~1 ms |
| the same with one `é` at the end | **2.6 s** |
| `"aé".repeat(10000)` / `20000` / `40000` | 0.28 s / 1.1 s / 5.1 s |

**Why it matters.** This cliff depends on the *data*. Tests use ASCII
fixtures and pass. Then a user named "José" makes a request slow, and a
long document can take seconds. This is the worst kind of performance
bug: invisible in review and in CI. `regex.Match.start/end` are also
"character indices", which makes the same cost show up in regex code.

**What others do.** Go and Rust index by byte, and it's O(1).
Swift makes `String.Index` opaque, so you can't write the O(n²) loop by
accident. Python stores a fixed-width representation per string. Pick
one: byte offsets (Go/Rust), opaque indices (Swift), or a cached
index. DESIGN.md's open question about string views is the right time
to decide.

---

## 4. Copy-on-write makes innocent-looking code quadratic

**Severity:** major. **Kind:** performance.

Two loops that look like the same thing:

```
fn add(s: State, x: Int) -> State {
  var t = s
  t.items.append(x)
  return t
}
fn add_keep(s: State, x: Int) -> State {
  var t = s
  t.items.append(x)
  print_if_big(s)          // reads the old value after changing the copy
  return t
}
// st = add(st, x: i) in a loop, and the same with add_keep
// plus: var v = m["k"] ?? []; v.append(i); m["k"] = v
```
```
n = 20,000:   x = f(x): 51 µs    old value read after: 37 ms    map read-modify-write: 37 ms
n = 100,000:  x = f(x): 266 µs   old value read after: 1.70 s   map read-modify-write: 1.70 s
```

Five times the data takes 46 times longer. That is quadratic. The only
difference is one read of the old value, or reading from a map before
writing back.

**Why it matters.** AGENTS.md mentions the map case in one sentence. But
the rule is general: *any* use of the old value after the change forces a
full copy. Nothing in the source shows it. The reader can't see the cost,
and "reads straight down, with nothing hidden" is the language's promise.

**What others do.** Swift has the same trap. Its answer is `inout`
and `_modify` accessors, plus years of blog posts. Rust makes the copy
explicit (`.clone()`). Suggestion: the compiler knows when it inserts a
full copy of a container inside a loop. Report it, at least with
`plumb check --perf`, or as a note in the error format the agents
already read.

---

## 5. `Shared<T>` is one mutex: concurrent readers get 27x slower

**Severity:** major. **Kind:** performance.

A read-mostly cache is the most common piece of shared state in a
backend. `Shared` has only `lock()`.

```
fn reader(cache: Shared<Map<Int, Int>>, n: Int) -> Int {
  var s = 0
  for i in 0..<n {
    with c = cache.lock() {
      s += c[i.bit_and(1023)] ?? 0
    }
  }
  return s
}
```
```
1 task, 8M reads:           82 ms
8 tasks, 1M reads each:     2.23 s
```

The same work spread over 8 cores is 27 times slower than on one.

**Why it matters.** The guide says to put shared state in a `Shared` and
pass it to handlers. Under load, every request queues on one lock. There
is no read lock, no atomic counter, and no concurrent map. A `Shared`
per key is possible (see 22), but not documented as a pattern.

**What others do.** Go has `sync.RWMutex`, `sync.Map` and
`atomic.Int64`. Rust has `RwLock`, `Arc<AtomicU64>` and `dashmap`. Java
has `ConcurrentHashMap`. A `with v = s.read() { }` (read-only `v`) keeps
the one-construct model and fixes the common case.

---

## 6. Generics without bounds: users can't write what the prelude writes

**Severity:** major. **Kind:** type system.

```
fn largest<T>(xs: List<T>) -> T? {
  ...
      if x > b {
```
```
error: `T0` values can't be ordered
  = help: only numbers and text can be ordered; compare a field instead
```

```
fn total<T>(xs: List<T>) -> Float {
  ...
    t += x.area()
```
```
error: can't call `area` on a generic value
  = help: generic values can only be compared, used as keys and put into text
```

The prelude has `fn max<T>(a: T, b: T) -> T`, `List.sum()`, `sorted()`
and `min()`. They all need an "orderable" or "number" bound that user
code can't express. The stdlib has a feature users are not allowed to
have. (The message also leaks the internal name `T0`, and still says
"text" for `String`.)

The workarounds each have a cost. You can pass a function
(`sum(xs, f: q => q.area())`). You can use an interface-typed list, which
boxes every value, and `List<Sq>` isn't accepted where `List<Shape>` is
expected:

```
error: expected `List<Shape>`, found `List<Sq>`
```

This error has no hint. The fix is `squares.map(q => q)`.

**Why it matters.** DESIGN.md says that bounds aren't needed because
`==`, hashing and `sort_by` work for everything. That's true for
containers. It isn't true for the helper functions every codebase grows
(`clamp`, `max_by_score`, `total`, a generic `Cache<K, V>` with eviction
by time). Go ran this experiment for ten years without generics and then
added constraints. DESIGN.md's first open question (changing a value
behind an interface) points to the same missing piece.

**What others do.** Go: `[T cmp.Ordered]` and `[T Shape]`. Swift
and Rust: protocol and trait bounds. A minimal version fits the rules: one
built-in bound (`<T: Ordered>`) plus interface bounds (`<T: Shape>`),
monomorphized. That's one new concept, and it removes the stdlib's
privileges.

---

## 7. SQL must be a literal, so ordinary queries can't be written

**Severity:** major. **Kind:** semantics / stdlib.

The literal rule blocks three things every backend does in its first
week:

```
fn by_ids(conn: db.Connection, ids: List<Int>) throws -> List<Row> {
  return try conn.query<Row>("select id from t where id in (?)", [ids])
}
fn sorted(conn: db.Connection, column: String) throws -> List<Row> {
  let q = if column == "id" { "select id from t order by id" } else { "select id from t order by name" }
  return try conn.query<Row>(q, [])
}
```
```
error: expected `Value`, found `List<Int>`
error: the SQL must be written right here, as text in quotes
```

Even choosing between *two literals* is rejected. So `IN (...)` lists,
a user-chosen `ORDER BY`, optional filters and bulk inserts need tricks.
The RealWorld port used an SQL view, `(?2 = '' or col = ?2)` and
numbered parameters (its GAPS #7). The stdlib review suggests
documenting `json_each(?)`. These tricks work on SQLite and hurt query
plans on Postgres.

**Why it matters.** The goal (no injection) is right. But when the rule
blocks legitimate code, people route around it with worse SQL. The
Postgres package can't take an array parameter either.

**What others do.** sqlc and jOOQ generate queries. Go's `sqlx.In`
expands a slice into `?, ?, ?`. Rust's `sqlx` checks literals at compile
time and also has `QueryBuilder`. A small fix in the spirit of the rule:
allow a `sql.Query` built from literal pieces (`if`/`match` over literals,
concatenation of literals), and a list parameter that expands to
`?, ?, ?`. Injection is still impossible, because every piece is still a
literal.

---

## 8. An optional value prints as `none` inside text

**Severity:** medium. **Kind:** semantics.

```
let name: String? = none
let m: Map<String, Int> = {}
print("name=${name} m=${m["x"]}")
```
```
name=none m=none
```

`"Hello ${user.nickname}"` compiles and sends "Hello none" to a customer.
Also, `${orders[1]}` prints the record without any "some" marker, so the
reader can't tell that the value was optional.

**Why it matters.** The type system knows it's `T?`. Everywhere else
the compiler forces you to decide what to do about a missing value, except
here, the place that goes straight to users.

**What others do.** Swift warns: "String interpolation produces a debug
description for an optional value". Kotlin prints `null`, and
linters flag it. Make it an error with the fix `${x ?? "..."}`.

---

## 9. `counts[w] += 1` compiles and then panics

**Severity:** medium. **Kind:** semantics.

```
var counts: Map<String, Int> = {}
for w in ["a", "b", "a"] {
  counts[w] += 1
}
```
```
panic: the key is not in the map
```

Reading `counts[w]` gives `Int?`, so `counts[w] + 1` wouldn't compile.
But the compound assignment compiles and becomes "a missing key is a bug".

**Why it matters.** This is the most common map idiom in Go (zero value)
and Python (`defaultdict`, `Counter`). The design rule (a path change to
a missing key is a bug) makes sense for `accounts[id].balance -= x`. For
`+=` on a scalar, it's a crash the compiler could have caught.

**What others do.** Go returns the zero value. Swift has
`dict[k, default: 0] += 1`. Rust has `*map.entry(k).or_insert(0) += 1`.
Suggestion: reject `m[k] op= v` when `V` is a plain value, with the fix
`m[k] = (m[k] ?? 0) + 1`. Or point to `count_each()`.

---

## 10. No private fields, so a type can't protect its invariants

**Severity:** medium. **Kind:** type system.

"Fields are visible wherever the type is." So a validated type can be
built or changed anywhere:

```
// email.plumb
pub type Email {
  value: String
}
pub fn parse(text: String) throws -> Email { ... checks "@" ... }

// main.plumb
var e = email.Email(value: "garbage")
e.value = "also garbage"
```
```
Email(value: "also garbage")
```

The same goes for `type UserId = Int`: anyone can write `UserId(-1)`.

**Why it matters.** "Parse, don't validate" is the best tool a type
system gives against bugs. It needs constructors that only the module can
call. DESIGN.md's test is "rules out a class of bugs". Private fields do
exactly that, and cost one keyword that already exists (`pub`).

**What others do.** Go: lowercase fields. Rust: private fields by
default. Swift: `private(set)`. Suggestion: the same rule as functions,
private unless `pub`, or at least "construction outside the module only
if all fields are `pub`".

---

## 11. Wrapping an error hides its type

**Severity:** medium. **Kind:** semantics / stdlib.

AGENTS.md teaches two things: wrap with
`throw Failure(message: "...", cause: err)`, and handle with
`if err is files.NotFound`. Together, they don't work:

```
fn load(p: String) throws -> String {
  return try files.read(p) catch err {
    throw Failure(message: "loading config", cause: err)
  }
}
// caller:
if err is files.NotFound { ... } else { print("other: ${err}") }
```
```
other: loading config: can't read "/nope": No such file or directory
```

No helper walks the `cause` chain.

**Why it matters.** Untyped `throws` puts all the weight on runtime
checks. Once one layer adds context, which the docs recommend, every
check above it silently becomes false.

**What others do.** Go has `errors.Is` and `errors.As`, which walk
`Unwrap()`. Rust's `anyhow` has `downcast_ref` over the chain. Make `is`
look through `cause`, or add `err.find<T>() -> T?`.

---

## 12. Lambdas capture a snapshot, and nothing says so

**Severity:** medium. **Kind:** semantics.

```
var count = 0
let inc = () => count + 1
count = 10
print("${inc()}")
```
```
1
```

Changing a captured `var` *inside* a lambda is an error, with a good
message. But reading a `var` that changes *later* silently gives the old
value. JavaScript, Go, Swift, Python and Kotlin all print 11.

**Why it matters.** It rarely bites in handlers. But when it does, the
program is wrong without any sign. I suspect the design is right
(capture by value is what makes closures safe to pass across tasks), but
it's under-explained: AGENTS.md says only "capture values by copy".

**What others do.** Swift captures by reference unless you write
`[count]`. C++ makes you pick `[=]` or `[&]`. Cheapest fix: an
error when a lambda captures a `var` that is assigned after the lambda is
made. With it, the snapshot is never a surprise.

---

## 13. Two ways to call a two-parameter function, both used in this repo

**Severity:** medium. **Kind:** syntax.

The rule is: with 3 or more parameters, name every argument after the
first. With 2 parameters, naming is *optional*:

```
print("${add(1, 2)} ${add(1, b: 2)} ${max(1, 2)} ${max(1, b: 2)}")
```

All four compile. `plumb fmt` leaves both forms alone. The repository's
own code uses both:

```
min(n, 100)          min(limit, 100)       min(body.length, 300)
min(limit, b: 100)   min(100, b: n)        max(0, b: j)
```

(`benchmarks/backend/plumb/server.plumb` has `max(1, b: min(limit,
b: 100))`.)

**Why it matters.** "One way to write each thing" is principle 3. And a
parameter named `b` shows that naming isn't always useful: `max(0, b: j)`
says nothing that `max(0, j)` doesn't.

**What others do.** Swift decides per parameter, at the declaration
(argument labels), and the call site must match. Suggestion: let the
formatter decide (name them if the declaration's names aren't
single letters), or make 2-parameter calls always positional.

---

## 14. Integer `div` and `%` round toward minus infinity, and the docs don't say so

**Severity:** medium. **Kind:** semantics / docs.

```
print("${7.div(2)} ${(-7).div(2)} ${(-7) % 2} ${7 % -2}")
```
```
3 -4 1 -1
```

That is Python's floored division. Go, Rust, C, Java, JavaScript, Swift
and Kotlin all give `-3 -1 1` for the same expressions. Neither
AGENTS.md nor `plumb doc Int` says this. `%` isn't documented at all.

**Why it matters.** Floored division is arguably *better* (for example,
`(-1) % 7 == 6` for weekdays and buckets). But every port from Go or Rust
code that handles negative numbers (time offsets, coordinates, balances)
silently changes behaviour. Agents will assume C semantics.

**What others do.** Python documents it prominently. Rust gives both
(`/` and `div_euclid`). At minimum, one line in the guide and in
`plumb doc Int.div`.

---

## 15. `is none or ...` doesn't narrow the type

**Severity:** medium. **Kind:** type system.

```
fn g(e: String?) -> String {
  if e is none or e.is_empty() {
    return "no"
  }
  return e
}
```
```
error: this value may be missing   (at e.is_empty())
error: expected `String`, found `String?`   (at return e)
```

Narrowing works after `if e is none { return }`, but not on the right of
`or`, and not after a combined guard.

**Why it matters.** `if x == nil || x.isEmpty` is the most common guard in
Go, TypeScript, Kotlin and Swift. Agents will write it first. The
workaround `(e ?? "").is_empty()` loses the narrowing afterward too.
(httpbin GAPS #36 reported the `some(x) or` half of this.)

**What others do.** TypeScript and Kotlin narrow through `||` and after
an early return. This is standard flow typing, and DESIGN.md already
promises "flow typing after `is`".

---

## 16. Sorting: no comparator, mixed keys impossible, and the hint makes it worse

**Severity:** medium. **Kind:** stdlib / error messages.

The GAPS for gron asked for list keys, and they were added. But list keys
must have one element type:

```
us.sorted_by(u => [u.name, u.age])
```
```
error: expected `String`, found `Int`
  = help: use interpolation: `"${x}"`, or `x.to_string()`
```

Following the hint sorts ages as text: `"10" < "9"`. The compiler's fix
brings in a bug.

There are other gaps. You can't sort "name ascending, then age
descending". Descending order is documented as `.reversed()`, which puts
equal keys in reverse order:

```
[ann:5, bob:9, cid:5].sorted_by(p => p.score).reversed()
→ ["bob", "cid", "ann"]      // cid now comes before ann
```

`sorted()` of records and `sorted_by(u => u.id)` with `id: UserId` are
also rejected ("`UserId` values can't be ordered").

**What others do.** Go has `slices.SortFunc(xs, func(a, b) int)` and
`cmp.Or`. Rust has `sort_by(|a, b| a.x.cmp(&b.x).then(b.y.cmp(&a.y)))`.
Python keys can be tuples of mixed types. Suggestion: record keys (records
compare field by field, which also fixes `UserId`), and a `descending`
wrapper, or a `sort_by(compare:)` form.

---

## 17. `Decimal` isn't exact, and it overflows at modest sizes

**Severity:** medium. **Kind:** semantics.

The docs say `+ - *` are exact. Multiplication silently rounds once the
scale passes 18 digits:

```
var x: Decimal = 19.99
let rate: Decimal = 0.0725
// x = x * rate, five times
```
```
0.000552286999609375
0.000040040807471680     // exact: 0.0000400408074716796875
```

And a product of two ordinary amounts panics:

```
let big: Decimal = 123456789012.34
print("${big * big}")
```
```
panic: Decimal overflow: more than 18 digits
```

**Why it matters.** Compounding interest, tax on tax, and currency
conversion chain multiplications. A panic in a handler is a 500 for
that request. The fixed 18 digits for the *whole number* (not only
the fraction) is small for intermediate results.

**What others do.** Java `BigDecimal`, Python `decimal` and C# `decimal`
(28-29 digits) either grow or give much more room, and they document
rounding in `*`. At minimum, fix "exact" in the docs and say where
rounding happens.

---

## 18. `with` closes on time, but a lambda can keep the closed resource

**Severity:** medium. **Kind:** semantics.

```
fn make() throws -> http.Router {
  var router = http.Router()
  with conn = try db.open(":memory:") {
    router.get("/", req => {
      let rows = try conn.query<Row>("select 1 as n", [])
      http.text(200, "${rows}")
    })
  }
  return router
}
```
```
error: db: the connection is closed      (at run time, on the first request)
```

A `with` variable captured by a lambda that outlives the block compiles
fine.

**Why it matters.** "Forgetting to close a file is a compile error" is a
headline claim. Use-after-close is its twin, and it's exactly what
happens when someone moves router setup into a helper function.

**What others do.** Rust's borrow checker rejects it. Plumb doesn't need
lifetimes: "a `with` variable can't be captured by a lambda that is
stored or passed anywhere except to a call inside the block" is a local
check, like the rule that a `Task` can't be captured.

---

## 19. Deadlock detection doesn't work in a server

**Severity:** medium. **Kind:** docs / semantics.

DESIGN.md says "a deadlock through function calls is reported at runtime
instead of hanging". It works in a toy program:

```
spawn ab(a, b: b)      // locks a, then b
spawn ab(b, b: a)      // locks b, then a
```
```
panic: deadlock: every task is waiting for another one
```

Add a background task that sleeps (any server has timers and a listening
socket), and the same deadlock is not reported. It hangs until the
sleeping task ends, 20 s later in my test. In a server that is never.

**Why it matters.** The two tasks that are stuck are exactly the
requests that hang. The docs make people think the runtime will catch it.
The README is precise about it ("nothing else (no timer, no socket) can
wake one"), but DESIGN.md and AGENTS.md aren't.

**What others do.** Go has the same limit ("all goroutines are
asleep"). But Go doesn't advertise it as a feature. A lock wait-for
graph (task A waits for lock L held by task B) catches lock cycles no
matter what else is running. That's cheap, because only `Shared` locks
are involved.

---

## 20. Structured concurrency blocks "fire and forget", and the docs don't show the pattern

**Severity:** medium. **Kind:** semantics / docs. *(I think the design is
right, but it's under-explained.)*

```
fn signup(req: http.Request) throws -> http.Response {
  spawn send_email("a@b.c")        // takes 2 s
  return http.text(201, "ok")
}
```
```
201 after 2s
```

The handler waits for its task, so the response is late. `spawn` inside a
lambda is also forbidden (`router.post("/s", req => { spawn ... })`).

**Why it matters.** "Send the welcome email after replying" is in every
backend. The right way (a `Channel` to a worker started in `main`) isn't
in AGENTS.md's HTTP section. An agent will write the code above, and it
compiles.

**What others do.** Go: `go sendEmail()`, which leaks by design. Kotlin:
you pick a scope (`applicationScope.launch`). Trio: pass a nursery. A
worked example "background jobs from a handler" in the guide would
close this.

---

## 21. `plumb test` doesn't test the project

**Severity:** medium. **Kind:** tooling.

DESIGN.md: "`plumb test` runs everything, with no configuration." In fact:

```
$ plumb test                 → prints the help text
$ plumb test .               → error: can't read .: Is a directory
$ plumb test main.plumb      → ok main  (1 passed)
```

`util.plumb`, imported by `main.plumb`, has a failing test. It doesn't
run. Unknown flags are ignored without a word (`plumb test f.plumb
--filter records` runs everything).

Also, `expect` works only directly in a `test` block. A helper can't
check things (RealWorld GAPS #15). A failing `expect` in a loop doesn't
say which iteration failed:

```
FAIL  loop
      expect failed at line 13: i * 2 != 4
      left: 4
     right: 4
```

**Why it matters.** An agent runs `plumb test main.plumb`, sees green, and
reports success, while tests in other files fail.

**What others do.** `go test ./...`, `cargo test` and `swift test`
run everything in the project by default.

---

## 22. Some DESIGN.md claims don't match the compiler

**Severity:** medium. **Kind:** docs.

Agents and reviewers take DESIGN.md as the spec. These claims are false
today:

- "a `Shared` can't contain another `Shared` in its value (a compile
  error)". It compiles, and the cycle leaks:
  ```
  type Node {
    name: String
    next: List<Shared<Node>> = []
  }
  let a = Shared<Node>(Node(name: "a"))
  with n = a.lock() { n.next.append(a) }
  ```
  ```
  $ plumb run --debug k6.plumb
  debug: 1 allocations, 1 not freed
  ```
- "Ordering (`<`) is built in for numbers, `String` and time." `Duration`
  can't be compared: `a < b` gives "`Duration` values can't be ordered".
  The workaround is `a.is_shorter_than(b)`.
- "you changed a copy that is never used afterward" (complaint 1).
- "`plumb test` runs everything" (complaint 21).
- "A lambda takes the expected type, the same way a number literal does",
  but a number literal doesn't take a new type:
  `let a: UserId = 7` → "expected `UserId`, found `Int`".

---

## 23. A never-thrown error type in `is` crashes the C compiler

**Severity:** medium. **Kind:** tooling (compiler bug).

```
type Oops implements Error {
  code: Int
  fn message(self) -> String { return "oops" }
}
fn f() throws -> Int {
  throw Failure(message: "x")
}
fn main() {
  let c = try f() catch err {
    if err is Oops { print("never") }
    0
  }
}
```
```
error: use of undeclared identifier 'Boxed4'
error: the C compiler failed on .../e2-25845-25845.c (this is a compiler bug)
```

It works as soon as some function throws `Oops`. That makes it a natural
state while writing code: you declare the error and the handler before
the thrower. Untyped `throws` also means `is Oops` is accepted even when
nothing can throw it. The compiler could warn instead of crashing.

---

## 24. A panic shows one line, and a thrown error shows no location

**Severity:** medium. **Kind:** tooling.

```
fn get(xs: List<Int>, i: Int) -> Int { return xs[i] }
fn middle(xs: List<Int>) -> Int { return get(xs, i: 10) }
```
```
panic: index 10 is out of range for a list of length 2
  at pn.plumb:2
```

There is no call stack. An error that escapes `main` prints only its
message: `error: can't read "/nope": No such file or directory`. There
is no file, line or chain of calls.

**Why it matters.** In a server, a bug becomes "a 500 and a log line".
If that line says `helpers.plumb:2`, and `get` is called from 40 places,
the log doesn't help. Untyped errors without locations make it worse.

**What others do.** Go and Rust (`RUST_BACKTRACE=1`) print full stacks on
panic. Java, Python and Swift errors carry a trace. The C backend
already knows the call sites. A shadow stack of call-site locations,
compiled only into panicking paths, would be enough.

---

## 25. No unused-variable or unused-import checks

**Severity:** medium. **Kind:** tooling / semantics.

```
import json
fn main() {
  let unused = 5
  var never_changed = 3
  print("${never_changed}")
}
```

It compiles cleanly. An unused import, an unused `let`, and a `var` that
is never changed get no error and no warning.

**Why it matters.** "Results must be used" is a headline rule. But
`let result = compute()` with `result` never read gets around it
silently. That's the same bug (a computed value forgotten) the rule
exists to catch. Agents often leave such dead bindings after edits.

**What others do.** Go: unused variables and imports are compile errors.
Rust and Swift warn, including "`var` never mutated, use `let`".

---

## 26. Number parsing accepts input it should refuse

**Severity:** medium. **Kind:** stdlib / semantics.

```
" 42".to_int()          → 42
"1_000".to_int()        → 1000
"nan".to_float()        → NaN
"1e400".to_float()      → Infinity
```

Then:

```
let f = try "nan".to_float()
let cents = (f * 100.0).round()
```
```
panic: can't turn NaN into a whole number
```

So a form field containing `nan` gets through parsing and crashes the
request. Accepting `_` makes sense in source code, not in user input.
`json.parse` rejects `1e400` (gron fix), but `to_float` accepts it, so the
two parsers disagree.

**What others do.** Go's `strconv.Atoi(" 42")` and `Atoi("1_000")` are
errors. Rust's `parse` rejects both. Parsing of external data should be
strict: a strict `to_int`/`to_float`, and `Float.round()` that throws
or returns `Int?` instead of panicking.

---

## 27. JSON decoding is fuzzy in a way attackers like

**Severity:** medium. **Kind:** stdlib (security).

```
{"NAME": "a", "Age": 5}                 → In(name: "a", age: 5, ...)
{"name": "a", "a_g-e": 5}               → age: 5
{"name": "a", "name": "b", "age": 5}    → name: "a"   (the first wins)
```

Keys match ignoring case, `_` and `-`, and the *first* duplicate wins.
Go, JavaScript and Python keep the *last* one. The differences are a
known source of parser-differential bugs. A proxy or validator in another
language checks `"role": "user"` (the last), and the Plumb service uses
`"role": "admin"` (the first). Fuzzy matching also means `is_admin`,
`isAdmin`, `IS-ADMIN` and `isadmin` all fill the same field.

**What others do.** Go matches case-insensitively too, and it's a
long-standing complaint (Go's `json/v2` makes it opt-in). Serde is
exact by default. Suggestion: reject duplicate keys, and keep the fuzzy
match as an option (`keys: CamelCase` exists already).

---

## 28. Regex: group 0 means different things in `find` and `replace`

**Severity:** medium. **Kind:** stdlib.

The stdlib review already covers the POSIX syntax gaps. Two new points:

```
let d = try regex.compile("(\\d+)-(\\d+)")
if d.find("12-34") is some(m) { print("groups[0]=${m.groups[0]} groups[1]=${m.groups[1]}") }
print(d.replace("12-34", replacement: "$1/$2"))
```
```
groups[0]=12 groups[1]=34
12/34
```

`m.groups[0]` is the first group, but in `replace`, `$0` is the whole
match and `$1` the first group. Every other regex API (Go, JS, Python,
Rust, PCRE) numbers the groups the same way in both places. Off-by-one
bugs will follow.

Second: an unsupported pattern *in a literal*
(`regex.compile("<(.+?)>")`) is found only at run time
(`error: regex: lazy quantifiers like *? aren't supported`). The compiler
checks SQL literals, so it could check regex literals too. There are no
raw strings either, so `\d` must be written `\\d`.

Also, a top-level `let` can't hold a compiled regex (it's a function
call), so a handler either compiles it on each request or has the regex
passed down from `main`.

---

## 29. Error messages: good on average, but some hints point the wrong way

**Severity:** minor. **Kind:** error messages.

Most messages are very good: `&&`, `!x`, `null`, `nil`, `len()`,
`toUpperCase`, `+` on strings and semicolons all get the right fix. The
misses:

| input | message | problem |
|---|---|---|
| `x++` | expected an expression, found `+` | no hint for `x += 1` |
| `c ? "a" : "b"` | hint lists `try`, `??`, `?.` | doesn't mention `if c { } else { }` |
| `y!` (force unwrap) | use `not x` instead of `!x` | wrong diagnosis |
| `[1].count` | call it: `.count()` | `count()` needs a test; the fix is `length` |
| `y.unwrap()` | `if x is some(v) { v.unwrap(...) }` | nonsense suggestion |
| `"a".find("l")` | did you mean `__find`? | suggests an internal method |
| `print(f"{x}")`, `` `a ${x}` `` | expected `)`, unexpected character | no hint for `"${x}"` |
| `x => {"a": x}` | expected the end of the line, found `:` | no hint for `x => ({"a": x})` |
| generic `x > b` | `T0` values can't be ordered | leaks an internal name |
| several | "text can't be indexed", "numbers and text" | the type is `String` now |
| `err is json.NotFound` | two errors for one mistake | duplicate |
| `(name: String) => ...` | lambda parameters have no type annotations | doesn't suggest `let f: fn(String) -> String = name => ...`, which works |
| help for `print(n)` | `"${x}"`, or `x.to_string()` | offers two ways |

`plumb doc String` and `plumb doc db` also list internal methods
(`__find`, `__rfind`, `__begin`, `__end`).

---

## 30. Changing through an interface-typed element doesn't work

**Severity:** minor. **Kind:** type system.

```
var xs: List<A> = [A()]
xs[0].bump()                 // works
var cs: List<Counter> = [A()]
cs[0].bump()                 // error: a mutating method needs a variable here
```

Paths through lists and maps work for records, but not for
interface-typed values. A `List<Plugin>` or `Map<String, Handler>` of
stateful things can't be updated in place. You have to take the value
out, change it and put it back, which is also a copy (complaint 4).

---

## 31. Local helpers are awkward: no nested functions, no annotated lambdas, no recursion

**Severity:** minor. **Kind:** syntax.

```
let greet = (name: String) => "hi ${name}"     // error: no type annotations
fn inner(x: Int) -> Int { ... }                 // inside a function: parse error
```

`let greet: fn(String) -> String = name => "hi ${name}"` works, but the
error doesn't say so. A local lambda can't call itself. So every helper
becomes a top-level function with all its context passed as parameters.

**What others do.** Go: `greet := func(name string) string {...}`.
Rust, Swift and Kotlin have nested functions. Suggestion: allow `fn`
inside a function. It's a construct that explains itself, and needs no
new concept.

---

## 32. Ranges exist only in `for`, with no reverse or step

**Severity:** minor. **Kind:** syntax.

```
for i in (0..<5).reversed() { }   // error: a range can only be used in `for`
let r = 0..<3                     // same error
```

Counting down (walking a string from the end, undo stacks) becomes a
`while` loop with manual `i -= 1`, which is the off-by-one-prone form
the ranges were meant to replace.

**What others do.** Rust: `(0..5).rev()`, `.step_by(2)`. Swift:
`stride(from:to:by:)`. Kotlin: `downTo`.

---

## 33. Common variable names are taken by modules, and there is no shadowing

**Severity:** minor. **Kind:** syntax. *(Probably right, but it has a
cost.)*

```
import path
import url
let path = "/tmp/a"     // error: `path` is the name of an imported module
                        //   = help: pick another name, like `path_value`
let x = 1
let x = x + 1           // error: `x` is already declared
```

`path`, `url`, `time`, `log`, `json`, `files`, `env` and `process` are
among the most natural local names in a CLI tool. `path_value` and
`url_value` are worse names. With qualified imports only, the clash can't
be resolved, only avoided. Go programmers know this pain (`url :=
url.Parse`), but Go at least allows it in inner scopes.

---

## 34. Inconsistent member shapes

**Severity:** minor. **Kind:** stdlib.

- `s.length` is a field, but `s.byte_length()` is a method:
  `error: byte_length is a method`.
- `xs.count` isn't the length, it's `count(test)`.
- `Failure` has a field `message` *and* a method `message()`.
- `Channel.receive()` both `throws` and returns `T?`.
- `task.wait()` needs `try` even when the task's function can't fail. So
  `main` must be `throws` to wait for pure computation.
- `List.chunks(0)` returns `[[], [], []]` instead of stopping as a bug.

Each one is small, but "the most predictable names" is a stated goal,
and agents learn from patterns.

---

## 35. Built-in types get operators, and user types can't

**Severity:** minor. **Kind:** type system. *(A fair trade-off, flagged.)*

`Decimal` has `+ - *`. `Duration` has neither operators nor `<`
(`a.plus(b)`, `a.is_longer_than(b)`). A user's `Money { cents: Int }` or
`Vector` also has no operators. The rule "no operator overloading" is
defensible. But then `Duration` should at least compare with `<`, as
DESIGN.md says "time" does (complaint 22).

---

## 36. Whole-program C compilation: every edit recompiles everything

**Severity:** medium. **Kind:** tooling / performance. *(Measured on small
programs, then extrapolated. TODO already has "compile time vs Go".)*

```
gron port (3,000 lines of Plumb → 25,000 lines of C):
  plumb build after a one-line edit:  2.0 s
  plumb run after a one-line edit:    1.8 s
```

One C file per program is great for inlining. But the cost grows with
program size, not with the size of the edit, and there is no incremental
build. `plumb check` is instant (13 ms), which helps the agent loop. But
`plumb test` and `plumb run` pay the full clang cost each time. At 30-50k
lines (a real service), that points to 20+ seconds per test run.

**What others do.** Go compiles per package with a cache and links
quickly. Swift and Rust use incremental compilation. Suggestions:
`-O0` for `run`/`test`, or per-module C files with the cache.

---

## 37. String building in a loop is quadratic, and the compiler suggests that form

**Severity:** minor. **Kind:** performance.

`+` on strings is an error, and its help says to use `"${a}${b}"`. In a
loop:

```
var out = ""
for i in 0..<400000 { out = "${out}x" }
```
```
40,000: 15 ms     400,000: 1.3 s
```

The value is unique (reference count 1), so it could be appended in place,
as `xs.append` is. Swift makes `s += x` amortized O(1). Go makes you use
`strings.Builder`. Either do the in-place append for `out = "${out}..."`,
or have the hint say "in a loop, collect parts and `join`".

---

## 38. Small things a Go/TS/Rust person will trip on

**Severity:** minor. **Kind:** syntax / semantics.

- **`Map<K, V?>` reads are ambiguous.** For `{"a": none}`, `m["a"]`
  prints `none`, and `if m["a"] is some(x)` matches with `x = none`.
  You need `contains_key` to tell "missing" from "stored none".
- **Adding an enum can break code elsewhere in the file.** `let s =
  Pending` works until a second enum with a `Pending` variant appears.
  Then it's an error ("a variant of several enums"). A record type named
  like a variant gives the confusing "`Pending` is a type, not a value".
- **`Float.round()` returns `Int`** and panics above 2⁶³ or on NaN. So
  rounding a float for display (`(x * 100.0).round()`) can stop a
  request. `Float` has no `to_int`, `trunc` or `is_nan` (`is_nan` lives
  in `math`).
- **`with x = try open() catch err { ... } { ... }`** puts two brace
  blocks in a row. It's the hardest construct in the guide to read.
- **No record update syntax.** RealWorld has `Article` and `Summary`
  with 9 of 10 fields in common and two 10-line copy functions
  (`full`, `summary`). Something like `Summary(from: article)` or
  `{...a, b: 3}` would remove it.
- **Mixed `Int`/`Float` math needs `.to_float()` everywhere.** This is
  right for safety, but averages and percentages read noisily. *(Probably
  the right design, flagged only for the friction.)*

---

## Summary of what I would fix first

1. The "changed a copy" error (1) and name-based pattern binding (2).
   Both are cheap, and both close silent-wrong-answer holes.
2. The string index cliff (3) and a "full copy here" diagnostic (4).
   Both are hidden O(n²) costs that tests don't catch.
3. A read lock for `Shared` (5), and bounds for generics (6).
4. Literal-built SQL with list expansion (7).
5. Make DESIGN.md match the compiler (22), and make `plumb test` run the
   project (21).
