# Standard library

Generated from the sources by `tools/stdlib_doc.py`: the public
declarations with their comments, bodies left out. `import <module>`,
then use `module.name`. Functions without a body are built into the
compiler and runtime.

Modules: [`files`](#files), [`path`](#path), [`io`](#io), [`process`](#process), [`env`](#env), [`cli`](#cli), [`term`](#term), [`log`](#log), [`time`](#time), [`json`](#json), [`http`](#http), [`net`](#net), [`sql`](#sql), [`db`](#db), [`crypto`](#crypto), [`encoding`](#encoding), [`random`](#random), [`regex`](#regex), [`csv`](#csv), [`xml`](#xml), [`template`](#template), [`url`](#url), [`zlib`](#zlib), [`archive`](#archive), [`math`](#math), and the [prelude](#prelude)
(available everywhere without `import`).

## files

files: read and write files and directories. Paths are text; errors say
which path failed and why.

```
// The whole file as text.
pub fn read(path: String) throws -> String
// The whole file as bytes.
pub fn read_bytes(path: String) throws -> Bytes
// Creates or replaces the file.
pub fn write(path: String, text: String) throws
pub fn write_bytes(path: String, data: Bytes) throws
// Adds to the end of the file, creating it if needed.
pub fn append(path: String, text: String) throws
pub fn exists(path: String) -> Bool
pub fn is_dir(path: String) -> Bool
// Names of the entries in a directory, sorted.
pub fn list(dir: String) throws -> List<String>
// Every file under a directory, at any depth, as paths starting with
// `dir` ("src/a/b.lang"; for "." just "a/b.lang"), sorted. Symlinked
// directories aren't followed.
pub fn walk(dir: String) throws -> List<String>
// The files matching a pattern, sorted: "*.txt", "src/**/*.lang",
// "logs/2026-*.{log,gz}" (see `path.matches`).
pub fn glob(pattern: String) throws -> List<String>
```

## path

path: the text of file paths ("a/b/c.txt"). Nothing here touches the
disk; reading and writing is in `files`.

```
// "a/b" + "c.txt" -> "a/b/c.txt"; an absolute `name` replaces `dir`
pub fn join(dir: String, name: String) -> String
// "a/b/c.txt" -> "c.txt"
pub fn name(path: String) -> String
// "a/b/c.txt" -> "a/b"
pub fn parent(path: String) -> String
// "a/b/c.txt" -> "txt"; none without a dot
pub fn extension(path: String) -> String?

// "a/b/c.txt" -> "c"
pub fn stem(path: String) -> String

pub fn is_absolute(path: String) -> Bool

// "/a/b/c.txt" -> ["a", "b", "c.txt"]
pub fn parts(path: String) -> List<String>

// Removes "." and resolves "..": "a/./b/../c" -> "a/c"
pub fn clean(path: String) -> String

// Whether a path matches a glob pattern: `*` is any text within one part of
// the path, `?` one character, `**` any number of parts (including none),
// `[a-z]` / `[!a-z]` one character from a set, `{a,b}` either text.
//   matches("src/net/tcp.lang", pattern: "src/**/*.lang") == true
pub fn matches(path: String, pattern: String) -> Bool
```

## io

io: streams of bytes, read and written piece by piece.

Files (`files.open`), network connections (`net.connect`) and this
program's standard input and output are all a `Stream`:

```
// a filter: `cat app.log | errors`
while try io.read_line() is some(line) {
  if line.contains("ERROR") {
    print(line)
  }
}
```

```
with from = try files.open("big.iso") {
  with to = try files.create("copy.iso") {
    let _ = try io.copy(from, to)
  }
}
```

```
pub builtin type Stream {
  // Up to `max` bytes, as soon as some are there; empty at the end.
  fn read(self, max: Int) throws -> Bytes
  // Exactly `count` bytes; an error if the stream ends first.
  fn read_exact(self, count: Int) throws -> Bytes
  // The next line without "\n" (or "\r\n"); none at the end.
  fn read_line(self) throws -> String?
  // Everything up to the end.
  fn read_all(self) throws -> Bytes
  fn write(self, data: Bytes) throws
  fn write_text(self, text: String) throws
  // For network connections, the other side's address: "93.184.215.14:80";
  // empty for other streams.
  fn peer(self) -> String
  // Writes what is still buffered and closes the stream.
  fn close(self) throws
}

// This program's standard input, output and error output.
pub fn stdin() -> Stream
pub fn stdout() -> Stream
pub fn stderr() -> Stream

// The next line of standard input; none at the end.
pub fn read_line() throws -> String?
// All of standard input.
pub fn read_all() throws -> Bytes

// Copies everything from one stream to another; returns the number of bytes.
// A file to a network connection goes straight from the disk (sendfile).
pub fn copy(from: Stream, to: Stream) throws -> Int
```

## process

process: this program, and running other programs.

```
pub type Output {
  // the exit status: 0 means success
  status: Int
  stdout: String
  stderr: String
}

// Runs a program and waits for it to finish. The arguments are passed as
// they are: there is no shell, so nothing needs quoting. A program that
// exits with a non-zero status is not an error: check `output.status`.
pub fn run(program: String, args: List<String>) throws -> Output

// A running program; get one with `with p = try process.start(...)`.
// Reading reads its output (stdout); writing writes to its input (stdin).
// Its error output goes to this program's error output. Leaving `with`
// stops it if it is still running.
//
//   with p = try process.start("sort", []) {
//     try p.write_text("b\na\n")
//     try p.close_input()
//     while try p.read_line() is some(line) { print(line) }
//   }
pub builtin type Process {
  fn read(self, max: Int) throws -> Bytes
  fn read_line(self) throws -> String?
  fn read_all(self) throws -> Bytes
  fn write(self, data: Bytes) throws
  fn write_text(self, text: String) throws
  // Tells the program there is no more input.
  fn close_input(self) throws
  // Waits for the program to finish and returns its exit status. Output it
  // writes meanwhile is kept for reading.
  fn wait(self) throws -> Int
  fn close(self) throws
}

// Starts a program without waiting for it (see `run` for the arguments).
pub fn start(program: String, args: List<String>) throws -> Process

// A program to run with more settings than `run` takes.
pub type Command {
  program: String
  args: List<String> = []
  // the directory to run it in; "" for the current one
  dir: String = ""
  // environment variables it gets on top of this program's
  env: Map<String, String> = {}
  // given as its standard input
  input: String = ""
}

// Runs a `Command` to the end, like `run`:
//   let out = try process.run_command(process.Command(program: "git", args: ["status"], dir: repo))
pub fn run_command(command: Command) throws -> Output

// Where a program is on PATH ("/usr/bin/git"), like `which`; none if it
// isn't installed.
pub fn find(program: String) -> String?

// The command-line arguments, without the program's name.
pub fn args() -> List<String>

// Whether Ctrl-C (or SIGTERM) was pressed. With tasks, Ctrl-C cancels
// main's task: every wait stops with `Cancelled`, `with` blocks close, and
// the program ends with status 130. A loop that never waits checks this
// instead: `while not process.interrupted() { ... }`. A second Ctrl-C ends
// the program at once.
pub fn interrupted() -> Bool

// Stops the program with an exit status.
pub fn exit(status: Int) -> Never
```

## env

env: environment variables.

```
pub fn get(name: String) -> String?

// Reads environment variables into a record: the field `database_url` comes
// from DATABASE_URL. Numbers and true/false are converted; a missing variable
// is an error unless the field is optional or has a default.
pub fn decode<T>() throws -> T

// Reads a `.env` file: `NAME=value` lines (`export NAME=value` too; `#`
// starts a comment; values may be in "double" quotes, with \n, or 'single'
// quotes, as they are). Variables already set are kept: the real
// environment wins. A missing file is fine. Call it at the start of `main`.
pub fn load(path: String) throws
```

## cli

cli: command-line arguments into a record.

```
type Options {
  input: String              // --input notes.txt (required)
  top: Int = 10            // --top 5 (optional: it has a default)
  verbose: Bool = false    // --verbose (a flag without a value)
  output_dir: String?        // --output-dir out (optional)
  args: List<String> = []    // everything that isn't an option
}
let opts = try cli.decode<Options>()
```

`--help` prints the options and exits. Unknown options and missing
required ones are errors that show the usage.

```
pub fn decode<T>() throws -> T
```

## term

term: the terminal: colors, questions, tables and progress for CLI tools.

```
print("${term.green("ok")} ${term.bold(name)}")
let name = try term.ask("Project name?")
if try term.confirm("Delete ${n} files?") { ... }
print(term.table([["name", "size"], ["a.txt", term.size(1536)]]))
```

Colors are added only when the output is a terminal and NO_COLOR isn't
set (FORCE_COLOR turns them on anyway), so piping into a file or another
program gives plain text.

```
// Whether standard output is a terminal (not a file or a pipe).
pub fn is_terminal() -> Bool

pub fn bold(text: String) -> String
pub fn dim(text: String) -> String
pub fn italic(text: String) -> String
pub fn underline(text: String) -> String
pub fn red(text: String) -> String
pub fn green(text: String) -> String
pub fn yellow(text: String) -> String
pub fn blue(text: String) -> String
pub fn magenta(text: String) -> String
pub fn cyan(text: String) -> String
pub fn gray(text: String) -> String

// The text without color codes.
pub fn strip(text: String) -> String

// How many columns the text takes on screen: wide characters (Chinese,
// emoji) take 2, color codes none.
pub fn width(text: String) -> Int

// The text padded with spaces to `columns` on screen.
pub fn pad(text: String, columns: Int) -> String

// Rows as aligned columns; the first row is the header (underlined when
// colors are on). Lines end without trailing spaces.
pub fn table(rows: List<List<String>>) -> String

// A size in bytes for people: 512 B, 1.5 KB, 23.4 MB, 1.2 GB (1 KB = 1024 B).
pub fn size(bytes: Int) -> String

// Asks a question on the terminal and returns the answer (without the line
// break); an error if the input has ended.
pub fn ask(question: String) throws -> String

// Asks until the answer is yes or no (y / n; an empty answer is no).
pub fn confirm(question: String) throws -> Bool

// Asks without showing what's typed: passwords.
pub fn secret(question: String) throws -> String

// Draws a progress line on standard error: [#########.....]  60% label.
// Call it as work advances; at `done == total` it ends the line. Nothing is
// drawn when standard error isn't a terminal.
pub fn progress(done: Int, total: Int, label: String)
```

## log

log: messages for people running the program, on standard error, with the
time: `2026-09-29T12:00:00Z INFO server started`.

```
pub fn debug(message: String)
pub fn info(message: String)
pub fn warn(message: String)
pub fn error(message: String)
```

## time

time: clocks, durations and waiting.

```
pub type Duration {
  nanos: Int
  pub fn seconds(self) -> Float
  pub fn millis(self) -> Int
  pub fn plus(self, other: Duration) -> Duration
  pub fn minus(self, other: Duration) -> Duration
  pub fn times(self, n: Int) -> Duration
  pub fn is_longer_than(self, other: Duration) -> Bool
  // For people: "1h30m", "2m5s", "1.5s", "250ms", "80µs", "12ns".
  pub fn text(self) -> String
}

pub fn minutes(n: Int) -> Duration

pub fn hours(n: Int) -> Duration

pub fn days(n: Int) -> Duration

// "1h30m", "90s", "1.5s", "250ms", "10us" (or µs), "10ns", "2d", "-5m".
pub fn parse_duration(text: String) throws -> Duration

pub fn seconds(n: Int) -> Duration

pub fn micros(n: Int) -> Duration

pub fn nanos(n: Int) -> Duration

pub fn millis(n: Int) -> Duration

// A point in time, for measuring how long something takes.
pub type Instant {
  nanos: Int
  pub fn elapsed(self) -> Duration
  pub fn plus(self, d: Duration) -> Instant
  // How long from `earlier` to this moment.
  pub fn since(self, earlier: Instant) -> Duration
}

pub fn now() -> Instant

// Wait without blocking other tasks. Fails with `Cancelled` if the task is cancelled.
pub fn sleep(duration: Duration) throws

// Waits until a moment (now or in the past: doesn't wait). For steady
// ticks, sleep until start + interval × n rather than a fixed interval,
// so the time the work takes doesn't add up:
//   for n in 1..=10 { try time.sleep_until(start.plus(interval.times(n))) ... }
pub fn sleep_until(moment: Instant) throws

// Runs `work`, giving up after `duration`: the work is cancelled (whatever
// it waits for stops with an error, its own tasks too) and `timeout`
// throws `TimedOut`.
//
//   let page = try time.timeout(time.seconds(5), () => try http.get(url))
pub fn timeout<R>(duration: Duration, work: fn() throws -> R) throws -> R

// The error of `timeout`.
pub type TimedOut implements Error {
  after: Duration
}

// A moment in UTC, from the wall clock.
pub type DateTime {
  year: Int
  month: Int
  day: Int
  hour: Int
  minute: Int
  second: Int
  // "2026-09-29T12:00:00Z"
  pub fn iso(self) -> String
  // The same moment on the wall clock of a time zone:
  //   let berlin = try moment.in_zone("Europe/Berlin")   // 14:05 UTC -> 16:05 +02:00
  pub fn in_zone(self, zone: String) throws -> Zoned
  // The calendar day.
  pub fn date(self) -> Date
  pub fn weekday(self) -> Weekday
  // As a pattern says (strftime): "%Y-%m-%d %H:%M" gives "2026-09-29 12:30".
  // %Y year, %m month, %d day, %H hour, %M minute, %S second, %y two-digit
  // year, %e day without zero, %I hour 1-12, %p AM/PM, %B September, %b Sep,
  // %A Tuesday, %a Tue, %j day of the year, %s Unix seconds, %z +0000,
  // %Z UTC, %% a percent sign.
  pub fn format(self, pattern: String) -> String
  pub fn is_before(self, other: DateTime) -> Bool
  pub fn is_after(self, other: DateTime) -> Bool
  // The same day and time `n` months later; the day is clamped to the
  // month's length (Jan 31 + 1 month = Feb 28).
  pub fn plus_months(self, n: Int) -> DateTime
  // Seconds since 1970-01-01 UTC.
  pub fn to_unix(self) -> Int
  // A moment `seconds` later (or earlier, if negative).
  pub fn plus_seconds(self, seconds: Int) -> DateTime
  pub fn plus_days(self, days: Int) -> DateTime
}

// Reads "2026-09-29", "2026-09-29T12:30:00Z" or "2026-09-29 12:30:00"
// (UTC; an offset like +02:00 is converted to UTC).
pub fn parse_iso(text: String) throws -> DateTime

// The current date and time in UTC.
pub fn utc_now() -> DateTime

// Seconds since 1970-01-01 UTC.
pub fn unix_now() -> Int

// The date and time of a Unix timestamp.
pub fn from_unix(seconds: Int) -> DateTime

pub enum Weekday {
  Monday
  Tuesday
  Wednesday
  Thursday
  Friday
  Saturday
  Sunday
}

// A calendar day, without a time or a zone: birthdays, due dates, reports.
pub type Date {
  year: Int
  month: Int
  day: Int
  // "2026-09-29"
  pub fn iso(self) -> String
  // Days since 1970-01-01 (negative before).
  pub fn to_days(self) -> Int
  pub fn plus_days(self, n: Int) -> Date
  // Clamped to the month's length: Jan 31 + 1 month = Feb 28.
  pub fn plus_months(self, n: Int) -> Date
  // Days from this date to `other` (negative if `other` is earlier).
  pub fn days_until(self, other: Date) -> Int
  pub fn weekday(self) -> Weekday
  pub fn is_before(self, other: Date) -> Bool
  pub fn is_after(self, other: Date) -> Bool
  // This day at a time of day, in UTC.
  pub fn at(self, hour: Int, minute: Int, second: Int) -> DateTime
  pub fn format(self, pattern: String) -> String
}

// Today in UTC.
pub fn today() -> Date

// "2026-09-29"
pub fn parse_date(text: String) throws -> Date

// Reads a date and time written as `pattern` says (see `DateTime.format`):
//   try time.parse("29/09/2026 14:05", pattern: "%d/%m/%Y %H:%M")
// Fields the pattern doesn't have are 0 (or 1 for month and day); with %z
// the result is converted to UTC.
pub fn parse(text: String, pattern: String) throws -> DateTime

// A moment as the wall clock of a time zone shows it.
pub type Zoned {
  year: Int
  month: Int
  day: Int
  hour: Int
  minute: Int
  second: Int
  // seconds east of UTC: 7200 for Berlin in summer, -18000 for New York in winter
  offset: Int
  // "Europe/Berlin"
  zone: String
  // "CEST"
  abbreviation: String
  // "2026-09-29T16:05:09+02:00"
  pub fn iso(self) -> String
  // As `DateTime.format`; %z gives the offset (+0200), %Z the abbreviation.
  pub fn format(self, pattern: String) -> String
  // The same moment in UTC.
  pub fn utc(self) -> DateTime
  pub fn date(self) -> Date
}

// The moment when the wall clock in `zone` shows this date and time:
//   let meeting = try time.in_zone("Europe/Berlin", date: d, hour: 9, minute: 30)
// A time skipped by a clock change moves forward; a repeated one takes
// the first.
pub fn in_zone(zone: String, date: Date, hour: Int, minute: Int) throws -> DateTime

// Now, on the wall clock of `zone`.
pub fn now_in(zone: String) throws -> Zoned

// This machine's zone ("Europe/Berlin"): TZ, else the system setting,
// else "UTC".
pub fn local_zone() -> String
```

## json

json: values to JSON text and back.

Records become objects with the field names as they are, lists become
arrays, a missing optional value becomes null, an enum variant without
fields becomes its name ("Draft"), one with fields an object with its
name under "type": {"type": "Circle", "radius": 2.0}.

```
// The value as compact JSON.
pub fn encode<T>(value: T) -> String
// The value as compact JSON with camelCase keys (created_at -> createdAt),
// for APIs that want them. Decoding needs nothing special: a `createdAt`
// key fills a `created_at` field (names match ignoring case and _ / -).
pub fn encode_camel<T>(value: T) -> String
// The value as indented JSON, for people.
pub fn encode_pretty<T>(value: T) -> String

// Reads JSON into a T, checking every field. A missing field is an error
// unless it's optional (`T?`) or has a default. Extra fields are ignored.
// Errors say where: `json: at $.users[2].age: expected a whole number`.
pub fn decode<T>(text: String) throws -> T

// A JSON Schema of T's JSON form: for describing data to other programs,
// and for asking language models for typed answers (see the llm package).
// Every field is listed as required (optional fields accept null), extra
// fields are not allowed: the "strict" form OpenAI-style APIs want.
pub fn schema<T>() -> String

// JSON of a shape that isn't known in advance.
pub enum Value {
  Null
  Bool(value: Bool)
  Number(value: Float)
  String(value: String)
  Array(items: List<Value>)
  Object(fields: Map<String, Value>)
}

// Reads any JSON into a `Value`.
pub fn parse(text: String) throws -> Value
```

## http

http: a server and a client.

```
fn get_note(req: http.Request) throws -> http.Response {
  let id = try (req.param("id") ?? "").to_int()
  return http.json(200, note)
}
```

```
var router = http.Router()
router.get("/notes/:id", get_note)
try http.serve(router, port: 8080)
```

Every request runs in its own task. An error thrown by a handler becomes a
500 response (and a log line); a bug in a handler also ends only that
request.

```
pub type Request {
  method: String
  // without the query: "/notes/7"
  path: String
  // header names in lower case
  headers: Map<String, String>
  body: Bytes
  // from the route pattern: "/notes/:id" gives "id"
  params: Map<String, String> = {}
  // from "?q=word&page=2"
  query_params: Map<String, String> = {}
  // A part of the path matched by `:name` in the route.
  pub fn param(self, name: String) -> String?
  // A value from the query: `?page=2` gives query("page") == "2".
  pub fn query(self, name: String) -> String?
  // A header, by its name in any case.
  pub fn header(self, name: String) -> String?
  // The body as text; an error if it isn't valid UTF-8.
  pub fn text(self) throws -> String
  // A cookie the client sent.
  pub fn cookie(self, name: String) -> String?
  // The fields of an HTML form (application/x-www-form-urlencoded).
  // For forms with files, see `parts`.
  pub fn form(self) throws -> Map<String, String>
  // The parts of a multipart/form-data body: form fields and uploaded
  // files. A field has `filename` none; `text()` reads its value.
  pub fn parts(self) throws -> List<Part>
}

// A part of a multipart form: a field or an uploaded file.
pub type Part {
  name: String
  // the file's name, for uploads
  filename: String? = none
  content_type: String = "text/plain"
  data: Bytes
  pub fn text(self) throws -> String
}

// A cookie to set with `Response.with_cookie`.
pub type Cookie {
  name: String
  value: String
  // seconds until it expires; none: when the browser closes; 0: delete it now
  max_age: Int? = none
  path: String = "/"
  // not readable from JavaScript
  http_only: Bool = true
  // only over HTTPS
  secure: Bool = false
  // "Lax", "Strict" or "None"
  same_site: String = "Lax"
}

// How long a client request took, each from its start. Connections are
// reused between requests (keep-alive), so `connect` is often zero.
pub type Timing {
  dns: time.Duration = time.nanos(0)
  connect: time.Duration = time.nanos(0)
  tls: time.Duration = time.nanos(0)
  // the answer's first byte arrived
  first_byte: time.Duration = time.nanos(0)
  total: time.Duration = time.nanos(0)
  // true if the request went over an already open connection
  reused: Bool = false
}

pub type Response {
  status: Int
  body: Bytes
  headers: Map<String, String> = {}
  // When set, the server sends this file as the body (see `file`).
  file: String = ""
  // When set, the server calls it to write the body (see `stream`).
  writer: (fn(io.Stream) throws)? = none
  // Client responses: how long the parts of the request took.
  timing: Timing = Timing()
  // The body as text; an error if it isn't valid UTF-8.
  pub fn text(self) throws -> String
  // A copy with one more header.
  pub fn with_header(self, name: String, value: String) -> Response
  // A copy that sets a cookie (several can be set).
  pub fn with_cookie(self, cookie: Cookie) -> Response
  // The cookies a client response sets, as name -> value.
  pub fn cookies(self) -> Map<String, String>
}

pub fn text(status: Int, body: String) -> Response

pub fn html(status: Int, body: String) -> Response

// The value as JSON (see the `json` module).
pub fn json<T>(status: Int, value: T) -> Response

// Any bytes: `http.bytes(200, png, content_type: "image/png")`.
pub fn bytes(status: Int, data: Bytes, content_type: String) -> Response

// A body written piece by piece while the client already receives it:
// live events, big exports. Each write reaches the client at once.
//
//   fn events(req: http.Request) throws -> http.Response {
//     return http.stream(200, content_type: "text/event-stream", writer: send_ticks)
//   }
//
//   fn send_ticks(out: io.Stream) throws {
//     for i in 0..<10 {
//       try out.write_text("data: tick ${i}\n\n")
//       try time.sleep(time.seconds(1))
//     }
//   }
//
// The writer runs after the status and headers are sent, so an error in it
// can't become a 500: it is logged and the connection is cut. When the
// client goes away, the next write fails, which ends the writer.
pub fn stream(status: Int, content_type: String, writer: fn(io.Stream) throws) -> Response

pub fn redirect(to: String) -> Response

// A file from the disk. The server sends it straight from the disk
// (sendfile), whatever its size; sets the content type from the extension;
// answers 304 when the client has this version cached and serves parts
// (`Range`) for resuming downloads and video. 404 if there is no such file;
// for a directory, its index.html.
pub fn file(path: String) -> Response

// Runs around every request: gets the request and `next` (the rest of the
// chain: other middleware, then the route) and returns a response.
//   router.use((req, next) => {
//     if req.header("authorization") is none { http.text(401, "log in") } else { try next(req) }
//   })
pub type Middleware = fn(Request, fn(Request) throws -> Response) throws -> Response

pub type Router {
  routes: List<Route> = []
  middleware: List<Middleware> = []
  // Adds middleware: the first added runs first (outermost).
  pub mutating fn use(m: Middleware)
  pub mutating fn get(pattern: String, handler: fn(Request) throws -> Response)
  pub mutating fn post(pattern: String, handler: fn(Request) throws -> Response)
  pub mutating fn put(pattern: String, handler: fn(Request) throws -> Response)
  pub mutating fn delete(pattern: String, handler: fn(Request) throws -> Response)
  // A WebSocket endpoint: the connection is upgraded and `handler` talks
  // over it until it returns (the connection then closes).
  //   router.websocket("/chat", ws => try chat(ws))
  pub mutating fn websocket(pattern: String, handler: fn(WebSocket) throws)
  // Serves the files in `dir` under `prefix`: files("/static", dir: "public")
  // answers /static/css/site.css with public/css/site.css. Paths can't leave
  // `dir`, and hidden files (".env", ".git") are not served.
  pub mutating fn files(prefix: String, dir: String)
  // Finds the route for a request and runs its handler: 404 when no route
  // has this path, 405 when one has it for another method. A pattern part
  // `*name` matches the rest of the path. HEAD is answered by GET routes.
  pub fn handle(self, request: Request) throws -> Response
}

// Middleware that logs each request: "GET /notes/7 200 1.2ms".
pub fn log_requests() -> Middleware

// Middleware for calls from web pages on other sites (CORS): answers the
// browser's OPTIONS question and marks responses as allowed for `origins`
// (["*"] for any site).
pub fn cors(origins: List<String>) -> Middleware

// The content type for a file name: "a.png" -> "image/png".
pub fn mime_type(path: String) -> String

// A request for tests: `router.handle(http.request("GET", "/notes/1"))`.
// With a body: `var req = http.request("POST", "/notes")`, then
// `req.body = "{\"title\": \"x\"}".bytes()`.
pub fn request(method: String, path: String) -> Request

// Serves the router until the program is stopped (Ctrl-C), then returns.
// It logs "listening on http://localhost:<port>" when ready, and "stopped".
pub fn serve(router: Router, port: Int) throws

// A request to send: http.send(http.ClientRequest(url: u, headers: {...}))
pub type ClientRequest {
  url: String
  method: String = "GET"
  headers: Map<String, String> = {}
  // Without a "content-type" header, one starting with { or [ is sent as
  // JSON, other text as plain text.
  body: Bytes = Bytes()
  // For the whole request, from connecting to the last byte of the body.
  // Streams read for long (http.open) need a long one.
  timeout: time.Duration = time.seconds(60)
  // 3xx answers are followed (at most 10; a POST becomes a GET after
  // 301/302/303, as browsers do). Otherwise the 3xx is the answer.
  follow_redirects: Bool = true
}

pub fn send(request: ClientRequest) throws -> Response

pub fn get(url: String) throws -> Response

pub fn post(url: String, body: String) throws -> Response

pub fn put(url: String, body: String) throws -> Response

pub fn delete(url: String) throws -> Response

// Sends a request and returns as soon as the status and headers arrive; the
// body is then read piece by piece: event streams, streamed answers of AI
// APIs, large responses.
//
//   with res = try http.open(http.ClientRequest(url: u)) {
//     while try res.read_line() is some(line) { ... }
//   }
pub fn open(request: ClientRequest) throws -> ResponseStream

pub builtin type ResponseStream {
  fn status(self) -> Int
  // A header, by its name in any case.
  fn header(self, name: String) -> String?
  // Reading the body works as for `io.Stream`. An error at the end means
  // the body was cut off.
  fn read(self, max: Int) throws -> Bytes
  fn read_line(self) throws -> String?
  fn read_all(self) throws -> Bytes
  // How long the request took up to the headers (`total` is filled in
  // once the body has been read).
  fn timing(self) -> Timing
  // Stops receiving the rest.
  fn close(self) throws
}

// Saves what `url` answers into the file `to`, without holding it in memory.
// The response has the status and headers and an empty body. The file is
// written only for a 2xx status.
pub fn download(url: String, to: String) throws -> Response

// A WebSocket connection: from `Router.websocket` on a server, or
// `http.websocket(address)` as a client. One task receives; sending from
// another task at the same time is fine for whole messages.
pub type WebSocket {
  conn: io.Stream
  // clients mask what they send (the protocol requires it)
  client: Bool = false
  // The next message; none when the other side has closed. Pings are
  // answered, and messages sent in pieces are joined, along the way.
  pub fn receive(self) throws -> WebSocketMessage?
  pub fn send_text(self, text: String) throws
  pub fn send(self, data: Bytes) throws
  // Says goodbye and closes the connection.
  pub fn close(self) throws
}

pub type WebSocketMessage {
  data: Bytes
  // text (UTF-8) or binary
  is_text: Bool
  pub fn text(self) throws -> String
}

// Connects to a WebSocket server: "ws://host:port/path" or "wss://..." (TLS).
//   with ws = try http.websocket("wss://echo.example/socket") {
//     try ws.send_text("hi")
//     print(try ws.receive()?.text())
//   }
pub fn websocket(address: String) throws -> WebSocket
```

## net

net: TCP and UDP. Waiting on the network doesn't block other tasks.

```
with conn = try net.connect("example.com", 80) {
  try conn.write_text("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
  print(try conn.read(4096).text())
}
```

```
try net.serve(7000, conn => try echo(conn))     // a task per connection
```

```
// A TCP connection is an `io.Stream`. Get one with
// `with conn = try net.connect(...)`, or in the handler of `net.serve`
// (which closes it when the handler returns).
pub fn connect(host: String, port: Int) throws -> io.Stream

// A TLS (encrypted) connection; the server's certificate is checked against
// the system's trusted certificates and the host name. Reading and writing
// work as for `connect`. (Serving TLS: put the program behind a proxy that
// terminates TLS, such as nginx or a cloud load balancer.)
pub fn connect_tls(host: String, port: Int) throws -> io.Stream

// Switches an open connection to TLS, for protocols that start plain and
// then upgrade (Postgres, SMTP's STARTTLS). `host` is the name the
// certificate must be for.
pub fn start_tls(conn: io.Stream, host: String) throws

// Accepts connections on `port` until the program is stopped (Ctrl-C),
// running `handler` for each in its own task. An error from a handler is
// logged; the connection is closed when the handler returns.
pub fn serve(port: Int, handler: fn(io.Stream) throws) throws

pub type Datagram {
  data: Bytes
  // "host:port" of the sender
  from: String
}

// A UDP socket. Get one with `with sock = try net.udp(port)`; port 0 picks
// a free one.
pub builtin type UdpSocket {
  // Sends to "host:port".
  fn send_to(self, data: Bytes, address: String) throws
  // Waits for the next datagram.
  fn receive(self) throws -> Datagram
  // The local port.
  fn port(self) -> Int
  fn close(self) throws
}

pub fn udp(port: Int) throws -> UdpSocket
```

## sql

sql: what SQL databases share. The built-in `db` (SQLite) and database
packages (Postgres, ...) use it, so they all work the same way:

- A `Query` can only be SQL written right in the call, as text in quotes.

```
Building SQL from pieces of text is a compile error, so SQL injection
can't happen: values go in as parameters.
```

- Parameters are written as plain values: `[name, 36, true]` becomes a

```
`List<Value>` by itself (Int, Float, String, Bool, Bytes, new types over
them, and `none`).
```

- Rows become records by column name, like `json.decode`.

```
// SQL text. Only a literal becomes a `Query`; a database package reads the
// text with `query.value`.
pub type Query = String

// A parameter or column value.
pub enum Value {
  Null
  Integer(value: Int)
  Real(value: Float)
  String(value: String)
  Blob(value: Bytes)
  Boolean(value: Bool)
}

// Rows as records: `columns` name the values of each row, matched to fields
// by name. Numbers and true/false convert as in `json.decode`; Null is
// "missing" (fine for optional fields and fields with defaults). For
// database packages.
pub fn decode<T>(columns: List<String>, rows: List<List<Value>>) throws -> List<T>
```

## db

db: SQLite databases (a file, or ":memory:").

```
with conn = try db.open("app.db") {
  try conn.execute("create table if not exists users (id integer primary key, name text, age integer)", [])
  try conn.execute("insert into users (name, age) values (?, ?)", ["Ada", 36])
  let adults = try conn.query<User>("select id, name, age from users where age >= ?", [18])
}
```

SQL is always a literal with `?` for values: building SQL from pieces of
text is a compile error, so SQL injection can't happen. Parameters can be
Int, Float, String, Bool and Bytes, and new types over them (see `sql`).
`query<T>` fills records by column name (like `json.decode`).

```
pub builtin type Connection {
  // A statement that returns no rows: create, insert, update, delete.
  fn execute(self, query: sql.Query, params: List<sql.Value>) throws
  // The same, returning the number of rows changed.
  fn execute_counting(self, query: sql.Query, params: List<sql.Value>) throws -> Int
  // The rows as records, columns matched to fields by name.
  fn query<T>(self, query: sql.Query, params: List<sql.Value>) throws -> List<T>
  // The id of the row inserted last.
  fn last_id(self) -> Int
  fn close(self) throws
  // Runs `work` in a transaction: everything or nothing. An error inside
  // rolls the changes back and comes out of `transaction`.
  fn transaction<R>(self, work: fn() throws -> R) throws -> R
}

// Opens (or creates) a database file; ":memory:" for one that lives only
// while the program runs.
pub fn open(path: String) throws -> Connection
```

## crypto

crypto: hashes, signatures, passwords and secure random bytes.

```
crypto.sha256("hello".bytes()).hex()
let stored = crypto.hash_password(password)       // keep this
crypto.verify_password(attempt, stored)           // later
```

```
pub fn sha256(data: Bytes) -> Bytes
// A signature of `data` with a secret `key` (webhooks, tokens).
// SHA-1, for protocols that require it (WebSocket handshakes, git). It
// isn't collision-safe: use sha256 for anything new.
pub fn sha1(data: Bytes) -> Bytes
pub fn hmac_sha256(key: Bytes, data: Bytes) -> Bytes
// A key derived from a password (PBKDF2 with HMAC-SHA256).
pub fn pbkdf2_sha256(password: Bytes, salt: Bytes, iterations: Int, length: Int) -> Bytes
// Secure random bytes.
pub fn random_bytes(count: Int) -> Bytes
// Compares secrets (tokens, signatures) in constant time.
pub fn equal(a: Bytes, b: Bytes) -> Bool

// A salted, slow hash of a password, safe to store:
// "pbkdf2-sha256$210000$<salt>$<hash>".
pub fn hash_password(password: String) -> String

// Checks a password against the result of `hash_password`.
pub fn verify_password(password: String, stored: String) -> Bool

pub builtin type PublicKey {
}

pub builtin type PrivateKey {
}

// From PEM text: "-----BEGIN PUBLIC KEY-----" (or RSA PUBLIC KEY).
pub fn public_key(pem: String) throws -> PublicKey
// From a JWK's RSA numbers (n and e, base64url-decoded).
pub fn rsa_public_key(n: Bytes, e: Bytes) throws -> PublicKey
// From a JWK's P-256 point (x and y, 32 bytes each).
pub fn ec_public_key(x: Bytes, y: Bytes) throws -> PublicKey
// From PEM text: "-----BEGIN PRIVATE KEY-----" (PKCS#8), RSA PRIVATE KEY or
// EC PRIVATE KEY.
pub fn private_key(pem: String) throws -> PrivateKey

// Whether `signature` is `key`'s signature of `data`.
pub fn verify(key: PublicKey, data: Bytes, signature: Bytes) -> Bool
pub fn sign(key: PrivateKey, data: Bytes) throws -> Bytes
```

## encoding

encoding: bytes as text and back. (Bytes to text: `data.hex()`,
`data.base64()`.)

```
// "68656c6c6f" -> the bytes of "hello"
pub fn from_hex(text: String) throws -> Bytes
// Standard or URL-safe base64, with or without padding.
pub fn from_base64(text: String) throws -> Bytes
// URL-safe base64 without padding (tokens, JWT).
pub fn base64_url(data: Bytes) -> String
```

## random

random: numbers from the operating system's secure generator.

```
// A whole number from `low` to `high`, both included.
pub fn between(low: Int, high: Int) -> Int
// A number from 0.0 (included) to 1.0 (not included).
pub fn fraction() -> Float
// A random item, none for an empty list.
pub fn pick<T>(items: List<T>) -> T?
// The items in a random order.
pub fn shuffle<T>(items: List<T>) -> List<T>
// Random letters and digits, for ids and secrets.
pub fn token(length: Int) -> String
// A random UUID (version 4): "3f0b6c5e-...".
pub fn uuid() -> String
// A UUID that starts with the time (version 7): ids made later sort later,
// which keeps database indexes compact. Use it for primary keys.
pub fn uuid_v7() -> String
```

## regex

regex: patterns for finding and replacing text.

```
let date = try regex.compile("(\\d{4})-(\\d{2})-(\\d{2})")
if date.find(line) is some(m) {
  print("year ${m.groups[0] ?? "?"}")
}
```

Extended regular expressions (POSIX), plus \d \w \s (and \D \W \S);
`(?i)` at the start ignores case. Positions are character indices.

```
pub type Match {
  // the matched text
  text: String
  start: Int
  end: Int
  // the parenthesized groups; none for a group that didn't take part
  groups: List<String?>
}

pub builtin type Regex {
  // Does the whole text match?
  fn matches(self, text: String) -> Bool
  // The first match anywhere in the text.
  fn find(self, text: String) -> Match?
  fn find_all(self, text: String) -> List<Match>
  // Replaces every match; "$1" in `replacement` is the first group, "$0" the match.
  fn replace(self, text: String, replacement: String) -> String
  // The pieces between matches.
  fn split(self, text: String) -> List<String>
}

pub fn compile(pattern: String) throws -> Regex
```

## csv

csv: comma-separated values (RFC 4180: quotes, commas and line breaks
inside quoted fields).

```
// Rows of fields.
pub fn parse(text: String) throws -> List<List<String>>

// Rows as records: the first row names the columns, which are matched to
// fields by name. Numbers and true/false are converted; an empty field is
// "missing" (fine for optional fields and fields with defaults).
pub fn decode<T>(text: String) throws -> List<T>

// Rows to CSV text, quoting fields where needed.
pub fn encode(rows: List<List<String>>) -> String
```

## xml

xml: reading and writing XML documents.

```
let feed = try xml.parse(text)
let channel = feed.child("channel") ?? xml.Element(name: "channel")
for item in channel.elements_named("item") {
  print(item.child_text("title") ?? "")
}
```

Whitespace between elements is dropped; other text is kept as it is.
Comments, `<?...?>` and `<!DOCTYPE>` are skipped. Names with a namespace
prefix are kept whole: "soap:Envelope".

```
pub type Element {
  name: String
  attributes: Map<String, String> = {}
  children: List<Node> = []
  pub fn attribute(self, name: String) -> String?
  // The first child element with this name.
  pub fn child(self, name: String) -> Element?
  // The text of the first child element with this name.
  pub fn child_text(self, name: String) -> String?
  // The child elements, without the text between them.
  pub fn elements(self) -> List<Element>
  pub fn elements_named(self, name: String) -> List<Element>
  // All the text inside, from every level: <p>a <b>b</b></p> gives "a b".
  pub fn text(self) -> String
  // A copy with one more child element (for building documents).
  pub fn add(self, element: Element) -> Element
  // A copy with text added at the end.
  pub fn add_text(self, text: String) -> Element
}

pub enum Node {
  ElementNode(element: Element)
  TextNode(text: String)
}

// Reads a document; errors say where: `xml: line 3: the closing tag doesn't
// match the open element`.
pub fn parse(text: String) throws -> Element

// The element as XML text, without the `<?xml ...?>` line.
pub fn render(element: Element) -> String

// Text safe inside an element or a quoted attribute: `<` becomes `&lt;` etc.
pub fn escape(text: String) -> String
```

## template

template: text from templates and data, in the Handlebars / Mustache
style most web developers know.

```
let page = try template.compile("<h1>{{title}}</h1>{{#each items}}<li>{{name}}: {{price}}</li>{{/each}}")
let html = try page.render(Page(title: "Menu", items: items))
```

{{name}}, {{user.email}}      a value (HTML-escaped in HTML templates)
{{{name}}}                     a value as it is (no escaping)
{{#if x}} ... {{else}} ... {{/if}}      false, none, 0, "" and [] are false
{{#unless x}} ... {{/unless}}
{{#each items}} ... {{else}} ... {{/each}}   {{this}}, {{@index}},

```
                             {{@first}}, {{@last}}, {{@key}} (maps)
```

{{#with user}} ... {{/with}}   names inside refer to user's fields
{{> header}}                   another template of the same `load`
{{! comment }}  {{~ trims whitespace before, ~}} after

Inside a block, a name not found there is looked up in the enclosing
data (../name does it explicitly). A name that isn't anywhere is an error,
with the line: a typo doesn't quietly render as nothing. The data is any
value, as `json.encode` sees it.

```
// A compiled template.
pub builtin type Template {
  fn render<T>(self, data: T) throws -> String
}

// A template for HTML: values are escaped (<, >, &, quotes).
pub fn compile(source: String) throws -> Template
// A template for plain text (emails, config files): values go in as they are.
pub fn compile_text(source: String) throws -> Template

// A folder of templates that can include each other with {{> name}}.
pub builtin type Library {
  // `name` is the file's path under the folder without its extension:
  // "pages/home" for pages/home.html.
  fn render<T>(self, name: String, data: T) throws -> String
  fn names(self) -> List<String>
}

// Loads every file under `dir`. Files ending in .html, .htm, .xml and .svg
// escape values; others (.txt, .md, ...) don't.
pub fn load(dir: String) throws -> Library
```

## url

url: parts of URLs, and percent-encoding.

```
pub type Url {
  scheme: String
  host: String
  port: Int?
  path: String
  query: Map<String, String>
  fragment: String?
  // "postgres://ada:secret@db:5432/app": "ada" and "secret" (decoded)
  user: String? = none
  password: String? = none
}

// "https://example.com:8080/a/b?x=1#top"
pub fn parse(text: String) throws -> Url

// Percent-encodes text for a URL part: "a b&c" -> "a%20b%26c"
pub fn encode(text: String) -> String

// "a%20b" -> "a b"
pub fn decode(text: String) throws -> String

// {"q": "a b", "page": "2"} -> "q=a%20b&page=2"
pub fn query_text(params: Map<String, String>) -> String
```

## zlib

zlib: compressing bytes.

```
// gzip format (.gz files, `content-encoding: gzip`).
pub fn gzip(data: Bytes) -> Bytes
pub fn gunzip(data: Bytes) throws -> Bytes
// zlib format.
pub fn deflate(data: Bytes) -> Bytes
// Reads zlib or gzip data.
pub fn inflate(data: Bytes) throws -> Bytes

// A .gz file as a stream (see `io`): reading gives the uncompressed data,
// so `read_line` reads a compressed log line by line.
pub fn open_gzip(path: String) throws -> io.Stream
// Creates a .gz file: what you write is compressed. Closing it (the end of
// `with`) writes the end of the file.
pub fn create_gzip(path: String) throws -> io.Stream
```

## archive

archive: tar and zip files, read and written in memory.

```
let entries = try archive.read_tar(try zlib.gunzip(try files.read_bytes("app.tar.gz")))
try archive.extract(entries, to: "out")
```

```
let zip = archive.write_zip(try archive.from_dir("site"))
try files.write_bytes("site.zip", zip)
```

`extract` refuses entries whose path would leave the target directory
(".." or absolute paths): archives from elsewhere can't overwrite files.

```
// A file or a directory in an archive.
pub type Entry {
  // "docs/readme.md"; directories end without a slash
  name: String
  data: Bytes = Bytes()
  is_dir: Bool = false
  // Unix permissions: 420 is rw-r--r-- (0644), 493 is rwxr-xr-x (0755)
  mode: Int = 420
  // seconds since 1970
  modified: Int = 0
}

pub fn read_tar(data: Bytes) throws -> List<Entry>

pub fn write_tar(entries: List<Entry>) -> Bytes

pub fn read_zip(data: Bytes) throws -> List<Entry>

pub fn write_zip(entries: List<Entry>) -> Bytes

// Every file and directory under `dir`, named relative to it.
pub fn from_dir(dir: String) throws -> List<Entry>

// Writes the entries under `to`. An entry whose path is absolute or goes
// up with ".." is an error: nothing outside `to` is written.
pub fn extract(entries: List<Entry>, to: String) throws
```

## math

math: the usual functions on Float.

```
pub fn pi() -> Float
pub fn e() -> Float
pub fn sin(x: Float) -> Float
pub fn cos(x: Float) -> Float
pub fn tan(x: Float) -> Float
pub fn asin(x: Float) -> Float
pub fn acos(x: Float) -> Float
pub fn atan(x: Float) -> Float
// the angle of the point (x, y)
pub fn atan2(y: Float, x: Float) -> Float
// natural logarithm
pub fn log(x: Float) -> Float
pub fn log10(x: Float) -> Float
pub fn log2(x: Float) -> Float
pub fn exp(x: Float) -> Float
// sqrt(x*x + y*y)
pub fn hypot(x: Float, y: Float) -> Float
pub fn is_nan(x: Float) -> Bool
pub fn infinity() -> Float
```

## prelude

Types and functions available in every file.

```
builtin type Int {
  fn to_float(self) -> Float
  fn to_decimal(self) -> Decimal
  fn to_string(self) -> String
  fn div(self, by: Int) -> Int
  fn abs(self) -> Int
  fn pow(self, exponent: Int) -> Int
  fn bit_and(self, other: Int) -> Int
  fn bit_or(self, other: Int) -> Int
  fn bit_xor(self, other: Int) -> Int
  fn shift_left(self, bits: Int) -> Int
  fn shift_right(self, bits: Int) -> Int
}

builtin type Float {
  // The closest decimal: 0.1 gives 0.1 (the shortest that reads back as
  // the same Float).
  fn to_decimal(self) -> Decimal
  fn round(self) -> Int
  fn floor(self) -> Int
  fn ceil(self) -> Int
  fn to_string(self) -> String
  // With exactly `decimals` digits after the point: 3.14159.format(2) == "3.14"
  fn format(self, decimals: Int) -> String
  fn abs(self) -> Float
  fn pow(self, exponent: Float) -> Float
  fn sqrt(self) -> Float
}

// An exact decimal number, for money: 0.1 + 0.2 == 0.3. Up to 18 digits.
// A number literal becomes one where a Decimal is expected:
// `let price: Decimal = 19.99`. `+ - *` are exact; division is `div`,
// which says how many digits to keep. `1.50` keeps its two digits when
// printed, and `1.50 == 1.5`.
builtin type Decimal {
  // Exactly `places` digits after the point, rounding half away from zero
  // (2.345 -> 2.35, like Excel and SQL): money.round(2)
  fn round(self, places: Int) -> Decimal
  // self / other with `places` digits after the point, rounded the same way
  fn div(self, other: Decimal, places: Int) -> Decimal
  fn abs(self) -> Decimal
  fn to_float(self) -> Float
  // "19.99"
  fn to_string(self) -> String
  // Rounded to `places` digits: 2.5.format(2) == "2.50"
  fn format(self, places: Int) -> String
}

fn __decimal(text: String) -> Decimal

builtin type Bool {
  fn to_string(self) -> String
}

builtin type String {
  length: Int
  fn is_empty(self) -> Bool
  fn byte_length(self) -> Int
  fn lower(self) -> String
  fn upper(self) -> String
  fn trim(self) -> String
  fn split(self, separator: String) -> List<String>
  fn lines(self) -> List<String>
  fn words(self) -> List<String>
  fn chars(self) -> List<String>
  fn contains(self, part: String) -> Bool
  // The position of the first `part` (in characters, as `slice` counts);
  // none if it isn't there. From a position: s.slice(from: i, to: s.length).index_of(x)
  fn index_of(self, part: String) -> Int?
  fn __find(self, part: String, from: Int) -> Int
  fn trim_start(self) -> String
  fn trim_end(self) -> String
  // Every character is 0-9 (and there is at least one).
  fn is_digit(self) -> Bool
  // Every character is a letter, of any script (and there is at least one).
  fn is_letter(self) -> Bool
  // Every character is a space, tab or line break.
  fn is_space(self) -> Bool
  fn is_upper(self) -> Bool
  fn is_lower(self) -> Bool
  fn starts_with(self, prefix: String) -> Bool
  fn ends_with(self, suffix: String) -> Bool
  fn replace(self, old: String, new: String) -> String
  fn slice(self, from: Int, to: Int) -> String
  fn repeat(self, times: Int) -> String
  fn pad_start(self, width: Int, fill: String) -> String
  fn pad_end(self, width: Int, fill: String) -> String
  fn to_int(self) throws -> Int
  fn to_float(self) throws -> Float
  // "19.99" -> 19.99 exactly
  fn to_decimal(self) throws -> Decimal
  fn to_string(self) -> String
  // The text as UTF-8 bytes.
  fn bytes(self) -> Bytes
}

// Raw bytes: file contents, network data, hashes. `Bytes()` is empty;
// `Bytes([104, 105])` from numbers 0-255; `text.bytes()` from text (UTF-8).
builtin type Bytes {
  length: Int
  fn is_empty(self) -> Bool
  // The text these bytes encode (UTF-8); an error if they aren't valid UTF-8.
  fn text(self) throws -> String
  fn slice(self, from: Int, to: Int) -> Bytes
  fn concat(self, other: Bytes) -> Bytes
  fn index_of(self, part: Bytes) -> Int?
  fn hex(self) -> String
  fn base64(self) -> String
  fn to_list(self) -> List<Int>
  // An unsigned big-endian number of `size` bytes (1-8) at `offset`
  // (network protocols).
  fn int_at(self, offset: Int, size: Int) -> Int
  mutating fn append(byte: Int)
  mutating fn append_all(other: Bytes)
  mutating fn append_text(text: String)
  // Adds `value` as `size` big-endian bytes.
  mutating fn append_int(value: Int, size: Int)
}

builtin type List<T> {
  length: Int
  fn is_empty(self) -> Bool
  fn first(self) -> T?
  fn last(self) -> T?
  fn contains(self, item: T) -> Bool
  fn find(self, test: fn(T) throws -> Bool) rethrows -> T?
  fn any(self, test: fn(T) throws -> Bool) rethrows -> Bool
  fn all(self, test: fn(T) throws -> Bool) rethrows -> Bool
  fn count(self, test: fn(T) throws -> Bool) rethrows -> Int
  fn map<R>(self, transform: fn(T) throws -> R) rethrows -> List<R>
  fn filter(self, test: fn(T) throws -> Bool) rethrows -> List<T>
  fn fold<A>(self, start: A, step: fn(A, T) throws -> A) rethrows -> A
  fn sorted(self) -> List<T>
  // Sorted by a key, smallest first; equal keys keep their order (stable).
  // Largest first: `xs.sorted_by(x => x.score).reversed()`.
  fn sorted_by<K>(self, key: fn(T) throws -> K) rethrows -> List<T>
  // Each item's list, one after another: `orders.flat_map(o => o.items)`.
  fn flat_map<R>(self, transform: fn(T) throws -> List<R>) rethrows -> List<R>
  // Without repeats, the first of each kept, in order.
  fn unique(self) -> List<T>
  // In pieces of `size` (the last may be shorter): batches.
  fn chunks(self, size: Int) -> List<List<T>>
  // The position of the first item equal to `item`.
  fn index_of(self, item: T) -> Int?
  // The position of the first item that passes `test`.
  fn find_index(self, test: fn(T) throws -> Bool) rethrows -> Int?
  // The items that pass `test` and the rest, in two lists.
  fn partition(self, test: fn(T) throws -> Bool) rethrows -> Partition<T>
  fn reversed(self) -> List<T>
  fn take(self, n: Int) -> List<T>
  fn drop(self, n: Int) -> List<T>
  fn concat(self, other: List<T>) -> List<T>
  fn indexed(self) -> List<Indexed<T>>
  fn zip<U>(self, other: List<U>) -> List<Pair<T, U>>
  fn sum(self) -> T
  fn min(self) -> T?
  fn max(self) -> T?
  fn min_by<K>(self, key: fn(T) throws -> K) rethrows -> T?
  fn max_by<K>(self, key: fn(T) throws -> K) rethrows -> T?
  fn join(self, separator: String) -> String
  fn parallel_map<R>(self, limit: Int, transform: fn(T) throws -> R) rethrows -> List<R>
  fn group_by<K>(self, key: fn(T) throws -> K) rethrows -> Map<K, List<T>>
  fn count_each(self) -> Map<T, Int>
  mutating fn append(item: T)
  mutating fn append_all(items: List<T>)
  mutating fn insert(item: T, at: Int)
  mutating fn remove_at(index: Int)
  mutating fn pop() -> T?
  mutating fn sort()
  mutating fn sort_by<K>(key: fn(T) throws -> K) rethrows
  mutating fn reverse()
  mutating fn clear()
}

builtin type Map<K, V> {
  length: Int
  fn is_empty(self) -> Bool
  fn contains_key(self, key: K) -> Bool
  fn keys(self) -> List<K>
  fn values(self) -> List<V>
  fn entries(self) -> List<Entry<K, V>>
  mutating fn remove(key: K)
  mutating fn take(key: K) -> V?
  mutating fn clear()
}

builtin type Set<T> {
  length: Int
  fn is_empty(self) -> Bool
  fn contains(self, item: T) -> Bool
  fn to_list(self) -> List<T>
  mutating fn add(item: T)
  mutating fn remove(item: T)
  mutating fn clear()
}

// A task started with `spawn`.
builtin type Task<T> {
  fn wait(self) throws -> T
  fn cancel(self)
}

// State shared between tasks (and between HTTP requests):
//
//   let counter = Shared<Int>(0)
//   with n = counter.lock() {
//     n += 1                       // other tasks wait at `with`
//   }
//
// Inside the block the value is used and changed like a variable, fields
// and all (`s.todos.append(x)`); the lock is released at the end of the
// block, even on an error. A lock directly inside another is an error.
builtin type Shared<T> {
  fn lock(self) -> Locked<T>
}

// What `lock()` gives: only for `with v = s.lock() { }`, where `v` is the
// value itself.
builtin type Locked<T> {
}

// A queue between tasks.
builtin type Channel<T> {
  fn send(self, item: T) throws
  fn receive(self) throws -> T?
  // A value if one is waiting, without waiting; none otherwise.
  fn try_receive(self) -> T?
  fn close(self)
}

// Raised in a task that was cancelled, at its next wait.
type Cancelled implements Error {
  fn message(self) -> String
}

// Raised by `send` on a closed channel.
type ChannelClosed implements Error {
  fn message(self) -> String
}

type Entry<K, V> {
  key: K
  value: V
}

type Indexed<T> {
  index: Int
  value: T
}

// What `List.partition` gives.
type Partition<T> {
  matching: List<T>
  rest: List<T>
}

type Pair<A, B> {
  first: A
  second: B
}

type Failure implements Error {
  message: String
  cause: Error? = none
  fn message(self) -> String
}

// A line to standard output.
fn print(text: String)
// A line to standard error: errors and messages for people, kept apart from
// the program's output (see also the `log` module).
fn eprint(text: String)
fn min<T>(a: T, b: T) -> T
fn max<T>(a: T, b: T) -> T
fn assert(condition: Bool)
fn panic(message: String) -> Never

fn __list_with_capacity<T>(capacity: Int) -> List<T>
fn __less<T>(a: T, b: T) -> Bool
```
