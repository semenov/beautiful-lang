# Gaps found while porting the RealWorld "Conduit" backend to lang

Severity: **major** (wrong behavior a user of the server would hit, or a
workaround of 50+ lines / a different design), **minor** (a few lines or
a detour), **docs** (the docs misled or said nothing). Repros are in
`repro/`; each one says what was expected and what happened.

Learned only from `lang help`, `lang guide`, `lang doc`, `AGENTS.md` and
`STDLIB.md`. The port passes the official suite (hurl, 154/154 requests)
and 486 of 488 assertions of the last full Postman collection (the two
failures contradict the hurl suite; see README.md).

## Ranked

| # | Gap | Sev. | What I did instead | Repro |
|---|-----|------|--------------------|-------|
| 1 | **`db.Connection.transaction` breaks when tasks share the connection**, which is how an HTTP server uses it (`insert` even says "Safe when several tasks share the connection"). Two requests in a transaction at once: the second fails with `cannot start a transaction within a transaction` (3 of 200 concurrent article creations got a 500). Worse, a statement another task runs while a transaction is open *joins* it: if the transaction rolls back, the other task's insert, which succeeded, silently disappears. | major (bug, data loss) | Every write goes through `App.write`, a `Shared<Int>` used as a mutex (`api.lang`), wrapped around each insert/update/delete and transaction. 400/400 concurrent creates then succeed. | `repro/shared_connection_transaction.lang` |
| 2 | **No connection pool: every query of every request runs one at a time.** 8 tasks each running a 220 ms query take 1.79 s. `GET /api/articles` does ~1300 req/s with 1 client and ~1190 with 16 on a 14-core Mac. Nothing in the docs says how a server should hold its database (one shared connection, one per request, a pool). | major (performance) | One shared connection; accepted the ceiling | `repro/serialized_queries.lang` |
| 3 | **Router: the first matching route wins, a `:param` beats a fixed path registered after it.** `GET /api/articles/feed` went to `/api/articles/:slug` with slug `feed`. Go's routers (net/http 1.22, httprouter, gin, chi) pick the most specific route in any order; `lang doc http.Router` doesn't mention order. | major (silent wrong route) | Register `/api/articles/feed` first, with a comment | `repro/route_order.lang` |
| 4 | **`json.decode<T>` can't tell a missing field from `null`.** Both give `none`. The RealWorld updates need the difference: `PUT /api/user` with `{"bio": null}` clears the bio, `{}` keeps it, `{"email": null}` is a 422; `PUT /api/articles/:slug` with `"tagList": null` is a 422, no `tagList` keeps the tags. | major | Updates parse with `json.parse` and walk the `json.Value.Object` by hand (`api.fields`, `required_text`, `optional_text`, `articles.tag_list`, ~60 lines); the typed record and decode's error messages are lost there | `repro/null_vs_missing.lang` |
| 5 | **`crypto.hash_password` / `verify_password` take ~200 ms** (PBKDF2-SHA256, 210000 rounds); OpenSSL does the same PBKDF2 in ~22 ms. Each login and registration burns 200 ms of CPU; 16 logins with 8 clients give 39 req/s. There's no bcrypt, scrypt or argon2 either. | major (performance) | Used `hash_password` as is (PBKDF2 at OWASP's round count is an acceptable password hash; only its speed is the problem) | `repro/pbkdf2_speed.lang` |
| 6 | **A `String?` can't be an SQL parameter**, though `lang doc sql` says parameters may be `none`. Saving an optional column (bio, image) fails to compile: ``expected `Value`, found `String?` ``. | minor | `api.nullable(text) -> sql.Value` (Null or String) | `repro/optional_sql_param.lang` |
| 7 | **SQL can't be shared between queries, not even a top-level `let` of a string literal.** The list, the feed and the single article need the same 15 columns and two `exists(...)` subqueries; filters (`?tag=&author=&favorited=`) need an optional WHERE clause. | minor (design) | An SQL view (`article_view`) for the shared part; one query where each filter is `(?2 = '' or v.author_username = ?2)`, numbered parameters reused. Works well, but only because SQLite has views and `?NNN`. | `repro/sql_constant.lang` |
| 8 | **Database errors have no type.** A UNIQUE violation is only the text `db: the statement failed: UNIQUE constraint failed: users.email (in: ...)`, so "username taken" can't be caught as such. | minor | Look up the username and email before inserting, inside `App.write` so no other request can take them in between | — |
| 9 | **`http.json` writes keys as the fields are named; there's no camelCase option.** Every RealWorld key is camelCase (`tagList`, `favoritesCount`, `createdAt`). `json.encode_camel` exists, but no response helper uses it. | minor | `api.reply(status, value)` = `http.bytes(status, data: json.encode_camel(value).bytes(), content_type: "application/json; charset=utf-8")` | — |
| 10 | **`time.DateTime` has no fraction of a second**: `iso()` gives `2026-09-29T12:00:00Z`. The Postman suite requires fractional seconds, and the hurl suite checks that `updatedAt` changes on an update made in the same second as the creation. | minor | `api.now()` builds `...T12:00:00.123Z` from `time.unix_millis()` by hand | — |
| 11 | **Leaving out one field when it is none isn't possible**; `omit_none` is for the whole value. A list shows articles without `body`, but `bio: null` and `image: null` must stay. | minor | Two records, `Article` and `Summary`, with 9 of their 10 fields the same, and two copy functions | — |
| 12 | **No mutex.** `Shared<T>` needs a value, so a plain lock is a `Shared<Int>` whose count nobody reads. | minor | `writes: Shared<Int>`, `count += 1` inside the lock | — |
| 13 | **Middleware can't hand anything to the handler** (no per-request values: the signed-in user). | minor | Each handler calls `auth.require(req, app)` / `auth.viewer(req, app)` itself | — |
| 14 | **A `{` block after `??` is read as a map literal**, and the error doesn't say so: ``expected `:` between a key and a value, found end of line``. | minor (error message) | `var secret = env.get(...) ?? ""` then `if secret.is_empty() { ... }` | `repro/block_after_coalesce.lang` |
| 15 | **`expect` works only directly in a `test` block**, not in a helper the tests call (`register(app, name)` checking the status). The error suggests `assert`, which is for bugs. | minor | The helper throws a `Failure` | — |
| 16 | No route groups or prefixes: `/api` is written in all 19 routes. | minor | written out | — |
| 17 | No way to trim given characters (`trim_matches("-")`) and no transliteration: "Привет, мир! Hello" slugs to `hello` (Go's gosimple/slug: `privet-mir-hello`). | minor | Two regexes (`[^a-z0-9]+` -> `-`, `^-+\|-+$` -> ``) | — |
| 18 | `jwt.verify` errors are `Failure`s with a message: an expired token can't be told from a forged one (to answer "token expired"). | minor | Both are 401 `token is invalid` | — |
| 19 | `lang add jwt --path ../../packages/jwt` says "1 package(s) pinned in lang.lock", but no `lang.lock` is written (path packages aren't pinned, which is fine, but the message says otherwise). | minor | — | — |
| 20 | `lang fmt --check` passes a file that doesn't parse (`repro/block_after_coalesce.lang`: exit 0, no message), so a broken file looks formatted. | minor | — | `repro/block_after_coalesce.lang` |

## Docs

- `lang doc cli` shows `output_dir: String?        // --output-dir out (optional)`,
  a `T?` field without `= none`; in a record built in code the same field
  is an error (``missing field(s) of `Out`: bio``) until `= none` is added.
  Decoding treats `T?` as optional, construction doesn't; the guide shows
  `= none` but never says the two differ.
- `lang doc String` lists internal methods: `__find(self, part, from)`,
  `__rfind(self, part)`.
- `lang doc db` doesn't say whether a connection may be shared by the
  requests of a server (it has to be, see #1 and #2), what happens to
  transactions then, or that a view or `?NNN` is the way to share SQL.
- `lang doc http.Router` doesn't say that the first matching route wins
  (#3).
- `lang doc sql` says parameters may be `none` (#6).
- `lang doc crypto.hash_password` doesn't say how long it takes.

## What worked well

- The first run of the full hurl suite passed 154/154 once the endpoints
  compiled; the type checker caught everything before that.
- The error middleware is 10 lines: a handler throws
  `ApiError(status: 404, field: "article", problem: "not found")` and
  `try next(req) catch err { if err is ApiError { ... } else { throw err } }`
  answers it.
- `query<T>` filling records by column name, `Bool` from SQLite's 0/1,
  numbered parameters, `json_group_array` into a `List<String>` via
  `json.decode`: no mapping code.
- The jwt package: sign and verify in one line each, `exp` checked.
- `http.cors`, `http.log_requests`, `router.handle(req)` for tests without a
  network, `cli.decode<Options>` for the flags.
- Error messages named the fix nearly every time (`some(x)` is only a
  pattern, write `x`; ``f` has 3 or more parameters: name every argument``).
