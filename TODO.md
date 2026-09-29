# TODO

Everything Vlad has asked for, in one place. Worked top to bottom; done
items stay (ticked) so nothing is lost. New requests are added as they come.

## Now

- [x] **Rename `Text` to `String`** (the type, its methods' docs, messages,
      the guide, stdlib, packages, tests)

- [x] **Postgres package** (`packages/postgres`, not stdlib): wire protocol v3
      over `net`, SCRAM-SHA-256 auth, parameters, typed rows, transactions,
      TLS. Installable with `lang add`.
- [x] **Redis package** (`packages/redis`): RESP over `net`, commands,
      pipelining, pub/sub.
  - [x] `lang add` of a package in a subdirectory of a repository (so both
        can live in this repo under `packages/`), and local packages
  - [x] a way for packages to take query parameters without `db`'s
        compiler magic (the `sql` module)
- [x] **LLM package** (`packages/llm`; OpenAI-compatible API, works with OpenRouter and
      co.): chat completions, streaming (SSE), tool calls, JSON output,
      embeddings; look at popular npm packages (openai, ai-sdk) for the API
- [x] **zlib on streams** (`zlib.open_gzip` / `create_gzip`): gzip/gunzip a `Stream` piece by piece (big files,
      `content-encoding: gzip` responses)
- [x] **Docs**: README for the new language, AGENTS.md (cheat sheet for
      agents), STDLIB.md (reference), DESIGN.md (stdlib decisions: why
      Postgres/Redis are packages, streams, static builds)

- [ ] **Port a small open-source project** to the language; note what's
      missing in the language and libraries and add it as I go
  - [x] rakyll/hey (load generator): section below
  - [ ] tomnomnom/gron (JSON of any shape, streams, colors): being finished
    - [x] bug: appending to a shared list doubled its capacity (OOM on deep JSON)
    - [x] `String.slice` / `index_of` were O(n) per call: ASCII strings now O(1)
    - [x] `io.stdout().write_text` was 10x slower than `print`: now shares its buffer
    - [x] reading `m[k]` then writing `m[k]` copies the value: documented (in-place paths, `take`); optimizing it: later
    - [ ] JSON numbers as written (gron prints them verbatim; `json.Value.Number` is a Float)
    - [x] Float text like JS (`12345678901234567000`, not `1.2345678901234567e+19`)
    - [x] sorting by a list key (lexicographic); `<` on lists. A comparator: not needed so far
    - [x] or-patterns in `match`: `"a" | "b" => ...`
    - [ ] `Duration` in interpolation shows its fields: needs types to choose their text (a `to_string` method used by `${}`): to raise with Vlad
    - [x] a variant named `String` can't be built by its bare name: now it can where the enum is expected, else the error names `Value.String(...)`
    - [x] guide: a one-line record example doesn't parse
  - [x] mccutchen/go-httpbin (the HTTP server API end to end): all endpoints,
        108/146 answers identical to Go's, 2552 lines vs 4099, throughput
        on par with Go (61k vs 66k req/s with logging; 26 MB vs 53 MB).
        Gaps found (/tmp/port-httpbin/GAPS.md):
    - [x] bug: chunked request bodies arrive empty (and `Expect: 100-continue`)
    - [x] bug: `+` in a path is decoded as a space; routes match raw parts
    - [x] bug: response header names are case-sensitive (two content types)
    - [x] security: cookie values aren't checked (attribute injection); `domain`
    - [x] the client's address on the request (`client_ip`)
    - [x] router: patch, any, add(method), automatic OPTIONS, 405 with allow
    - [x] query: repeated names (`query_all`), `raw_query`, `raw_path`
    - [x] crypto.md5 (digest auth, old protocols)
    - [x] status reason texts for every code, `http.status_text`
    - [x] 204/304 without content-length or content-type; Content-Length on a stream
    - [x] test helper `http.request` splits off the query
    - [ ] repeated headers; trailers; cookie Domain/Expires
    - [x] server options: bind address, body size limit (`http.serve_with`)
    - [ ] server timeouts (slow clients, idle keep-alive): needs a timer wheel first; deadlines are a locked linked list now, too slow to arm per request
    - [x] language: hex literals, `\u{...}` escapes, calling a stored
          function `r.handler(x)`, `if a is some(x) or ...` message, named
          function types take plain functions
    - [x] stdlib: seeded random (`random.seeded`), wall-clock ms
          (`time.unix_millis`), env listing (`env.all`), Duration
          `divided_by` / `is_shorter_than`, `Int.wrapping_*`
    - [x] stdlib: relative `url.parse`, `url.resolve`, `String.last_index_of`
    - [ ] JSON field renames / omit-empty: needs field attributes, to raise with Vlad
    - [x] docs: `http.bytes` example, query/path decoding, middleware order
    - [x] tests over real connections (compiler/tests/wire, curl)

- [x] **Learn from fasthttp** (benchmarks/http/README.md; next step below): why Go's fasthttp beats net/http (buffer and
      object reuse, no per-request allocations, lazy header parsing, worker
      pool...) and apply what fits to our HTTP server; measure before/after

- [x] **Top npm packages by downloads**: reviewed in research/npm-top-packages.md;
      the resulting work is the next section

- [x] **The `lang` CLI teaches the language**: tell an agent that knows
      nothing about it "run `lang help`", and it can learn the syntax and
      look up any stdlib module or function from the command line
      (`lang guide`, `lang doc http`, `lang doc http.Router`), built from the
      sources so it's never out of date
- [ ] **Designed but never built** (DESIGN.md promises them):
  - [x] `lang fmt`: indentation and whitespace (spacing inside lines: not yet)
  - [x] `Decimal` for money
  - [x] `time.timeout(duration, work)`
  - [x] `lang doc`
  - [x] a `Date` type (calendar dates apart from instants)
  - [x] decode key naming: loose matching when decoding, `json.encode_camel`
  - [ ] test doubles: a fixed clock, calling handlers without a network (done
        for http), temporary directories (done)

- [ ] **Scheduler I/O path** (from the fasthttp study): register sockets
      once edge-triggered; idle workers poll I/O themselves; per-worker run
      queues. Measure with benchmarks/http/cpu.sh (45 us/request now)
- [ ] From the newcomer-agent test (all 3 programs worked first try):
  - [x] crash on `spawn` inside a lambda -> a clear error
  - [x] `lang run` passes signals to the program (it execs it)
  - [x] guide: maps, shared state in handlers, tests with a body, task lists
  - [x] one "this call can fail" per chain, not per call (with the fixed line)
  - [x] String: character tests (is_letter of any script, is_digit, ...), index_of, trim_start/end

- [ ] From the second newcomer test (invoice with Decimal and templates,
      notes server with SQLite and WebSockets, a `tidy` tool with
      subcommands and archives: all worked, 3 compile errors total):
  - [x] `cli.decode_from<T>([])` crashed (field defaults weren't generated)
  - [x] `lang doc files` stopped halfway (a `{` inside a string), and so did STDLIB.md
  - [x] `-> Never` couldn't be written though the docs show it
  - [x] docs: `?.` in the websocket example, Decimal in SQL, no top-level
        constants, the file operations, Ctrl-C, broadcasting to listeners
  - [x] `Channel.try_send` (a slow listener must not stall a broadcast)
  - [x] errors in the source's words: `the option --count: ...`,
        `csv: line 4, column quantity: ...`, `db: row 2, column age: ...`
  - [x] `--help` shows defaults; `lang run` shows the program's name, not a temp file
  - [x] `files.info`: size, permissions, modification time
  - [x] `term.table` aligns number columns to the right
  - [x] a server started with SIGINT ignored (`&` in a script) keeps it
        ignored, and gives back the program's own Ctrl-C handler when it stops
  - [x] `archive`: streaming tar writing (`create_tar`, `add_file`, `add_dir`); real modes and times
  - [x] `lang test`: log lines show only under a failing test
  - [x] `--help`: field comments as option descriptions
  - [x] `--debug` after `process.exit`: no false leak count

## Stdlib gaps from the npm review (research/npm-top-packages.md), in order

- [x] `files.walk`, `files.glob`, `path.matches`, `files.delete_all`
- [x] `term`: colors (off when not a terminal / NO_COLOR), text width, tables,
      questions (yes/no, password), a progress line
- [x] `time`: format and parse with a pattern, time zones, `Date`,
      `parse_duration`, printing durations
- [x] `Decimal`
- [x] `http`: middleware (`router.use`), cookies, forms (urlencoded,
      multipart), `http.mime_type`, CORS, request logging
- [x] WebSockets (server and client)
- [x] **templates** (Vlad asked too; `template` module, Handlebars syntax): user-friendly; look at what's popular
      (Handlebars/Mustache, Jinja/Nunjucks, EJS, Go templates) and pick the
      shape agents know best; HTML-escaping by default
- [x] `tar` and `zip` (`archive` module)
- [x] small ones: `env.load(".env")`, `process.find`, `process.run_command`
      (dir / env / input), `random.uuid_v7`, List helpers (flat_map, unique,
      chunks, partition, index_of, find_index), log levels and JSON logs
- [x] still small: log fields (`log.with_fields`), decode key naming, cli subcommands
- [x] signals: Ctrl-C cancels `main` so `with` blocks close
- [x] `crypto`: RSA and ECDSA P-256 sign/verify, keys from PEM or JWK (Ed25519: not yet)
- [x] **markdown package** (Vlad asked; `packages/markdown`): Markdown -> HTML (CommonMark)
- [x] jwt package
- [x] semver, smtp packages
- [x] yaml package
- [x] s3 package
- [ ] packages: mysql

## From porting rakyll/hey (an agent's port: 730 lines vs Go's 1038)

- [x] false "deadlock" panic in servers under many short connections
- [x] catch blocks ending in process.exit didn't compile
- [x] HTTP client: connection reuse (keep-alive), a thread pool, total
      `timeout: Duration`, `follow_redirects`, HEAD, timings, detailed errors
- [x] writing to a closed pipe (`| head`) exits quietly (status 141)
- [x] `eprint(text)` for standard error without `with`
- [x] `try x catch err { none }` into a `T?`; `if a and x is some(v)`
- [x] `time`: parse_duration, Duration text and arithmetic, precise timers
      (macOS: sleep(10ms) takes ~70ms), a ticker (sleep_until on a grid)
- [x] `cli`: -n style flags, one-letter names, subcommands (decode_from)
- [x] channels: try_receive (a timeout: time.timeout around receive); select: not yet
- [x] `-> Never` in user code
- [x] constants: top-level `let` (Vlad chose it over `const`)
- [ ] top-level `let` of a list, map or interpolated string is rebuilt at each use: cache it if it shows up in profiles
- [x] signals: Ctrl-C cancels main's tasks; process.interrupted()
- [x] a field default can't use a type declared later in the file
- [x] Float.format: NaN text (width: use pad_start)

## Then: my own review of what's missing or weak

- [ ] Review the stdlib against Go and Node for backend/CLI gaps
- [ ] Review the language for rough edges found while writing the packages:
  - `?.` : I reached for it 3 times while writing packages (agents expect it)
  - `/` on Ints is an error (use `.div`): tripped 3 times; the error is clear
  - `catch` without `try` in front is a parse error with a vague message
- [ ] Performance: `words` benchmark is slower than Go (Text is not a view)

## Done

- [x] Language design from first principles (DESIGN.md)
- [x] Fast compiler producing efficient code (Rust → C → cc; Go-level speed)
- [x] Modules (one file = one module), packages (`lang add`, git + lock file)
- [x] Stdlib: files, process, env, log, random, json, cli, http, time
- [x] Stdlib additions: TCP/UDP (`net`), crypto, encoding, db (SQLite),
      regex, csv, url, math
- [x] Public GitHub repo, milestone commits pushed
- [x] `path`, `url`, `zlib`, client TLS (`net.connect_tls`)
- [x] `xml`
- [x] `lang build --static` (Linux; macOS explains why not needed)
- [x] Linux support tested in Docker (tools/linux)
- [x] Streams: `io.Stream` for files, connections, stdin/stdout, programs;
      reading stdin; binary chunks; `process.start`; `io.copy`
- [x] Fast HTTP file server: bytes bodies, `http.file` (sendfile, 304,
      Range, HEAD), `Router.files`
- [x] HTTP streaming both ways (`http.stream`, `http.open`), client
      requests with headers (`http.ClientRequest`, `http.send`)
