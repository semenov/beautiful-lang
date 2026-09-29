# Gaps found while porting go-httpbin to Plumb

Severity: **blocker** (can't be done), **major** (needs a big workaround, or
wrong behavior for real clients), **minor** (awkward, or a small divergence).
Repros are in `repro/`. The endpoint-by-endpoint comparison is in
`compare/` (`compare.sh`, `cases.txt`, `result.txt`).

## Standard library: HTTP server

| # | Gap | Sev. | What I did instead | Repro |
|---|-----|------|--------------------|-------|
| 1 | **No client address on `http.Request`.** `/ip`, the `origin` field of every echo response and the request log need `r.RemoteAddr`. `io.Stream.peer()` exists, but the request's connection isn't exposed. | major | Used `Fly-Client-IP`/`X-Forwarded-For`/... headers, else `""` | `repro/no_remote_addr.plumb` |
| 2 | **Router has only `get/post/put/delete`.** No `patch`, `head`, `options`, no "any method" route (Go: `mux.HandleFunc("/anything", ...)`), no custom methods, no subtree patterns (`/anything/`). | major | Wrote my own dispatcher (Go-style `"GET /x/{p}"` patterns, 404/405 + `Allow`) in one middleware that never calls `next`. It works because middleware runs even for unmatched paths. | — |
| 3 | **Query is `Map<String, String>`, the last value wins, and the raw query isn't available.** `?a=1&a=2` echoes `["2"]`; the echoed `url` must be rebuilt with `url.query_text` (`q=a+b` comes back as `q=a%20b`). | major | Rebuilt the query from the map | `repro/query_multi.plumb` |
| 4 | **`+` in the path is decoded as a space.** Route `/c+d` never matches, `:param` of `a+b` is `"a b"`. `+` means a space only in query strings. | major (bug) | `/base64/{data}`: put the `+` back by replacing spaces | `repro/plus_in_path.plumb` |
| 5 | **Chunked request bodies are silently dropped** (`Transfer-Encoding: chunked`, `curl -T -`): `req.body` is empty. | major (bug) | nothing possible | `repro/chunked_body.plumb` |
| 6 | **Response header names are case-sensitive map keys.** `http.text(...).with_header("Content-Type", "text/html")` sends *two* content types. | major (bug) | Always lower-case names | `repro/header_case.plumb` |
| 7 | **Cookie values are written unvalidated**: `Cookie(value: "x; Domain=evil.example")` injects attributes (Go sanitizes). | major (security) | `sanitize_cookie_value` by hand | `repro/cookie_injection.plumb` |
| 8 | Path is given decoded, raw path lost: `/a%2Fb` and `/a/b` are the same; echoed URLs must be re-escaped by hand. | minor | `escape_path` | — |
| 9 | Headers are `Map<String, String>`, repeated headers joined with `", "`; original name case lost. Can't tell `X-Foo: 1`+`X-Foo: 2` from `X-Foo: 1, 2`. | minor | Canonicalize names by hand, one value per name | — |
| 10 | One value per response header (`with_header` replaces). `/response-headers?a=1&a=2` can't send both. Only cookies are special-cased. | minor | last value wins | — |
| 11 | A content type is always added: empty-body responses (302, 304, OPTIONS, `/bytes/0`) get `content-type: text/plain; charset=utf-8`; `content-type: ""` is sent as an empty header. | minor | none | `repro/status_204.plumb` |
| 12 | No reason phrase for many codes: `HTTP/1.1 418 `, same for 206, 300, 307, 406, 412, 415, 416, 501, 100. No `http.status_text(code)` either. | minor | wrote a 60-line status-text table for JSON errors | `repro/status_204.plumb` |
| 13 | A 204 advertises `content-length` of the body it (rightly) doesn't send. | minor (bug) | send empty bodies | `repro/status_204.plumb` |
| 14 | 1xx codes are sent as a final response (`/status/100`); Go sends `100 Continue` and then a `200`. | minor | none | — |
| 15 | Streams are always chunked; a `content-length` header on `http.stream` is dropped silently. `/drip` in Go sends a fixed-length body slowly. | minor | chunked drip | — |
| 16 | No trailers. `/trailers` and SSE's `Server-Timing` trailer can't be ported. | minor | `/trailers` answers like `/anything` without trailers | — |
| 17 | `http.Cookie` has no Domain or Expires, always writes `SameSite=` (with `""`: `SameSite=;`), and a fixed attribute order. `attr[Domain]=` in `/cookies/set` is ignored. | minor | `max_age: 0` for delete | `repro/cookie_injection.plumb` |
| 18 | `http.request("GET", "/x?a=1")` (the test helper) doesn't split off the query: `path` keeps `?a=1`, `query_params` stays empty. | minor (bug) | my own `new_request` in tests | `repro/test_request_query.plumb` |
| 19 | No server options: bind address, max body size (a 5 MB body is read into memory before the handler can refuse it), timeouts, `Date` header. | minor | check `req.body.length` in handlers | — |
| 20 | No way to see the client going away during `time.sleep` (Go: `r.Context().Done()` → 499). The writer of a stream does fail on the next write, which is enough for `/drip`. | minor | none | — |

What worked well: middleware runs for unmatched paths and before the
WebSocket upgrade (so `/websocket/echo` can validate and answer 400 first);
`router.websocket` echo was 6 lines; `http.stream` + lambdas made `/stream`,
`/sse`, `/jsonl`, `/drip` short; `router.handle(req)` made network-free tests
easy; HEAD bodies are dropped automatically.

## Standard library: other

| # | Gap | Sev. | What I did instead |
|---|-----|------|--------------------|
| 21 | **No MD5** (digest auth). | major | MD5 in Plumb, ~90 lines (`digest.plumb`) |
| 22 | No seeded random generator; `random` is only the OS generator. | minor | xorshift (`helpers.Rng`); seeded `/bytes?seed=` can't match Go's sequence anyway |
| 23 | No wall-clock milliseconds: `time.unix_now()` is whole seconds, `time.now().nanos` is monotonic. SSE `timestamp` is in ms. | minor | seconds×1000 + monotonic offset (so every stream starts at `…000`) |
| 24 | `url.parse` rejects relative URLs (`"/get"`: "has no scheme"). | minor | only parse when `://` is present |
| 25 | *Fixed: `json.encode_with(x, options: json.EncodeOptions(keys: json.Keys.Kebab, omit_none: true))`.* JSON: no `omitempty`, no field renaming (`"user-agent"` can't be a field name). | minor | two record types per optional field; `json.Value.Object` for `user-agent` |
| 26 | JSON numbers in `json.Value` are `Float`: `12345678901234567890` is echoed as `1.2345678901234567e+19` (Go: `12345678901234567000`). | minor | none |
| 27 | No lossy UTF-8 decoding (Go's `string(bytes)` + U+FFFD on encode). A non-UTF-8 body gives `data: ""`. | minor | `""` |
| 28 | No listing of environment variables (`env.get` only): `/env` can't collect `HTTPBIN_*`. | minor | `/env` returns an empty map |
| 29 | No embedding of files in the binary. | minor | `static/` read at start-up (`--static-dir`) |
| 30 | `time.Duration` has no comparison operators or division: `min.is_longer_than(d)` for `d < min`, `time.nanos(d.nanos.div(n))`. | minor | as written |

## Language

| # | Gap | Sev. | What I did instead |
|---|-----|------|--------------------|
| 31 | No hex literals (`0x67452301` is a parse error). | minor | decimal constants; MD5's K table computed with `math.sin` |
| 32 | No `+` on strings and no string builder. | minor | `out = "${out}${c}"` (quadratic) or a list + `join` |
| 33 | Integer `*` overflow panics; no wrapping multiply. PRNGs and hashes need it. | minor | shift/xor-only generator |
| 34 | No `\u{...}` escape in string literals. | minor | — |
| 35 | Lambda parameters can't have type annotations, so a local helper function (`let render = (name: String) => ...`) isn't possible. | minor | top-level function |
| 36 | `if a is some(x) or b is some(y)` is rejected ("names bound by `is` can only be used in an `if` condition" — it *is* in an `if`). | minor | `not (a is none) or ...` |
| 37 | `type Handler = fn(...)` is a new type, so a function must be wrapped (`Handler(f)`) — yet `http.Middleware` is declared the same way and lambdas are passed to `router.use` directly. | minor | spelled out the `fn(...)` type in the record field |
| 38 | A stored function can't be called as `r.handler(x)`. | minor | `let h = r.handler` then `h(x)` |

## Error messages

- Good overall: every error named the fix (`use interpolation`, `name every
  argument after the first`, `let f = x.handler then f(...)`).
- Misleading: calling a stored function field suggests "or name the function
  type: `type Rule = fn(...)`", but a named fn type then needs explicit
  wrapping at every use (#37).
- `names bound by is can only be used in an if condition` fires *inside* an
  `if` condition when combined with `or` (#36); it should say "with `or`".
- `plumb check` on a file importing a module with a type error reports it in
  that module (fine), but test failures inside loops don't say which
  iteration failed; I collected failure lists by hand (`check_statuses`).

## Docs

- `plumb doc http.bytes` example `http.bytes(200, png, content_type: "image/png")`
  doesn't compile (3 parameters: every argument after the first must be
  named: `data: png`).
- The http docs don't mention: that queries keep only the last value, that
  paths are decoded (and `+` becomes a space), that a content type is always
  added, what happens to `content-length` on a stream, or how routes and
  middleware order interact (a middleware that doesn't call `next` hides every
  middleware added after it — `log_requests` must be added first).
- `time.now()` doesn't say it's monotonic (not wall time).
