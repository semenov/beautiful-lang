# The standard library against Go and Node (2026-09-29)

A review of lang's standard library against what Go's standard library and
Node (with its most used npm packages) give programmers for backends and
command-line tools. Method: every module's `lang doc`, `lang guide`,
DESIGN.md (the stdlib decisions: protocol clients, YAML, Markdown, JWT,
LRU caches and so on are packages on purpose), TODO.md, HANDOFF.md and
`research/npm-top-packages.md` (already acted on: globs, `term`, dates and
zones, `Decimal`, middleware, cookies, forms, WebSockets, templates,
archives, `.env`, log levels and fields, Ctrl-C, RSA/ECDSA). Where unsure,
a small program was run (kept in /tmp, not here); findings from those runs
are marked **(tried)**. Items already open in TODO.md are not re-reported,
except where their weight is worth restating (marked **(in TODO)**).

## Summary

The library is broad for its age: a typical CLI tool or JSON service can be
written without packages, and the npm review's list is done. What remains
is mostly *depth inside modules that exist*, not missing modules. The
weakest places:

1. **`regex`** is POSIX ERE: no `(?:...)`, no lazy `*?`, no named groups, no
   `\b`, and `\p{L}` compiles but silently matches nothing **(tried)**.
   Agents and people write RE2 / JavaScript syntax by reflex.
2. **`crypto`** has no symmetric encryption (AES-GCM), no SHA-512 or
   HMAC-SHA512, no HKDF, no hashing of a stream, no key generation.
3. **The program's surroundings**: no current directory, home / config
   directories, pid, hostname or OS name; child programs can't use the
   terminal (no `$EDITOR`, `git commit`, pagers), can't be signalled, and
   binary output has no `Bytes` form. And a bug: `time.timeout` doesn't
   stop `process.run`: the wait for a child isn't cancellable **(tried)**.
4. **`time.DateTime`** keeps whole seconds only: `parse_iso` drops
   `.123` **(tried)**, and there is no `Duration` between two moments.
5. **HTTP server structure**: no route groups / sub-routers with their own
   middleware, no request-scoped values from middleware to handlers, no
   response compression, no streaming of request bodies, no TLS. And a bug:
   two servers in one program don't stop together **(tried)**.
6. **`files`**: no `chmod`, symlinks, atomic writes, temp files, locks or
   watching.

None of these needs a new language feature except file embedding (and
`select`, already decided "later"); most are one to five functions each.

## Gaps by importance

### High: most programs of the kind hit it, and the workaround is poor

| # | gap | why it matters | suggested shape |
|---|---|---|---|
| 1 | regex syntax: `(?:...)`, lazy `*? +? ??`, named groups, `\b` `\B`, `\p{L}` classes; an error (not a silent non-match) for anything unsupported | Every regex written from memory or copied from Go/JS/Python uses these; today they fail to compile, and `\p{L}` quietly never matches. Go's RE2 is the natural target: linear time, no backreferences, same syntax as JS for the common parts | keep `regex.compile`; RE2 syntax; `Match.named(name) -> String?`; `r.replace_with(text, m => ...)` for computed replacements |
| 2 | symmetric encryption: AES-256-GCM | Encrypted cookies and sessions, secrets and tokens at rest, encrypted fields in a database. Node `createCipheriv`, Go `crypto/cipher`. Without it people hand-roll XOR or skip encryption | `crypto.encrypt(key: Bytes, data: Bytes) -> Bytes` (random nonce prepended, tag appended), `crypto.decrypt(key, data) throws -> Bytes`; `crypto.random_bytes(32)` for keys |
| 3 | where the program is: current directory, home / config / cache directories, absolute paths | Every CLI tool resolves arguments against the current directory and keeps config in `~/.config/<tool>` (Go `os.Getwd`, `os.UserHomeDir`, `os.UserConfigDir`, `filepath.Abs`; Node `process.cwd()`, `os.homedir()`). `env.get("HOME")` works on Unix only and misses XDG rules | `process.dir() -> String`, `path.absolute(p) -> String` (against `process.dir()`), `files.home_dir()`, `files.config_dir()`, `files.cache_dir()` (XDG on Linux, `~/Library/...` on macOS) |
| 4 | running a program on the terminal (inherited stdin / stdout / stderr) | Opening `$EDITOR` for a commit message, running `git`, `ssh`, `docker` with their progress bars, pagers. `process.run` and `start` always pipe all three streams **(tried: runtime source)**. Node's `stdio: "inherit"` is execa's most used option; Go sets `cmd.Stdin = os.Stdin` | `Command.attached: Bool = false` (all three streams are this program's), used by `process.run_command`; the result's status only |
| 5 | `DateTime` with sub-second precision, and the time between two moments | JSON APIs send `2026-09-29T12:00:00.123Z` and Unix milliseconds; parsing then printing loses the fraction **(tried)**. Expiry checks ("token older than 15 minutes"), ages and log times need `a - b` as a `Duration`. Go `time.Time` has nanoseconds and `Sub`; JS `Date` has ms | a `nanos: Int` field (0-999,999,999) on `DateTime` / `Zoned`, printed by `iso()` when non-zero; `moment.since(earlier) -> Duration`, `plus(d: Duration)`, `time.from_unix_millis(ms)`, `to_unix_millis()`; `%f` / `%3f` in patterns |
| 6 | route groups and mounted routers with their own middleware | Real backends protect `/api/admin/*` with auth and leave `/health` and `/login` open. Today middleware is global, so every middleware re-checks paths by hand. chi `r.Route` / `r.Group`, Express `app.use("/api", router)`, Go 1.22 `ServeMux` patterns | `router.mount("/admin", admin_router)` (the sub-router's middleware applies only under the prefix); or `router.group("/api", g => { g.use(auth); g.get(...) })` |
| 7 | values passed from middleware to the handler | The authenticated user, a request id, a tenant: Express `req.user`, Go `context.WithValue`. The workaround is writing a fake header in middleware (`r.headers["x-user"] = id`), which is easy to get wrong | `Request.values: Map<String, String> = {}` plus `req.value(name) -> String?` and `req.with_value(name, v) -> Request` (strings keep it simple; typed data decoded from them) |
| 8 | `chmod`, atomic writes, temp files | An installer or generator must make a file executable (`chmod +x`); a config or state file must not be half-written when the program is killed (write to a temp file, then rename). Go `os.Chmod`, `os.CreateTemp`, `renameio`; Node `fs.chmod`, `write-file-atomic` (40M/week) | `files.set_mode(path, 0o755)`; `files.write` made atomic (temp + rename in the same directory) or `files.write_atomic`; `files.temp_file() -> TempFile` (like `TempDir`, deleted at the end of `with`) |
| 9 | `time.timeout` doesn't stop `process.run` (bug) | `time.timeout(time.seconds(1), () => try process.run("sleep", ["5"]))` returns after 5 s with the result, no `TimedOut` **(tried)**. DESIGN.md promises every wait is cancellable; a hung `git fetch` or `ffmpeg` hangs the tool | waiting for a child becomes a cancellable wait: on cancel, SIGTERM, a short grace period, SIGKILL, then `Cancelled`; also `Command.timeout: Duration` |

### Medium: common in one kind of program, or a workaround exists but is awkward

| # | gap | why it matters | suggested shape |
|---|---|---|---|
| 10 | `select` over channels / receive with a timeout **(decided "later" in HANDOFF)** | Worker loops that also watch a quit channel, fan-in, per-receive deadlines. `time.timeout` around `receive` works for one channel only | `select { msg = jobs.receive() => ..., _ = quit.receive() => ..., after time.seconds(5) => ... }` (a language change: raise with Vlad) |
| 11 | response compression | JSON and HTML compress 5-10x; Express `compression` (25M/week), Go `gzhttp`. The client already asks for gzip; the server never gives it | `router.use(http.compress())`: gzip when the client accepts it, the type is text-like and the body is over ~1 KB |
| 12 | streaming request bodies | Uploads bigger than `max_body` (64 MB) are refused; everything below is held in memory. Go gives `r.Body` as a reader; busboy streams | `req.stream() -> io.Stream` for routes added with `router.upload(pattern, handler)` (no size limit, body not buffered) |
| 13 | HTTPS serving (and client certificates, custom CAs) | `net` says "put a proxy in front", which fits cloud deployments but not internal services, dev servers, mTLS between services or a single-binary tool. Go `ListenAndServeTLS`; Node `https.createServer` | `ServerOptions.cert_file` / `key_file`; `ClientRequest.ca_file`, `cert_file`, `key_file`; `net.serve_tls(port, cert_file:, key_file:, handler:)` |
| 14 | HTTP client retries with backoff | Calls to third-party APIs fail with 429/503 and resets; axios-retry, p-retry (30M), got's built-in `retry`, Go `retryablehttp`. Everyone rewrites the loop | `ClientRequest.retries: Int = 0` (idempotent methods; connect errors, 429, 502-504; exponential backoff with jitter; honors `Retry-After`); a generic `time.retry(times:, work:)` for other calls |
| 15 | multipart and file bodies in the client | Uploading a file to an API (Slack, S3 presigned POST, OpenAI files) needs a hand-built multipart body; big uploads must fit in memory | `http.multipart(parts: List<Part>) -> ClientBody` (or sets `body` and `content-type` on a `ClientRequest`); `ClientRequest.body_file: String = ""` streamed from disk |
| 16 | hashing a stream or a file | Checksums of downloads and uploads (sha256 of a 2 GB file), ETags, S3 `x-amz-content-sha256`: today the whole file must be read into `Bytes` | `crypto.sha256_file(path) throws -> Bytes`, or a `crypto.Hasher` with `add(data)` / `finish()` for any stream |
| 17 | more hashes and KDFs: SHA-512 / SHA-384, HMAC-SHA512, HKDF, Ed25519 **(Ed25519 in TODO)**, key generation | HS512 and ES384 JWTs, webhooks that sign with SHA-512, deriving several keys from one secret; `generate_key` for tests and for issuing tokens (today keys come only from PEM) | `crypto.sha512`, `hmac_sha512`, `hkdf_sha256(secret, salt:, info:, length:)`, `crypto.generate_key("ES256") -> PrivateKey`, `private_key.public_key()`, `.pem()` |
| 18 | process control: pid, signals to children, separate stderr, bytes output | Stopping a child gracefully (SIGTERM, then kill), supervisors, `kill -HUP` for reload; capturing a program's binary output (`git cat-file`, image tools) since `Output.stdout` is `String` (tied to the open UTF-8 question) | `Process.id()`, `Process.signal("TERM")`, `Process.stderr() -> io.Stream` (when `Command.separate_stderr`), `Output.stdout_bytes`; `process.id()`, `process.hostname()`, `process.os()` ("macos" / "linux"), `process.arch()` |
| 19 | CSV options and streaming | Semicolon CSV (Excel in most of Europe), TSV, files larger than memory, and writing records (`csv.encode` takes only string rows). Go `csv.Reader.Comma`; papaparse / csv-parse options | `csv.parse_with(text, options: csv.Options(separator: ";"))`; `csv.open(path) -> csv.Reader` with `read_row()`; `csv.encode_records<T>(rows: List<T>)` (header from the fields) |
| 20 | navigating `json.Value` | Webhooks and loosely specified APIs: `payload.data.object.id` needs a `match` per level. gjson (Go), plain `obj.a.b` in JS | `v.get("data")?.get("id")?.string()`; `v.at(0)`, `v.string()`, `v.int()`, `v.bool()`, `v.items()`; `json.decode_value<T>(v)` to go from a part to a record |
| 21 | strings: cut at the first separator, prefixes, split with a limit, number bases | `"key=a=b".split("=")` gives three parts **(tried)**; parsing `key=value`, headers and `prefix:rest` is daily work. Go `strings.Cut`, `TrimPrefix`, `SplitN`, `strconv.ParseInt(s, 16)`; JS `split(sep, n)`, `parseInt(s, 16)` | `s.split_once("=") -> Pair<String, String>?`, `s.trim_prefix(p)`, `s.trim_suffix(p)`, `s.split(sep, limit: 2)` (a second function if no default parameters), `s.replace_first(a, b)`, `s.count(part)`, `text.to_int_base(16)`, `n.to_string_base(16)` |
| 22 | CLI details: enum values in lower case, short aliases, `--version`, subcommand help | `--format json` fails for `enum Format { Text Json }` (it wants `Json`), and `--help` shows `<text>` instead of the choices **(tried)**. A field is either `-v` or `--verbose`, never both. commander / cobra do these | enum values matched ignoring case and listed in `--help` ("text, json"); a field comment tag or naming rule for a short alias (e.g. a record `aliases: {"verbose": "v"}` option on a `cli.decode_with`); `cli.Options(version: "1.2.0")`; `cli.subcommands` help that lists the commands |
| 23 | network basics: DNS lookup, listening on a host / port 0, Unix sockets, IP addresses | Health checks and service discovery (`net.LookupHost`, SRV); tests that start a server on a free port; the Docker socket and local daemons (`/var/run/*.sock`); allow-lists by CIDR | `net.lookup(host) -> List<String>`, `net.lookup_txt`, `lookup_mx`, `lookup_srv`; `net.listen(host:, port: 0) -> Listener` with `port()` and `accept()`; `net.connect_unix(path)`, `http.ClientRequest.unix_socket`; `net.parse_ip`, `net.parse_network("10.0.0.0/8").contains(ip)` |
| 24 | a server's two listeners stop together (bug) | A program serving an API and a metrics/admin port: on SIGTERM one `http.serve` returns, the other keeps waiting in `accept` until a new connection arrives (one global listen fd in the runtime) **(tried)** | every `serve` watches the stop flag and its own fd; `http.serve` returns in each task |
| 25 | binary data: little-endian, signed and float numbers, varints | The mysql package (in TODO) speaks a little-endian protocol; msgpack, protobuf, WAV/PNG/ZIP headers too. `Bytes.int_at` / `append_int` are unsigned big-endian only | `b.int_at(offset, size:, little_endian: true)` (or `int_le_at`), `b.signed_at`, `b.float_at`, `append_int_le`, `append_float`; `encoding.varint` / `from_varint` |
| 26 | database: large results, migrations, dynamic filters **(pool in TODO)** | `query<T>` returns a `List`: a 5M-row export must fit in memory. Every service needs schema migrations (golang-migrate, knex, Prisma migrate). Optional filters, `IN (...)` and a user-chosen `ORDER BY` can't be built since SQL must be a literal: workarounds exist (`? is null or name = ?`, `json_each(?)`, a `match` over literal queries) but aren't documented | `conn.each<T>(sql, params, row => ...)` (rows one at a time); `db.migrate(conn, dir: "migrations")` (numbered `.sql` files, a version table, one transaction each); document the dynamic-filter patterns in `lang doc sql` |
| 27 | testing: a fixed clock **(in TODO)**, running one test, all files of a project, benchmarks, a server for client code | `lang test` takes one file and runs all of it; Go has `-run`, `./...`, `testing.B`, `httptest.NewServer`; Node `--test-name-pattern`. Code that calls HTTP APIs can't be tested without a real server | `lang test [dir or file] [--match text]`; `bench "name" { ... }` blocks with `lang test --bench`; `http.test_server(router) -> TestServer` (a free port, `url`, closed by `with`) |
| 28 | embedding files in the binary | One file to deploy with its templates, static assets and SQL migrations (Go `//go:embed`, used by most Go web apps; Node needs `pkg`/SEA). A language/tooling change: raise with Vlad | `let assets = embed("public")` at the top level giving a read-only `files`-like view; `template.load_embedded`, `router.files_embedded` |
| 29 | Set operations and more collection helpers | `union`, `intersection`, `difference`, building a Set from a list, `sum_by`, `window` (the npm review listed the last two; not built), `Map.filter`, `map_values`. Go 1.21 `slices`/`maps`, lodash | `a.union(b)`, `a.intersection(b)`, `a.difference(b)`, `xs.to_set()`, `xs.sum_by(key)`, `xs.windows(size)`, `m.filter(e => ...)`, `m.map_values(v => ...)` |
| 30 | rate limiting | Login endpoints and public APIs need per-client limits (express-rate-limit 71M/week, Go `x/time/rate`); calling a rate-limited API needs a limiter too | `router.use(http.rate_limit(per_minute: 60, key: req => req.client_ip))`; `time.Limiter(per_second: 10)` with `try limiter.wait()` |

### Low: specialized, or a package / workaround is fine

| # | gap | why | suggested shape |
|---|---|---|---|
| 31 | symlinks and other file details | Tools that manage dotfiles, deploys (`current -> releases/42`), `ln -s`; `FileInfo.is_symlink`; setting times (`touch`); hard links | `files.symlink(target, at:)`, `files.read_link`, `FileInfo.is_symlink` via `files.link_info`, `files.set_modified(path, seconds)` |
| 32 | file watching | Dev tools and hot reload: chokidar (100M/week), fsnotify | `files.watch(dir) -> Channel<Change>` (kqueue / inotify), closed by `with` |
| 33 | skipping directories while walking | `walk` visits `node_modules` and `.git` and then the program filters; `.gitignore` rules (ignore, 367M/week) | `files.walk_with(dir, skip: dir => dir.ends_with("/node_modules"))`; `files.walk_ignoring(dir, ignore_file: ".gitignore")` |
| 34 | file locks | Two copies of a CLI tool or cron job must not run at once | `with lock = try files.lock(path) { ... }` (flock), `files.try_lock` |
| 35 | URL building and repeated query keys | `Url` has no text form (`"${u}"` prints the record), and `query` keeps only the last of `?x=1&x=2` **(tried)** | `u.to_string()` / `u.text()`; `Url.query_all(name)` or `query: List<Pair>`; `url.with_query(base, params)` |
| 36 | terminal size and input | Wrapping help text and tables to the window; `is_terminal` covers stdout only | `term.columns() -> Int?`, `term.wrap(text, columns)`, `term.is_input_terminal()`; a choice list (`term.choose(question, options)`) like prompts / inquirer |
| 37 | reading big archives piece by piece | `read_tar` / `read_zip` take `Bytes`: a 3 GB `.tar.gz` doesn't fit | `archive.open_tar(path) -> TarReader` with `next() -> Entry?` and the entry's data as a stream |
| 38 | more encodings: base32, CRC-32, text encodings | TOTP 2FA secrets are base32 (every login-with-2FA service); CRC-32 for formats; Latin-1 / Windows-1251 files from old systems | `encoding.base32(data)`, `encoding.from_base32`; `crypto.crc32(data) -> Int`; `encoding.decode_latin1(bytes) -> String` |
| 39 | periodic jobs and cron | Cleanup every hour, a nightly report: `sleep_until` in a loop works; cron expressions are a package (node-cron, robfig/cron) | `time.every(time.minutes(5), () => try cleanup())` ending when the task is cancelled; cron as a package |
| 40 | template helpers | Handlebars programs format dates and money inside templates (`{{format price}}`); here the data must be prepared first. Probably deliberate (logic-less) | leave it; or a fixed set of formatters: `{{date created "%d %b"}}`, `{{money total}}` |
| 41 | metrics and runtime stats | Prometheus scraping (prom-client 11M, Go `expvar`, `runtime.MemStats`), a health page showing memory and task counts | `process.memory() -> Int` (RSS), `process.tasks() -> Int`; Prometheus text format as a package |
| 42 | other signals | `SIGHUP` to reload config, `SIGUSR1` to reopen logs; Ctrl-C and TERM are handled | `process.signals(["HUP"]) -> Channel<String>` closed by `with` |
| 43 | numbers: `Int` limits, `Float.is_finite`, big integers | Edge checks, 128-bit ids, crypto packages written in the language; `BigInt` in JS, `math/big` | `Int.max()` / `Int.min()`, `x.is_finite()`, `math.BigInt` (low) |
| 44 | XML namespaces and streaming | SOAP and feeds with `xmlns`; big XML exports | `Element.namespace`, `xml.open(path)` pull reader |
| 45 | zstd / brotli | Newer HTTP and archive formats (Node has brotli; Go needs packages) | `zlib.brotli` later, or a package |
| 46 | Unicode normalization | Comparing user names and file names from macOS (NFD) | `s.normalized()` (NFC) |

## Notes by area

### Files and paths

Has: whole-file read/write/append, streams (`open`, `create`,
`open_append`), `list`, `walk`, `glob`, `exists`, `is_dir`, `info` (size,
mode, mtime), `make_dir` (with parents), `copy`, `rename`, `delete`,
`delete_all`, `temp_dir`, typed errors (`NotFound`, `PermissionDenied`,
`IsADirectory`, `AlreadyExists`); `path` for joining, parts, `clean`,
`matches`.

Missing against Go `os`/`path/filepath`/`io/fs` and Node `fs`/`fs-extra`:
the current directory and `path.absolute` / `path.relative(from, to)`
(`filepath.Rel`, used to print paths relative to a project root); home,
config and cache directories; `chmod`; atomic writes; a temp *file*;
symlinks; `touch`; locks; watching; walk with skipping; random access in a
file (seek; only needed by format readers). `path.relative` belongs with #3.

Matters: #3 and #8 are daily for CLI tools; the rest are low.

### Processes and signals

Has: `run` (arguments as a list, no shell), `run_command` (dir, env, input),
`start` for streaming both ways, `find` (which), `args`, `exit`,
`interrupted`; Ctrl-C / SIGTERM cancel `main` so `with` blocks close; writing
to a closed pipe exits quietly.

Missing: attached children (#4), pid / signals / separate stderr / bytes
(#18), own pid, hostname, OS and architecture (a CLI that downloads the
right binary, or prints `version (darwin/arm64)`), other signals (#42), and
`time.timeout` around `run` doesn't stop the child (#9, a bug). `exec` (replace this process) is rare.

### Environment and config

Has: `env.get`, `env.all`, `env.decode<T>`, `env.load(".env")`. With the
yaml package and `json.decode`, config files are covered. Missing only
`env.set` for the process itself (rare; `Command.env` covers children). No
gap worth a row.

### CLI parsing

Has: `cli.decode<T>` with defaults, optional fields, repeated options into
lists, `--` handling, generated `--help` with field comments, enums, `-n`
one-letter fields, subcommands through `decode_from` and a `match`.

Weak: enum values must be written as the variant (`Json`), help shows
`<text>` for enums (#22); no `-v` + `--verbose` for one field; no
environment fallback for an option (`--token` or `$TOKEN`, common in
cobra/viper); no `--version`; subcommands' usage is written by hand; no
shell completion (low).

### Logging

Has: levels with `LOG_LEVEL`, text or JSON lines with `LOG_FORMAT`, fields
through `with_fields`, request logging middleware. That's the core of
pino / slog. Missing and low: a level set from code, writing to a file
(12-factor apps log to stderr anyway), non-string field values in JSON
(numbers come out as strings), a request id in `log_requests`.

### Time and dates

Has: `Duration` (parse, print, arithmetic), `Instant`, `sleep`,
`sleep_until`, `timeout`, `DateTime` (UTC), `Date`, `Zoned` with the system
zone database, strftime `format` and `parse`, `plus_months`, weekday.

Weak: whole seconds only (#5); no `Duration` between two `DateTime`s (only
`to_unix()` differences in Int seconds) and no `plus(Duration)`; no Unix
milliseconds in / out of `DateTime` (`unix_millis()` exists but only for
now); HTTP dates (RFC 1123) have no named format
(`%a, %d %b %Y %H:%M:%S GMT` as a pattern works). A fixed clock for
tests is in TODO. Relative text ("3 minutes ago") is low.

### Text, strings, regex, Unicode

Has: the usual String methods, Unicode letter categories and case mapping,
`code_points`, `Int.character`, `term.width` for display width, `pad_*`,
`Float.format`, `Decimal.format`.

Weak: regex (#1: the biggest single gap in the review); `split_once`,
`trim_prefix`, `split` with a limit, number bases (#21); text wrapping
(#36); normalization (#46). Also no `String.compare` / case-insensitive
equality helper (`lower()` on both sides works).

### Encoding

Has: JSON (records, dynamic values, exact numbers, key styles, schema),
CSV (parse, decode into records, encode), XML (tree), base64 (standard and
URL), hex, URL parts and percent-encoding, templates.

Weak: `json.Value` has no accessors (#20); CSV has no separator option, no
streaming and no record encoding (#19); base32 (#38); URL text form and
repeated query keys (#35). Missing and low: streaming JSON for huge arrays
(JSON lines work with `read_line`), XML namespaces and pull parsing.
msgpack / protobuf are packages (the npm review) but need #25 first.

### Crypto

Has: SHA-256, SHA-1, MD5, HMAC-SHA256, PBKDF2, password hashing, constant
time compare, secure random, RSA and ECDSA P-256 sign/verify with keys from
PEM or JWK.

Missing: AES-GCM (#2), SHA-512 family, HMAC-SHA512, HKDF, key generation,
Ed25519 (in TODO), streaming hashes (#16), CRC-32. Password hashing is
PBKDF2 only: verifying bcrypt / argon2 hashes from an existing user table
(Node apps mostly used bcrypt) needs a package or a migration on login;
worth a note in `lang doc crypto` (low). x509 certificate parsing (expiry
monitors) is low.

### Networking

Has: TCP client and server (a task per connection), TLS client and
STARTTLS, UDP, HTTP client and server, WebSockets, proxies.

Missing: DNS lookups, host binding and port 0 for `net.serve`, a listener
object, Unix sockets, IP / CIDR types (#23); TLS serving (#13). HTTP/2 is
low for backends behind a proxy but blocks gRPC (a package would need it).

### HTTP server

Has: router with params and wildcards, 405 / OPTIONS / HEAD, middleware,
CORS, request logging, cookies, forms and multipart, files with sendfile /
Range / 304, streaming responses (SSE), WebSockets, timeouts, body limit,
bind address, handlers testable without a network, in-flight requests
finish on Ctrl-C **(tried)**.

Missing: groups / mounting (#6), request values (#7), compression (#11),
streaming uploads (#12), TLS (#13), rate limiting (#30), the two-listeners
bug (#24), repeated request headers (in TODO). Low: security headers
(helmet), request ids, sessions (easy once #2 exists: an encrypted cookie),
CSRF tokens, `ETag` for dynamic responses, trusted-proxy handling for
`client_ip`.

### HTTP client

Has: `send` with method, headers, body, timeout, redirects, proxies,
insecure, keep-alive, gzip, timings, streaming responses (`open`),
`download`, WebSocket client, cookies read from responses.

Missing: retries (#14), multipart and file bodies (#15), custom CA and
client certificates (#13), a cookie jar across requests (low: sessions
with scraped sites), basic auth helper (low: a header), Unix sockets (#23).

### Databases

Has: SQLite (system library 3.54: JSON functions and FTS5 work **(tried)**),
typed rows, transactions, `insert` returning the id, SQL-injection-proof
literal queries; `sql` shared by the postgres package; redis package.

Missing: #26 (streaming rows, migrations, dynamic-filter guidance), a pool
(in TODO), mysql (in TODO), `sql.Value` for `Decimal` / dates as documented
types (Decimal goes as text: fine). Decoding is strict (an integer column
into a `String` field is an error **(tried)**), which is right but worth a
sentence in the doc with `cast(x as text)`.

### Concurrency

Has: `spawn` with structured lifetimes, `parallel_map(limit:)`, `Shared<T>`
with `with` locking and deadlock reports, `Channel` with `try_send` /
`try_receive`, cancellation, `time.timeout`.

Missing: `select` (#10, decided "later"); a rate limiter (#30); read/write
locks (low: `Shared` serializes readers; matters only for read-heavy shared
caches); a periodic job helper (#39). A semaphore for unstructured work
(limit concurrent calls from many request handlers to one API) can be made
with a `Channel` of capacity n; worth a guide example rather than an API.

### Compression and archives

Has: gzip / zlib in memory and as streams, tar and zip in memory, tar
written as a stream, safe `extract`. Missing: streaming reading of archives
(#37), zstd / brotli (#45). Fine for most tools.

### Templates

Has: Handlebars with escaping, partials, `load(dir)`, errors for missing
names. Missing only helpers (#40), probably by design.

### Testing

Has: `test` blocks, `expect` with both sides printed, `expect throws`,
parallel tests, temp dirs, HTTP handlers without a network, seeded random.
Missing (#27): a fixed clock (in TODO), selecting tests, running a project,
benchmarks, a test server for client code. Snapshot / golden files are low
(files + `expect` do it).

### Other things Go and Node programmers use daily

- **Embedding files** (#28): `go:embed` is the usual way to ship a web app
  as one binary.
- **Runtime and build info**: version of the program (`-ldflags -X`,
  `debug.ReadBuildInfo`), OS / arch (#18). A `lang build --set version=1.2`
  or a generated constant would help CLI `--version`.
- **Profiling a live service** (`net/http/pprof`, `--inspect`): low for
  now; perf works on Linux.
- **Email address and header parsing** (`net/mail`): low; the smtp package
  can carry it.
- **Deliberately packages** (not gaps): Postgres, Redis, MySQL, YAML,
  Markdown, JWT, semver, SMTP, S3, LLM APIs, LRU caches, cron expressions,
  Prometheus format, msgpack / protobuf.
