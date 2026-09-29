# What the most downloaded npm packages say about our standard library

Weekly downloads from the npm API (`api.npmjs.org/downloads/point/last-week`),
2026-09-29, for ~250 packages relevant to CLI tools and backends (front-end
and build tooling left out). Grouped by the need behind them; for each: what
we have, and the decision: **stdlib**, **package** (in the language, in this
repository or elsewhere), or **not needed** (the language already covers it).

## Already covered by the language itself

| need | packages (weekly) | how |
|---|---|---|
| deep equality, cloning, hashing, immutability | fast-deep-equal 243M, deepmerge 109M, clone 96M, object-hash 78M, immer 71M, rfdc 60M, deep-equal 35M | values: `==` and hashing are structural, assignment copies (copy-on-write) |
| structure validation of input | zod 342M, ajv 434M, joi 30M, yup 14M, valibot 23M, superstruct 7M | `decode<T>`: the record is the schema; `json.schema<T>()` exports it |
| concurrency limits | p-limit 381M, async 121M, rxjs 120M | `parallel_map(limit:)`, `spawn`, `Channel` |
| test runners, assertions | chai 141M, vitest 122M, jest 53M, mocha 17M, sinon 13M | `test` blocks, `expect`, handlers tested without a network |
| HTTP servers and body parsing | express 156M, body-parser 161M, hono 73M, fastify 16M, koa 9M | `http` |
| HTTP clients | undici 217M, node-fetch 229M, axios 133M, got 47M, superagent 26M, ky 8M | `http.send` / `open` / `download` |
| random ids | uuid 330M, nanoid 290M | `random.uuid`, `random.token` |
| temp files, spawning | tmp 91M, cross-spawn 289M, execa 193M | `files.temp_dir`, `process.run` / `start` |
| query strings | qs 209M | `url.query_text`, `req.query` |
| config from env | dotenv-expand 48M, env-var, convict | `env.decode<T>` |
| CLI arguments | commander 581M, yargs 297M, minimist 184M, meow 46M, cac 56M | `cli.decode<T>` (subcommands missing, see below) |
| password hashing, JWT signing primitives | bcryptjs 15M, crypto-js 22M | `crypto` |
| compression | pako 133M, fflate 96M | `zlib` |
| XML, CSV | fast-xml-parser 91M, xml2js 50M, csv-parse 23M, papaparse 18M | `xml`, `csv` |
| logging | pino 58M, winston 31M, debug 808M | `log` (levels and fields missing, see below) |
| LLM APIs | @anthropic-ai/sdk 46M, openai 45M, ai 30M | `packages/llm` |
| Postgres, Redis | pg 64M, ioredis 33M, redis 16M | `packages/postgres`, `packages/redis` |

## Gaps that belong in the standard library

Ordered by how much they're needed (downloads, and how often a CLI tool or
service can't do without them).

1. **Globs and walking directories.** minimatch 807M, picomatch 561M,
   glob 449M, micromatch 184M, fast-glob 174M, ignore 367M. We only have
   `files.list(dir)`. Add `files.walk(dir)` (recursive), `files.glob("src/**/*.lang")`,
   `path.matches(path, pattern)`. Go has `filepath.Glob` and `WalkDir`.
2. **Terminal output.** ansi-styles 777M, strip-ansi 590M, chalk 579M,
   string-width 574M, wrap-ansi 493M, picocolors 271M, kleur 118M,
   colorette 88M, ora 104M, cli-table3 38M, boxen 33M, prompts 70M,
   inquirer 55M, enquirer 40M, cli-progress 13M. A `term` module: colors
   and bold that switch off when output isn't a terminal or `NO_COLOR` is
   set; the display width of text; aligned tables; asking a question,
   yes/no, a password without echo; a progress line.
3. **Dates and times.** date-fns 115M, dayjs 80M, luxon 46M, moment 41M,
   ms 627M, humanize-duration 4M. `time` has only UTC `DateTime` with ISO
   text. Add: formatting and parsing with a pattern, time zones (the
   system's zone database), a `Date` for calendar days (designed, not
   built), `time.parse_duration("1h30m")` and printing durations.
4. **Decimal numbers.** decimal.js 101M, bignumber.js 71M, big.js 39M.
   Money can't be `Float`. `Decimal` is designed, not built.
5. **HTTP server pieces.** cookie 245M, cors 92M, express-rate-limit 71M,
   busboy 36M, multer 25M, helmet 17M, cookie-parser 14M, mime-types 308M.
   Middleware (`router.use(...)`: logging, auth, CORS in one place);
   cookies (`req.cookie(name)`, `res.with_cookie(...)`); forms
   (urlencoded and multipart with files); `http.mime_type(path)`.
6. **WebSockets.** ws 310M. Real-time backends need them; server and
   client in `http`.
7. **Templates.** ejs 49M, handlebars 47M, mustache 20M, nunjucks 4M;
   escape-html 137M, entities 344M, he 49M. Server-rendered pages, emails
   and generated files. A small logic-less template language
   (`{{name}}`, sections, lists) that escapes HTML by default, filled from
   a record like `json.encode` sees it. Go has `text/template` and
   `html/template`.
8. **Archives.** tar-stream 118M, tar 100M, jszip 52M, archiver 50M,
   unzipper 27M, adm-zip 26M. `tar` and `zip` reading and writing (Go:
   `archive/tar`, `archive/zip`); we already have gzip.
9. **Small conveniences.**
   - `.env` files: dotenv 207M. Add `env.load(".env")`.
   - Finding programs: which 392M. Add `process.find("git")`.
   - Process options: execa's reason to exist. Add `cwd`, `env` and input to `process.run`.
   - Byte sizes: bytes 155M, pretty-bytes 36M. Add formatting of sizes.
   - Case conversion: camelcase 208M, change-case 35M. Add decode key naming, `keys: camel_case` (designed, not built).
   - Time-ordered ids: `random.uuid_v7()` for database keys.
   - List helpers: lodash 193M. Missing `flat_map`, `unique`, `chunks`, `partition`, `index_of`, `find_index`, `sum_by`, `window`.
   - Logging: debug 808M. Add levels chosen by `LOG_LEVEL`, fields (`log.info("saved", {"id": id})`), and JSON output for services.
   - CLI: commander's subcommands. `cli` needs git-style subcommands.
10. **Signals.** signal-exit 330M. A CLI tool that must clean up on Ctrl-C
   (temp files outside `with`, a terminal state) has no way to. Needs a
   design that fits structured concurrency (e.g. Ctrl-C cancels `main`'s
   task, so `with` blocks close and waits stop with `Cancelled`).

## Packages (not stdlib)

- **Protocol and service clients.** Each has many ways to be used, changes
  with its server, and can be written in the language on `net`, `crypto`
  and `sql`:
  - databases: mysql (mysql2 17M), MongoDB;
  - SMTP mail: nodemailer 26M, much easier now that `net.start_tls` exists;
  - S3 and AWS: @aws-sdk/client-s3 51M (SigV4 is SHA-256 and HMAC);
  - Stripe, Twilio;
  - message queues: kafkajs, amqplib, mqtt, nats;
  - GraphQL clients: graphql 55M;
  - gRPC and protobuf: 99M + 63M;
  - msgpack: 43M.
- **Formats with many dialects or heavy dependencies:**
  - Markdown: marked 89M, markdown-it 32M;
  - HTML parsing and querying: jsdom 117M, cheerio 31M;
  - Excel: xlsx, exceljs;
  - PDF: pdf-lib, pdfkit;
  - images: sharp 116M, image-size 35M;
  - file type detection: file-type 65M;
  - QR codes: qrcode 30M;
  - YAML: js-yaml 340M, yaml 241M. YAML is on the edge: many CLI tools read YAML configs. A `yaml.decode<T>` on top of our generated decoders would be cheap once there's a parser, so it could move into the stdlib later.
- **Tools:**
  - semver 975M (npm's own), as a package;
  - diff 165M;
  - cron expressions: cron, node-cron;
  - LRU cache: lru-cache 647M, written in the language on top of `Map`;
  - metrics: prom-client 11M;
  - JWT: jose 155M, jsonwebtoken 65M. HS256 can be written now, but RS256 and ES256 need asymmetric crypto (below).

## Not needed

- Build tools and front-end: typescript, esbuild, prettier (our `lang fmt`
  is designed), cross-env, nodemon, pm2.
- Polyfills and helpers the language removes:
  - signal-exit's old-Node workarounds;
  - rimraf and mkdirp (`files.delete` and `make_dir` should just be recursive);
  - fs-extra;
  - clone and deep-merge;
  - iconv-lite for UTF-8-only work, though a `latin1` / `windows-1251` decoder may still be worth adding for old files and services.

## A gap the list exposes in `crypto`

Asymmetric signatures are needed for JWT (RS256 and ES256 are the common
algorithms of identity providers), webhooks of some services, and SSH.
There are no npm packages for these because Node's built-in `crypto` has
them. Our `crypto` needs Ed25519 and ECDSA P-256 signing and verification,
and RSA verification. They come from the system: CommonCrypto/Security on
macOS, OpenSSL on Linux.
