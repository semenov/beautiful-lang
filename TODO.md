# TODO

Everything Vlad has asked for, in one place. Worked top to bottom; done
items stay (ticked) so nothing is lost. New requests are added as they come.

## Now

- [ ] **Postgres package** (a separate package, not stdlib): wire protocol v3
      over `net`, SCRAM-SHA-256 auth, parameters, typed rows, transactions,
      TLS. Installable with `lang add`.
- [ ] **Redis package** (separate package): RESP over `net`, commands,
      pipelining, pub/sub.
  - [ ] `lang add` of a package in a subdirectory of a repository (so both
        can live in this repo under `packages/`)
  - [ ] a way for packages to take query parameters without `db`'s
        compiler magic
- [ ] **zlib on streams**: gzip/gunzip a `Stream` piece by piece (big files,
      `content-encoding: gzip` responses)
- [ ] **Docs**: README for the new language, AGENTS.md (cheat sheet for
      agents), STDLIB.md (reference), DESIGN.md (stdlib decisions: why
      Postgres/Redis are packages, streams, static builds)

## Then: my own review of what's missing or weak

- [ ] Review the stdlib against Go and Node for backend/CLI gaps
- [ ] Review the language for rough edges found while writing the packages
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
