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
  - [ ] one "this call can fail" per chain, not per call
  - [x] String: character tests (is_letter of any script, is_digit, ...), index_of, trim_start/end

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
- [ ] still small: decode key naming, log fields, cli subcommands
- [x] signals: Ctrl-C cancels `main` so `with` blocks close
- [x] `crypto`: RSA and ECDSA P-256 sign/verify, keys from PEM or JWK (Ed25519: not yet)
- [x] **markdown package** (Vlad asked; `packages/markdown`): Markdown -> HTML (CommonMark)
- [ ] packages: jwt, smtp, s3, mysql, semver, yaml (maybe stdlib)

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
- [x] `cli`: -n style flags and one-letter names (subcommands: still to do)
- [x] channels: try_receive (a timeout: time.timeout around receive); select: not yet
- [ ] `-> Never` in user code; constants (`const`)
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
