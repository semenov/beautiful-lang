# Standard library

Generated from the sources by `tools/stdlib_doc.py`: the public
declarations with their comments, bodies left out. `import <module>`,
then use `module.name`. Functions without a body are built into the
compiler and runtime.

Modules: [`files`](#files), [`path`](#path), [`io`](#io), [`process`](#process), [`env`](#env), [`cli`](#cli), [`log`](#log), [`time`](#time), [`json`](#json), [`http`](#http), [`net`](#net), [`sql`](#sql), [`db`](#db), [`crypto`](#crypto), [`encoding`](#encoding), [`random`](#random), [`regex`](#regex), [`csv`](#csv), [`xml`](#xml), [`url`](#url), [`zlib`](#zlib), [`math`](#math), and the [prelude](#prelude)
(available everywhere without `import`).

## files

files: read and write files and directories. Paths are text; errors say
which path failed and why.

```
// The whole file as text.
pub fn read(path: Text) throws -> Text
// The whole file as bytes.
pub fn read_bytes(path: Text) throws -> Bytes
// Creates or replaces the file.
pub fn write(path: Text, text: Text) throws
pub fn write_bytes(path: Text, data: Bytes) throws
// Adds to the end of the file, creating it if needed.
pub fn append(path: Text, text: Text) throws
pub fn exists(path: Text) -> Bool
pub fn is_dir(path: Text) -> Bool
// Names of the entries in a directory, sorted.
pub fn list(dir: Text) throws -> List<Text>
// Deletes a file or an empty directory.
pub fn delete(path: Text) throws
// Creates a directory and any missing parents.
pub fn make_dir(path: Text) throws
pub fn copy(from: Text, to: Text) throws
pub fn rename(from: Text, to: Text) throws

// A file as a stream (see the `io` module), for reading or writing it
// piece by piece: `with f = try files.open(path) { ... }`.
pub fn open(path: Text) throws -> io.Stream
// Creates (or empties) a file for writing.
pub fn create(path: Text) throws -> io.Stream
// Opens a file for writing at its end, creating it if needed.
pub fn open_append(path: Text) throws -> io.Stream

// A new empty directory, deleted with everything in it at the end of `with`.
pub builtin type TempDir {
  path: Text
  fn close(self) throws
}

pub fn temp_dir() throws -> TempDir
```

## path

path: the text of file paths ("a/b/c.txt"). Nothing here touches the
disk; reading and writing is in `files`.

```
// "a/b" + "c.txt" -> "a/b/c.txt"; an absolute `name` replaces `dir`
pub fn join(dir: Text, name: Text) -> Text
// "a/b/c.txt" -> "c.txt"
pub fn name(path: Text) -> Text
// "a/b/c.txt" -> "a/b"
pub fn parent(path: Text) -> Text
// "a/b/c.txt" -> "txt"; none without a dot
pub fn extension(path: Text) -> Text?

// "a/b/c.txt" -> "c"
pub fn stem(path: Text) -> Text

pub fn is_absolute(path: Text) -> Bool

// "/a/b/c.txt" -> ["a", "b", "c.txt"]
pub fn parts(path: Text) -> List<Text>

// Removes "." and resolves "..": "a/./b/../c" -> "a/c"
pub fn clean(path: Text) -> Text
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
  fn read_line(self) throws -> Text?
  // Everything up to the end.
  fn read_all(self) throws -> Bytes
  fn write(self, data: Bytes) throws
  fn write_text(self, text: Text) throws
  // For network connections, the other side's address: "93.184.215.14:80";
  // empty for other streams.
  fn peer(self) -> Text
  // Writes what is still buffered and closes the stream.
  fn close(self) throws
}

// This program's standard input, output and error output.
pub fn stdin() -> Stream
pub fn stdout() -> Stream
pub fn stderr() -> Stream

// The next line of standard input; none at the end.
pub fn read_line() throws -> Text?
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
  stdout: Text
  stderr: Text
}

// Runs a program and waits for it to finish. The arguments are passed as
// they are: there is no shell, so nothing needs quoting. A program that
// exits with a non-zero status is not an error: check `output.status`.
pub fn run(program: Text, args: List<Text>) throws -> Output

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
  fn read_line(self) throws -> Text?
  fn read_all(self) throws -> Bytes
  fn write(self, data: Bytes) throws
  fn write_text(self, text: Text) throws
  // Tells the program there is no more input.
  fn close_input(self) throws
  // Waits for the program to finish and returns its exit status. Output it
  // writes meanwhile is kept for reading.
  fn wait(self) throws -> Int
  fn close(self) throws
}

// Starts a program without waiting for it (see `run` for the arguments).
pub fn start(program: Text, args: List<Text>) throws -> Process

// The command-line arguments, without the program's name.
pub fn args() -> List<Text>

// Stops the program with an exit status.
pub fn exit(status: Int) -> Never
```

## env

env: environment variables.

```
pub fn get(name: Text) -> Text?

// Reads environment variables into a record: the field `database_url` comes
// from DATABASE_URL. Numbers and true/false are converted; a missing variable
// is an error unless the field is optional or has a default.
pub fn decode<T>() throws -> T
```

## cli

cli: command-line arguments into a record.

```
type Options {
  input: Text              // --input notes.txt (required)
  top: Int = 10            // --top 5 (optional: it has a default)
  verbose: Bool = false    // --verbose (a flag without a value)
  output_dir: Text?        // --output-dir out (optional)
  args: List<Text> = []    // everything that isn't an option
}
let opts = try cli.decode<Options>()
```

`--help` prints the options and exits. Unknown options and missing
required ones are errors that show the usage.

```
pub fn decode<T>() throws -> T
```

## log

log: messages for people running the program, on standard error, with the
time: `2026-09-29T12:00:00Z INFO server started`.

```
pub fn info(message: Text)
pub fn warn(message: Text)
pub fn error(message: Text)
```

## time

time: clocks, durations and waiting.

```
pub type Duration {
  nanos: Int
  pub fn seconds(self) -> Float
  pub fn millis(self) -> Int
}

pub fn seconds(n: Int) -> Duration

pub fn millis(n: Int) -> Duration

// A point in time, for measuring how long something takes.
pub type Instant {
  nanos: Int
  pub fn elapsed(self) -> Duration
}

pub fn now() -> Instant

// Wait without blocking other tasks. Fails with `Cancelled` if the task is cancelled.
pub fn sleep(duration: Duration) throws

// A moment in UTC, from the wall clock.
pub type DateTime {
  year: Int
  month: Int
  day: Int
  hour: Int
  minute: Int
  second: Int
  // "2026-09-29T12:00:00Z"
  pub fn iso(self) -> Text
  // "2026-09-29"
  pub fn date(self) -> Text
  // Seconds since 1970-01-01 UTC.
  pub fn to_unix(self) -> Int
  // A moment `seconds` later (or earlier, if negative).
  pub fn plus_seconds(self, seconds: Int) -> DateTime
  pub fn plus_days(self, days: Int) -> DateTime
}

// Reads "2026-09-29", "2026-09-29T12:30:00Z" or "2026-09-29 12:30:00"
// (UTC; an offset like +02:00 is converted to UTC).
pub fn parse_iso(text: Text) throws -> DateTime

// The current date and time in UTC.
pub fn utc_now() -> DateTime

// Seconds since 1970-01-01 UTC.
pub fn unix_now() -> Int

// The date and time of a Unix timestamp.
pub fn from_unix(seconds: Int) -> DateTime
```

## json

json: values to JSON text and back.

Records become objects with the field names as they are, lists become
arrays, a missing optional value becomes null, an enum variant without
fields becomes its name ("Draft"), one with fields an object with its
name under "type": {"type": "Circle", "radius": 2.0}.

```
// The value as compact JSON.
pub fn encode<T>(value: T) -> Text
// The value as indented JSON, for people.
pub fn encode_pretty<T>(value: T) -> Text

// Reads JSON into a T, checking every field. A missing field is an error
// unless it's optional (`T?`) or has a default. Extra fields are ignored.
// Errors say where: `json: at $.users[2].age: expected a whole number`.
pub fn decode<T>(text: Text) throws -> T

// A JSON Schema of T's JSON form: for describing data to other programs,
// and for asking language models for typed answers (see the llm package).
// Every field is listed as required (optional fields accept null), extra
// fields are not allowed: the "strict" form OpenAI-style APIs want.
pub fn schema<T>() -> Text

// JSON of a shape that isn't known in advance.
pub enum Value {
  Null
  Bool(value: Bool)
  Number(value: Float)
  String(value: Text)
  Array(items: List<Value>)
  Object(fields: Map<Text, Value>)
}

// Reads any JSON into a `Value`.
pub fn parse(text: Text) throws -> Value
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
  method: Text
  // without the query: "/notes/7"
  path: Text
  // header names in lower case
  headers: Map<Text, Text>
  body: Bytes
  // from the route pattern: "/notes/:id" gives "id"
  params: Map<Text, Text> = {}
  // from "?q=word&page=2"
  query_params: Map<Text, Text> = {}
  // A part of the path matched by `:name` in the route.
  pub fn param(self, name: Text) -> Text?
  // A value from the query: `?page=2` gives query("page") == "2".
  pub fn query(self, name: Text) -> Text?
  // A header, by its name in any case.
  pub fn header(self, name: Text) -> Text?
  // The body as text; an error if it isn't valid UTF-8.
  pub fn text(self) throws -> Text
}

pub type Response {
  status: Int
  body: Bytes
  headers: Map<Text, Text> = {}
  // When set, the server sends this file as the body (see `file`).
  file: Text = ""
  // When set, the server calls it to write the body (see `stream`).
  writer: (fn(io.Stream) throws)? = none
  // The body as text; an error if it isn't valid UTF-8.
  pub fn text(self) throws -> Text
  // A copy with one more header.
  pub fn with_header(self, name: Text, value: Text) -> Response
}

pub fn text(status: Int, body: Text) -> Response

pub fn html(status: Int, body: Text) -> Response

// The value as JSON (see the `json` module).
pub fn json<T>(status: Int, value: T) -> Response

// Any bytes: `http.bytes(200, png, content_type: "image/png")`.
pub fn bytes(status: Int, data: Bytes, content_type: Text) -> Response

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
pub fn stream(status: Int, content_type: Text, writer: fn(io.Stream) throws) -> Response

pub fn redirect(to: Text) -> Response

// A file from the disk. The server sends it straight from the disk
// (sendfile), whatever its size; sets the content type from the extension;
// answers 304 when the client has this version cached and serves parts
// (`Range`) for resuming downloads and video. 404 if there is no such file;
// for a directory, its index.html.
pub fn file(path: Text) -> Response

pub type Router {
  routes: List<Route> = []
  pub mutating fn get(pattern: Text, handler: fn(Request) throws -> Response)
  pub mutating fn post(pattern: Text, handler: fn(Request) throws -> Response)
  pub mutating fn put(pattern: Text, handler: fn(Request) throws -> Response)
  pub mutating fn delete(pattern: Text, handler: fn(Request) throws -> Response)
  // Serves the files in `dir` under `prefix`: files("/static", dir: "public")
  // answers /static/css/site.css with public/css/site.css. Paths can't leave
  // `dir`, and hidden files (".env", ".git") are not served.
  pub mutating fn files(prefix: Text, dir: Text)
  // Finds the route for a request and runs its handler: 404 when no route
  // has this path, 405 when one has it for another method. A pattern part
  // `*name` matches the rest of the path. HEAD is answered by GET routes.
  pub fn handle(self, request: Request) throws -> Response
}

// A request for tests: `router.handle(http.request("GET", "/notes/1"))`.
pub fn request(method: Text, path: Text) -> Request

// Serves the router until the program is stopped (Ctrl-C), then returns.
pub fn serve(router: Router, port: Int) throws

// A request to send: http.send(http.ClientRequest(url: u, headers: {...}))
pub type ClientRequest {
  url: Text
  method: Text = "GET"
  headers: Map<Text, Text> = {}
  // Without a "content-type" header, one starting with { or [ is sent as
  // JSON, other text as plain text.
  body: Bytes = Bytes()
  // Seconds without any progress before giving up.
  timeout: Int = 60
}

pub fn send(request: ClientRequest) throws -> Response

pub fn get(url: Text) throws -> Response

pub fn post(url: Text, body: Text) throws -> Response

pub fn put(url: Text, body: Text) throws -> Response

pub fn delete(url: Text) throws -> Response

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
  fn header(self, name: Text) -> Text?
  // Reading the body works as for `io.Stream`. An error at the end means
  // the body was cut off.
  fn read(self, max: Int) throws -> Bytes
  fn read_line(self) throws -> Text?
  fn read_all(self) throws -> Bytes
  // Stops receiving the rest.
  fn close(self) throws
}

// Saves what `url` answers into the file `to`, without holding it in memory.
// The response has the status and headers and an empty body. The file is
// written only for a 2xx status.
pub fn download(url: Text, to: Text) throws -> Response
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
pub fn connect(host: Text, port: Int) throws -> io.Stream

// A TLS (encrypted) connection; the server's certificate is checked against
// the system's trusted certificates and the host name. Reading and writing
// work as for `connect`. (Serving TLS: put the program behind a proxy that
// terminates TLS, such as nginx or a cloud load balancer.)
pub fn connect_tls(host: Text, port: Int) throws -> io.Stream

// Switches an open connection to TLS, for protocols that start plain and
// then upgrade (Postgres, SMTP's STARTTLS). `host` is the name the
// certificate must be for.
pub fn start_tls(conn: io.Stream, host: Text) throws

// Accepts connections on `port` until the program is stopped (Ctrl-C),
// running `handler` for each in its own task. An error from a handler is
// logged; the connection is closed when the handler returns.
pub fn serve(port: Int, handler: fn(io.Stream) throws) throws

pub type Datagram {
  data: Bytes
  // "host:port" of the sender
  from: Text
}

// A UDP socket. Get one with `with sock = try net.udp(port)`; port 0 picks
// a free one.
pub builtin type UdpSocket {
  // Sends to "host:port".
  fn send_to(self, data: Bytes, address: Text) throws
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
`List<Value>` by itself (Int, Float, Text, Bool, Bytes, new types over
them, and `none`).
```

- Rows become records by column name, like `json.decode`.

```
// SQL text. Only a literal becomes a `Query`; a database package reads the
// text with `query.value`.
pub type Query = Text

// A parameter or column value.
pub enum Value {
  Null
  Integer(value: Int)
  Real(value: Float)
  String(value: Text)
  Blob(value: Bytes)
  Boolean(value: Bool)
}

// Rows as records: `columns` name the values of each row, matched to fields
// by name. Numbers and true/false convert as in `json.decode`; Null is
// "missing" (fine for optional fields and fields with defaults). For
// database packages.
pub fn decode<T>(columns: List<Text>, rows: List<List<Value>>) throws -> List<T>
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
Int, Float, Text, Bool and Bytes, and new types over them (see `sql`).
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
pub fn open(path: Text) throws -> Connection
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
pub fn hmac_sha256(key: Bytes, data: Bytes) -> Bytes
// A key derived from a password (PBKDF2 with HMAC-SHA256).
pub fn pbkdf2_sha256(password: Bytes, salt: Bytes, iterations: Int, length: Int) -> Bytes
// Secure random bytes.
pub fn random_bytes(count: Int) -> Bytes
// Compares secrets (tokens, signatures) in constant time.
pub fn equal(a: Bytes, b: Bytes) -> Bool

// A salted, slow hash of a password, safe to store:
// "pbkdf2-sha256$210000$<salt>$<hash>".
pub fn hash_password(password: Text) -> Text

// Checks a password against the result of `hash_password`.
pub fn verify_password(password: Text, stored: Text) -> Bool
```

## encoding

encoding: bytes as text and back. (Bytes to text: `data.hex()`,
`data.base64()`.)

```
// "68656c6c6f" -> the bytes of "hello"
pub fn from_hex(text: Text) throws -> Bytes
// Standard or URL-safe base64, with or without padding.
pub fn from_base64(text: Text) throws -> Bytes
// URL-safe base64 without padding (tokens, JWT).
pub fn base64_url(data: Bytes) -> Text
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
pub fn token(length: Int) -> Text
// A random UUID (version 4): "3f0b6c5e-...".
pub fn uuid() -> Text
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
  text: Text
  start: Int
  end: Int
  // the parenthesized groups; none for a group that didn't take part
  groups: List<Text?>
}

pub builtin type Regex {
  // Does the whole text match?
  fn matches(self, text: Text) -> Bool
  // The first match anywhere in the text.
  fn find(self, text: Text) -> Match?
  fn find_all(self, text: Text) -> List<Match>
  // Replaces every match; "$1" in `replacement` is the first group, "$0" the match.
  fn replace(self, text: Text, replacement: Text) -> Text
  // The pieces between matches.
  fn split(self, text: Text) -> List<Text>
}

pub fn compile(pattern: Text) throws -> Regex
```

## csv

csv: comma-separated values (RFC 4180: quotes, commas and line breaks
inside quoted fields).

```
// Rows of fields.
pub fn parse(text: Text) throws -> List<List<Text>>

// Rows as records: the first row names the columns, which are matched to
// fields by name. Numbers and true/false are converted; an empty field is
// "missing" (fine for optional fields and fields with defaults).
pub fn decode<T>(text: Text) throws -> List<T>

// Rows to CSV text, quoting fields where needed.
pub fn encode(rows: List<List<Text>>) -> Text
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
  name: Text
  attributes: Map<Text, Text> = {}
  children: List<Node> = []
  pub fn attribute(self, name: Text) -> Text?
  // The first child element with this name.
  pub fn child(self, name: Text) -> Element?
  // The text of the first child element with this name.
  pub fn child_text(self, name: Text) -> Text?
  // The child elements, without the text between them.
  pub fn elements(self) -> List<Element>
  pub fn elements_named(self, name: Text) -> List<Element>
  // All the text inside, from every level: <p>a <b>b</b></p> gives "a b".
  pub fn text(self) -> Text
  // A copy with one more child element (for building documents).
  pub fn add(self, element: Element) -> Element
  // A copy with text added at the end.
  pub fn add_text(self, text: Text) -> Element
}

pub enum Node {
  ElementNode(element: Element)
  TextNode(text: Text)
}

// Reads a document; errors say where: `xml: line 3: the closing tag doesn't
// match the open element`.
pub fn parse(text: Text) throws -> Element

// The element as XML text, without the `<?xml ...?>` line.
pub fn render(element: Element) -> Text

// Text safe inside an element or a quoted attribute: `<` becomes `&lt;` etc.
pub fn escape(text: Text) -> Text
```

## url

url: parts of URLs, and percent-encoding.

```
pub type Url {
  scheme: Text
  host: Text
  port: Int?
  path: Text
  query: Map<Text, Text>
  fragment: Text?
  // "postgres://ada:secret@db:5432/app": "ada" and "secret" (decoded)
  user: Text? = none
  password: Text? = none
}

// "https://example.com:8080/a/b?x=1#top"
pub fn parse(text: Text) throws -> Url

// Percent-encodes text for a URL part: "a b&c" -> "a%20b%26c"
pub fn encode(text: Text) -> Text

// "a%20b" -> "a b"
pub fn decode(text: Text) throws -> Text

// {"q": "a b", "page": "2"} -> "q=a%20b&page=2"
pub fn query_text(params: Map<Text, Text>) -> Text
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
pub fn open_gzip(path: Text) throws -> io.Stream
// Creates a .gz file: what you write is compressed. Closing it (the end of
// `with`) writes the end of the file.
pub fn create_gzip(path: Text) throws -> io.Stream
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
  fn to_text(self) -> Text
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
  fn round(self) -> Int
  fn floor(self) -> Int
  fn ceil(self) -> Int
  fn to_text(self) -> Text
  // With exactly `decimals` digits after the point: 3.14159.format(2) == "3.14"
  fn format(self, decimals: Int) -> Text
  fn abs(self) -> Float
  fn pow(self, exponent: Float) -> Float
  fn sqrt(self) -> Float
}

builtin type Bool {
  fn to_text(self) -> Text
}

builtin type Text {
  length: Int
  fn is_empty(self) -> Bool
  fn byte_length(self) -> Int
  fn lower(self) -> Text
  fn upper(self) -> Text
  fn trim(self) -> Text
  fn split(self, separator: Text) -> List<Text>
  fn lines(self) -> List<Text>
  fn words(self) -> List<Text>
  fn chars(self) -> List<Text>
  fn contains(self, part: Text) -> Bool
  fn starts_with(self, prefix: Text) -> Bool
  fn ends_with(self, suffix: Text) -> Bool
  fn replace(self, old: Text, new: Text) -> Text
  fn slice(self, from: Int, to: Int) -> Text
  fn repeat(self, times: Int) -> Text
  fn pad_start(self, width: Int, fill: Text) -> Text
  fn pad_end(self, width: Int, fill: Text) -> Text
  fn to_int(self) throws -> Int
  fn to_float(self) throws -> Float
  fn to_text(self) -> Text
  // The text as UTF-8 bytes.
  fn bytes(self) -> Bytes
}

// Raw bytes: file contents, network data, hashes. `Bytes()` is empty;
// `Bytes([104, 105])` from numbers 0-255; `text.bytes()` from text (UTF-8).
builtin type Bytes {
  length: Int
  fn is_empty(self) -> Bool
  // The text these bytes encode (UTF-8); an error if they aren't valid UTF-8.
  fn text(self) throws -> Text
  fn slice(self, from: Int, to: Int) -> Bytes
  fn concat(self, other: Bytes) -> Bytes
  fn index_of(self, part: Bytes) -> Int?
  fn hex(self) -> Text
  fn base64(self) -> Text
  fn to_list(self) -> List<Int>
  // An unsigned big-endian number of `size` bytes (1-8) at `offset`
  // (network protocols).
  fn int_at(self, offset: Int, size: Int) -> Int
  mutating fn append(byte: Int)
  mutating fn append_all(other: Bytes)
  mutating fn append_text(text: Text)
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
  fn sorted_by<K>(self, key: fn(T) throws -> K) rethrows -> List<T>
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
  fn join(self, separator: Text) -> Text
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

// State shared between tasks; read and change it inside `with x = s.lock() { ... }`.
builtin type Shared<T> {
  fn lock(self) -> Locked<T>
}

builtin type Locked<T> {
}

// A queue between tasks.
builtin type Channel<T> {
  fn send(self, item: T) throws
  fn receive(self) throws -> T?
  fn close(self)
}

// Raised in a task that was cancelled, at its next wait.
type Cancelled implements Error {
  fn message(self) -> Text
}

// Raised by `send` on a closed channel.
type ChannelClosed implements Error {
  fn message(self) -> Text
}

type Entry<K, V> {
  key: K
  value: V
}

type Indexed<T> {
  index: Int
  value: T
}

type Pair<A, B> {
  first: A
  second: B
}

type Failure implements Error {
  message: Text
  cause: Error? = none
  fn message(self) -> Text
}

fn print(text: Text)
fn min<T>(a: T, b: T) -> T
fn max<T>(a: T, b: T) -> T
fn assert(condition: Bool)
fn panic(message: Text) -> Never

fn __list_with_capacity<T>(capacity: Int) -> List<T>
fn __less<T>(a: T, b: T) -> Bool
```
