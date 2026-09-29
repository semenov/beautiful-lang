# Linen

*Familiar at first glance. Hard to misuse. Small and fast when it runs.*

Linen is a small, statically typed language. A programmer who knows Python,
TypeScript, Kotlin or Swift should be able to read it without a tutorial.
It uses indentation instead of braces and needs no semicolons. It has no null
and no hidden exceptions, and there is exactly one way to write each thing.

Linen compiles to **small native binaries**. It has no garbage collector and
no virtual machine, and it calls C libraries directly. JavaScript is available
as a secondary target.

```
# Reduce a total by a discount, never below zero.
fn apply(discount: Discount, total: Decimal) -> Decimal
  require total >= 0
  return match discount
    case NoDiscount => total
    case Percent(rate) => total - total * rate / 100
    case Fixed(amount) => max(total - amount, 0)

test "discounts never go below zero"
  expect apply(Fixed(50), total: 30) == 0
```

- The standard library is described in [`STDLIB.md`](STDLIB.md).
- A compact cheat sheet for AI agents is in [`AGENTS.md`](AGENTS.md).
- Experiments with models writing Linen are in [`experiments/`](experiments).
- An earlier, prose-like version of the design is kept in
  [`drafts/prose-edition`](drafts/prose-edition).

---

## Principles

1. **Familiar first.** Linen only borrows syntax that most programmers
   already know: `fn`, `let`, `if`, `for`, `match`, `.`, `=>`, `<T>`.
2. **One way to write it.** The formatter is part of the language. There's one
   loop form, one way to build a value and one way to handle an error.
3. **Local truth.** There's no null, no unchecked exceptions, no inheritance
   and no macros. Each line does what it says.
4. **Values by default.** Ordinary data is never shared, so a change is never
   seen elsewhere. The only exception is a `resource`, which is a handle to
   something that lives outside, like a connection or a store.
5. **Explicit edges.** Anything that can fail is marked twice: with `throws` in
   the signature and with `try` at the call site. Untyped data from outside
   enters the program only through `decode`.
6. **Intent next to code.** Contracts (`require`, `ensure`) and tests (`test`)
   sit right beside the functions they describe.
7. **Lean at runtime.** Memory is freed as soon as it's no longer needed, and
   there are no pauses and no virtual machine. Readable code shouldn't cost
   speed.

---

## Values

```
let name = "Ada"
let count = 42                      # Int
let ratio = 0.75                    # Float
let price: Decimal = 19.99          # a literal takes the expected number type
let ready = true
let primes = [2, 3, 5, 7]           # List<Int>
let ages = {"ada": 36, "alan": 41}  # Map<Text, Int>: keys in quotes
let origin = (0, 0)                 # tuple (Int, Int)
let greeting = "Hello, {name}"      # interpolation; "{user.name ?? "anon"}" works too
let poem = """
  Text over several lines
  keeps its line breaks.
  """
```

Comments start with `#`. Names use `snake_case`. Types and variants use
`PascalCase`. Text uses double quotes only.

**Numbers.**
- `Int` is 64-bit. Overflow is a bug and stops the program, rather than
  silently wrapping around.
- `Float` is a 64-bit IEEE float.
- `Decimal` is an exact 128-bit decimal (28 significant digits), meant for
  money.
- A number literal takes whatever number type is expected. In arithmetic, an
  `Int` widens to `Float` or `Decimal` automatically. `Float` and `Decimal`
  never mix.
- `/` on two `Int`s gives a `Float`. For whole-number division use `a.div(b)`,
  and for the remainder use `a % b`.
- Nothing else converts on its own. Use `to_float()`, `to_decimal()`,
  `to_text()`, `round()` and `parse_int(text)` (see [`STDLIB.md`](STDLIB.md)).

**Braces.** `{"key": value}` with quoted keys is a `Map`. `{key: value}` with
bare keys is an object literal that builds whatever record type is expected at
that spot.

## Names

```
let limit = 10        # can't change
var total = 0         # can change
total += 5
var seen: Set<Text> = {}
```

You can't declare the same name twice in one scope (no shadowing). If a value
needs to change, use `var`.

---

## Functions

```
fn greet(name: Text)
  print("Hello, {name}")

fn area(width: Float, height: Float) -> Float
  return width * height

fn double(n: Int) -> Int = n * 2           # single-expression form

fn first_or<T>(items: List<T>, fallback: T) -> T = items.first() ?? fallback
```

A generic function always declares its type parameters: `fn name<T>(…)`.

### Argument names

A function with one or two parameters is called positionally. When it has
three or more, every argument after the first is named:

```
area(3, 4)
move(book, from: shelf, to: box)
text.replace(old: "-", new: "_")          # the receiver counts as the first argument
```

This is a **formatting rule, not a compile error**. `linen fmt` adds missing
names automatically, so code without them still compiles, and after
formatting it always reads the same way. Calls through a function value
(`f(a, b, c)`) stay positional, because function types don't carry parameter
names.

### Method syntax

Any function can be called on its first argument: `x.f(y)` is exactly
`f(x, y)`.

```
let names = users.filter(u => u.active).map(u => u.name).sort()
```

How `x.name` is resolved:

1. Without parentheses, `x.name` is always a field.
2. With parentheses, `x.name(...)` is a call to a function `name` whose first
   parameter accepts the type of `x`. The compiler looks in the current module,
   then in imports, then in the prelude.
3. Several functions can share a name if their first parameter types differ.
   That's the only overloading in Linen.
4. If `x` has a field with a function type and a function of the same name
   also exists, the compiler reports an error rather than picking one.

A line that starts with `.` continues the expression above it:

```
let report = orders
  .filter(o => o.paid)
  .sort_by(o => o.total)
  .take(10)
```

### Changing arguments: `var` parameters

A function may change a value that was passed in, but only if the parameter
is marked `var` and the caller passes a `var`:

```
fn add_tag(tags: var List<Text>, tag: Text)
  if not tags.contains(tag)
    tags.append(tag)

var tags = ["new"]
tags.add_tag("urgent")     # tags is now ["new", "urgent"]
```

### Functions as values

```
type Rule = fn(Decimal) -> Decimal             # type alias

fn percent_off(rate: Decimal) -> Rule = total => total - total * rate / 100

fn combine(rules: List<Rule>) -> Rule
  return total => rules.fold(start: total, step: (acc, rule) => rule(acc))

let costs = order.items.map(cost)              # a named function passed as a value
```

- **Lambdas** are `x => expr` or `(a, b) => expr`. When a lambda needs
  several lines, indent them. The value of the last line is the result.
- **Tuples in lambdas.** When a lambda receives a tuple, `(a, b) =>` unpacks
  it: `counts.entries().map((word, n) => "{word}: {n}")`.
- **Function types** are `fn(Int, Text) -> Bool`, `fn(T) throws -> R`, or
  `fn()` for no arguments and no result.
- **Captured variables are copied** when the lambda is created. Changing a
  captured `var` inside a lambda is a compile error.
- **Choosing an overload.** If a function name is overloaded, the expected
  type picks the right version.
- **Functions can't be compared.** `f == g` is a compile error.
- **Effects pass through.** If a lambda contains `try`, the call it's passed
  to `throws` too.

---

## Types

### Records

```
type User
  name: Text
  email: Text
  born: Int
  nickname: Text? = none         # a default value; can be left out

let ada = User(name: "Ada", email: "ada@example.com", born: 1815)
let older = ada.with(born: 1800) # a copy with one field changed
```

### Enums

```
enum Shape
  Circle(radius: Float)
  Rectangle(width: Float, height: Float)

enum Status
  Draft
  Published
  Archived
```

### Tuples

Tuples are for two or three values that belong together. For more, use a
record. Tuples are only unpacked, never indexed:

```
fn min_max(xs: List<Int>) -> (Int, Int)
  return (xs.min() ?? 0, xs.max() ?? 0)

let (low, high) = min_max(scores)
```

### Type aliases

`type Name = OtherType` gives an existing type a second name. It doesn't
create a new type:

```
type Rule = fn(Decimal) -> Decimal
type Grid = List<List<Int>>
```

### Generics

```
type Stack<T>
  items: List<T> = []

fn push<T>(stack: Stack<T>, item: T) -> Stack<T> = stack.with(items: stack.items + [item])

let empty = Stack<Int>()
```

### Resources

`type` values are never shared. A **`resource`** is a handle to something that
lives outside the value world: a database connection, an open file, a cache, a
store in memory.

```
resource MemoryStore
  var data: Map<Text, Bytes> = {}

fn put(store: MemoryStore, key: Text, value: Bytes)
  store.data[key] = value          # changes the shared store
```

- Copying a resource copies the handle, and every copy sees the same state.
- Only `var` fields of a resource can change, and they can change through any
  handle.
- **Automatic cleanup.** If a function `fn close(r: MyResource)` exists, it
  runs when the last handle disappears. Files and connections close by
  themselves, at a predictable moment.
- Resources can't be compared with `==`, encoded to JSON or used as map keys.
- A resource field that points "back up" (child → parent) must be `weak`, so
  resources don't keep each other alive forever (see [Memory](#memory)).

Looking at a type is enough to know whether a value can change behind your
back: `type` never, `resource` yes.

### Optional values

There is no null. A value that can be missing has type `T?`. A plain `T` can be
used wherever a `T?` is expected:

```
fn find(users: List<User>, email: Text) -> User?
  for user in users
    if user.email == email
      return user                  # User becomes User? automatically
  return none

let nick = user.nickname ?? user.name
let city = user.address?.city      # Text?
if find(users, email) is some(user)
  print("found {user.name}")
```

`some(x)` appears only in patterns.

---

## Control flow

```
if age >= 18
  print("welcome")
else if has_guardian
  print("welcome, with company")
else
  print("sorry")

for user in users
  print(user.name)

for n in 1..=10            # 1 to 10, inclusive
  print(n)

for i in 0..<count         # 0 to count - 1
  print(i)

for (i, line) in lines.indexed()
  print("{i}: {line}")

for (name, age) in ages    # iterating a Map gives (key, value)
  print("{name} is {age}")

while fuel > 0
  fuel -= burn_rate
```

There are two range forms: `..=` includes the end and `..<` excludes it. A bare
`..` is a compile error. `break` and `continue` work as usual.

**`if` as a value.** The short form is on one line. The long form uses blocks,
and the last line of each block is the value:

```
let label = if age >= 18 then "adult" else "child"

let fee = if member
  0
else if order.total > 100
  5
else
  order.total * 0.1
```

## Pattern matching

`match` is an expression. The compiler checks that it covers every case.

```
fn area(shape: Shape) -> Float
  return match shape
    case Circle(r) => PI * r ^ 2
    case Rectangle(w, h) => w * h

fn describe(point: (Int, Int)) -> Text
  return match point
    case (0, 0) => "origin"
    case (x, 0) => "on the x axis at {x}"
    case (x, y) if x == y => "on the diagonal"
    case _ => "somewhere else"
```

When an arm needs several lines, put them in an indented block after `=>`,
with the value on the last line. `x is Pattern` checks a single pattern and
gives a `Bool`: `if shape is Circle(r)`.

---

## Errors

Errors are values, not hidden jumps. A function that can fail says so. **Every
call to it starts with `try`.** On its own, `try` passes the error up.
Followed by `catch`, it handles the error right there:

```
fn load_settings(path: Text) throws -> Settings
  let text = try files.read(path)                    # pass the error up
  let port = try parse_int(text) catch 8080          # or use a fallback
  if port < 1
    throw Error("port must be positive, got {port}")
  return Settings(port)
```

**What can follow `catch`:**

```
let port = try parse_int(text) catch 8080                         # a value
let age = try parse_int(field) catch continue                       # skip this iteration
let user = try find_user(id) catch return none                      # leave the function
let rows = try db.query(sql) catch err => throw Unavailable(err.message)   # translate
let config = try load_config(path) catch err                        # a block; its last line is the value
  print("using defaults: {err.message}")
  Config()
```

**`try`/`catch` blocks** handle a whole section:

```
try
  let settings = try load_settings("app.conf")
  start_server(settings)
catch err
  print("could not start: {err.message}")
```

**Typed errors.** `throws` on its own means `throws Error`, where `Error` has
a `message` field. A function can name its own error type:

```
enum StorageError
  NotFound(key: Text)
  Unavailable(reason: Text)

fn get(store: Store, key: Text) throws StorageError -> Bytes
```

- Inside a `throws Error` function, `try` accepts any error type and converts
  it to `Error`.
- Inside a `throws StorageError` function, every `try` must throw
  `StorageError`. Any other error has to be caught and translated.
- In a `catch err` block, `err` has the declared error type, so you can use
  `match err` on it.

Calling a `throws` function without `try` is a compile error.

## Contracts and tests

```
fn withdraw(account: Account, amount: Decimal) throws -> Account
  require amount > 0
  ensure result.balance >= 0
  if amount > account.balance
    throw Error("insufficient funds")
  return account.with(balance: account.balance - amount)
```

- `require` and `ensure` come first in a function body. `result` in `ensure`
  is the value being returned.
- **A failed contract is a bug, not an error.** The program stops with a
  message, and `catch` can't intercept it.
- **How to choose:** if a *correct* caller could cause the problem (not
  enough money, a malformed file, a network error), use `throw`. If only a
  *buggy* caller could cause it (a negative amount), use `require`.

```
test "withdraw takes money out"
  let account = Account(balance: 100)
  expect (try account.withdraw(30)).balance == 70

test "withdraw refuses to overdraw"
  let err = expect throws Account(balance: 10).withdraw(30)
  expect err.message.contains("insufficient")

test "withdraw rejects a negative amount"
  expect throws Account(balance: 10).withdraw(-5)    # a failed require counts here
```

`expect throws <call>` passes only if the call throws **or breaks a contract**,
and it gives back the error for further checks. Catching a contract failure
is possible only this way, inside a `test`. For typed errors, use
`expect err is NotFound(_)`. `linen test` runs every `test` block.

---

## Interfaces and generic constraints

Linen has no classes and no inheritance. Shared behavior is described by an
**interface**. A type satisfies an interface automatically when the functions
it needs exist, and every such function takes the value as its first
parameter (`self` in the interface):

```
interface Describable
  fn describe(self) -> Text

type Point
  x: Float
  y: Float

fn describe(point: Point) -> Text = "({point.x}, {point.y})"
fn describe(user: User) -> Text = user.name
```

**Use an interface as a type directly.** This is the usual case:

```
fn print_all(items: List<Describable>)
  for item in items
    print(item.describe())

let items: List<Describable> = [Point(1, 2), ada]     # different types in one list
print_all(items)
```

**Several interfaces at once** are joined with `+`:

```
fn copy_keys(from: Storage + Listable, to: Storage, prefix: Text) throws StorageError
  for key in try from.keys(prefix)
    if try from.get(key) is some(value)
      try to.put(key: key, value: value)
```

`from` and `to` can have different types, for example copying from a
`MemoryStore` into a `FileStore`.

**Use generics `<T: I>` only when types must be the same.** For example, the
result must have the same type as the argument, or all elements must share
one type:

```
fn loudest<T: Describable>(items: List<T>) -> T? = items.max_by(x => x.describe().length)
```

A few more rules:
- **Shared logic** is written as an ordinary function over the interface, and
  method syntax makes it available on every type that satisfies it:
  `fn shout(x: Describable) -> Text = x.describe().upper()`. Interfaces have
  no default methods.
- **Generic interfaces:** `interface Container<T>` with
  `fn add(self, item: T) -> Self`.
- **There's no way back.** An interface value can't be cast back to a concrete
  type. If you need that, use an enum.
- **Enum or interface?** If all variants are known in advance, use an enum.
  If other modules should be able to add their own types, use an interface.

**Built-in behavior:**
- `==` compares any record, enum, tuple or list element by element.
- `<`, `sort()` and `max()` work on numbers, `Text` and tuples of them. For
  anything else, sort by a key: `sort_by(u => u.name)`.
- `"{x}"` prints any value in a standard form. If a function
  `to_text(x: MyType) -> Text` exists, it's used instead.
- Any type that supports `==` can be a map key.

There's no operator overloading and no overloading on anything but the first
argument.

---

## Doing things in parallel

```
parallel
  let paris = try forecast("Paris")
  let tokyo = try forecast("Tokyo") catch none    # this branch recovers on its own
print("{paris.degrees}° in Paris")

let pages = try urls.parallel_map(url => try web.get(url))           # over a list
let users = names.parallel_map(n => try fetch_user(n) catch none)    # List<User?>
```

- Every `let` inside `parallel` runs at the same time. The block ends when all
  of them have finished, and their names stay visible afterwards.
- `parallel_map` does the same for every element of a list, and keeps the
  results in order.
- If a branch fails without handling it, the other branches are cancelled and
  the error goes to the enclosing function. `catch` keeps a failure local to
  one branch.
- `return`, `break` and `continue` aren't allowed inside `parallel`.

Linen code has no `async`/`await`. Waiting on the network or disk doesn't
block a thread (see [Runtime](#runtime)).

## Modules

```
import json
import web
import shop.pricing            # file shop/pricing.ln

pub type Invoice …
pub fn total(invoice: Invoice) -> Decimal …
```

Each file is a module, and standard modules (`files`, `web`, `json`, `time`,
…) need `import` too. Definitions are private unless marked `pub`. Top-level
statements in the entry file make up the program.

## JSON and other untyped data

Untyped data gets into the program in exactly one way: `decode` it into a
type.

```
type Order
  id: Text
  customer: Text @json("customer_name")    # explicit key
  items: List<Line>
  note: Text?                              # key may be missing

let order = try json.decode<Order>(text)
let text = json.encode(order)
```

Keys are matched to fields regardless of style (`created_at`, `createdAt` and
`CreatedAt` all match the field `created_at`). Extra keys are ignored. When
decoding fails, the error gives the exact path:
`items[2].price: expected Decimal, got "12"`.

---

## Memory

Linen has no garbage collector. Memory is freed **as soon as the last
reference disappears**, so there are no pauses and no heap reserved in
advance.

- **Reference counting.** Every value keeps a count of references. Copying a
  value only increments the count. It doesn't copy the data.
- **Copy on write.** Data is copied only when you change a value that someone
  else also refers to.
- **Change in place when possible.** If a value has only one owner, then
  `user.with(...)`, `list + [x]` and `xs.append(x)` change it in place, with
  no copy and no new allocation. You write in the safe "a copy every time"
  style, and the machine code modifies memory directly. This technique is
  known as Perceus, from Koka and Lean 4.
- **The compiler removes unneeded counting.** A value that's only borrowed
  for a call doesn't touch the counter.
- **Cycles.** Values can't form cycles, because they never change after
  creation. Resources can, which is why a back-pointer must be `weak`
  (`weak parent: Node?`). The compiler reports an error if a resource can
  hold itself through strong fields.
- **Compact data.** Numbers, `Bool`, small records and tuples are stored
  directly, not in separate boxes. A `List<Point>` is one contiguous block of
  memory.
- **Specialization.** Generic functions are compiled separately for each type
  they're used with, which removes the cost of generics at runtime. Calls
  through an interface value go through a small method table.

## Runtime

- **Compilation.** Linen is compiled to C and then to machine code by
  Clang/GCC. The result is a single statically linked binary with no virtual
  machine and no JIT warm-up. Cross-compiling works through `zig cc`.
- **Our own allocator.** Linen ships a mimalloc-class allocator with
  per-thread free lists instead of the system `malloc`. The macOS system
  allocator can hold on to freed memory, and it's slow at allocating many
  small objects (see [`benchmarks/`](benchmarks)).
- **Lightweight tasks.** `parallel` and `parallel_map` start tasks that a
  scheduler spreads over every processor core, as in Go.
- **Non-blocking waiting.** Network and disk I/O go through epoll, kqueue or
  io_uring. A task waiting on the network doesn't hold a thread, and the code
  looks synchronous.
- **Safety across threads.** Values can be shared between tasks safely,
  because they never change. A value's counter becomes atomic only once the
  value actually reaches another task.
- **Resources and tasks.** Every read and write of a resource's `var` field is
  atomic. How to make a multi-step change atomic is an
  [open question](#open-questions).

**Goals** (not measurements yet): speed and memory in the same range as Rust
and Swift, and noticeably below Go. The runtime for a small program should
take a few megabytes, not tens. The comparison with JavaScript runtimes, Go,
Rust and C is in [`benchmarks/`](benchmarks).

---

## Interop

### C libraries

Linen calls C directly, with no wrapper layer at runtime. An `extern c` block
declares what's needed:

```
extern c "sqlite3"                          # links libsqlite3
  opaque Sqlite3 closed by sqlite3_close    # a C pointer, managed as a resource
  fn sqlite3_open(filename: CText, db: out Sqlite3?) -> CInt
  fn sqlite3_exec(db: Sqlite3, sql: CText, callback: CPtr?, arg: CPtr?, error: out CText?) -> CInt
  fn sqlite3_errmsg(db: Sqlite3) -> CText
```

- **`opaque T closed by f`** declares a C pointer type. It behaves like a
  `resource`, and `f` is called when the last handle disappears.
- **`out`** marks an output parameter. At the call site you pass a `var`.
- **`CText`, `CInt`, `CPtr`** are the C types. `Text` converts to `CText` and
  back automatically, with a copy.
- **Names match the C names exactly.**
- **C calls don't throw.** C reports failure through return codes, and
  translating them into `throw` is the wrapper's job.
- `linen bind sqlite3.h` generates an `extern c` block from a header file.

`extern c` is a trusted boundary: C can crash the program, and Linen can't
check it. So the convention is to write the declarations once, in a small
module, and give the rest of the program an ordinary Linen API:

```
pub resource Database
  handle: Sqlite3

pub fn open_database(path: Text) throws -> Database
  var handle: Sqlite3? = none
  if sqlite3_open(path, db: handle) != 0
    throw Error("cannot open {path}")
  return Database(try handle.or_throw("sqlite returned no handle"))
```

**How types map between the two languages**

| C | Linen |
|---|---|
| `int8_t` … `int64_t`, `int` | `Int8` … `Int64`, `CInt` (converted to `Int` explicitly) |
| `double`, `float` | `Float`, `Float32` |
| `const char*` | `CText` (converted to and from `Text` automatically) |
| pointer to an opaque struct | `opaque` type (a resource) |
| struct passed by value | `type` with `@c` (fields in C order) |
| `T* out` | `out T?` |
| a callback with no context | `fn` without captured variables |
| `void*` | `CPtr` |

Rust, Zig and Go libraries that export a C ABI connect the same way.

### WebAssembly

The same compiler produces a WebAssembly module for the browser and for
edge runtimes. Talking to modules in other languages through WebAssembly
components (WIT) is planned.

### JavaScript as a second target

`linen build --target js` compiles a program or library into an ES module with
`.d.ts` types. This is for integrating with the npm world. It's not how Linen
normally runs.

- `import js "pkg"` works **only** when building for this target. A library
  meant for every platform doesn't use it.
- The package name becomes the module name, with `-` replaced by `_`
  (`date-fns` → `date_fns`). Foreign names are used unchanged, and calls need
  `try`.
- A foreign `Promise<T>` looks like a plain `T`. `any` becomes `Dynamic`,
  which you turn into a type with `try decode<T>(raw)`. JavaScript globals are
  available as `js.Date()` and so on.
- `pub` functions are exported. Records become frozen objects, enums become
  tagged unions with `kind`, and `throws` becomes a thrown `LinenError`.
- `Int` in JavaScript is a `BigInt`, and `Decimal` is a runtime object. That
  makes number-heavy code on this target noticeably slower than native.

---

## Syntax at a glance

**Keywords:**

```
let  var  fn  return  type  enum  resource  interface  self  Self  weak
if  then  else  match  case  for  in  while  break  continue
try  catch  throw  throws
require  ensure  result  test  expect
parallel  import  pub  extern  c  js  opaque  out  closed  by  as
and  or  not  is  some  none  true  false
```

**Operators:** `+ - * / % ^`, `== != < > <= >=`, `and or not`, `??`, `?.`,
`=>`, `..=`, `..<`, `+= -= *= /=`, and `+` between interfaces.

**Layout:** indent with 2 spaces. A line continues the previous one when it
starts with `.` or an operator, or when it's inside open brackets. Trailing
commas are allowed, and the formatter adds them to multi-line lists.

---

## Why agents like it

- **Familiar tokens.** In two experiments ([run 1](experiments/run1/REPORT.md),
  [run 2](experiments/run2/REPORT.md)), 64 programs written by Haiku and
  Sonnet had no syntax slips from Python or TypeScript.
- **Rules follow habits.** `try … catch 0`, `expect throws`, type aliases and
  `catch` with a block were added because models reached for them on their
  own.
- **One canonical form.** Formatting rules (like argument names) are applied
  by `linen fmt`, not reported as errors.
- **Nothing is hidden.** `try` marks every call that can fail, `T?` every
  missing value, `Dynamic` every untyped value, and `resource` every shared
  state.
- **Tests and contracts in the source** give an agent a spec to work to and a
  fast way to check its work.
- **Errors that suggest fixes**, including for common habits from other
  languages (see [`AGENTS.md`](AGENTS.md)).

---

## Open questions

- **Multi-step changes to resources across tasks.** Should calls on one
  resource run one at a time, like an actor, or should there be an explicit
  `lock r` block?
- **Should the standard HTTP/TLS stack be native** (our own client on top of
  the OS) or use libcurl and OpenSSL?
- **Backend: C, or LLVM directly?** C is simpler and more portable. LLVM
  gives better debug information and faster compilation.
- **Automatic `extern c` from headers** at build time, as Zig does, or always
  a pre-generated file?
- Should `fn` bodies be allowed to end with an implicit last expression
  instead of `return`?

See [`examples/`](examples) for complete programs.
