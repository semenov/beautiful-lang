# Ports of open-source programs

Real programs ported to the language by agents that learned it only from
`lang help`, `lang guide` and `lang doc`. Each port found gaps in the
language, the library, the error messages or the docs; its `GAPS.md` lists
them, and `repro/` has small programs that show them. `TODO.md` tracks what
was fixed.

| port | original | what it exercises | result |
|---|---|---|---|
| [`hey`](hey) | [rakyll/hey](https://github.com/rakyll/hey), an HTTP load generator (Go, 1038 lines) | the HTTP client, concurrency, flags, timing | 730 lines; same numbers as the Go tool |
| [`httpbin`](httpbin) | [mccutchen/go-httpbin](https://github.com/mccutchen/go-httpbin), an HTTP testing service (Go, 4099 lines) | the HTTP server end to end: routing, streaming, cookies, auth, gzip, WebSockets | 2552 lines; 108 of 146 answers identical to Go's; 61k req/s vs Go's 66k, half the memory |
| [`gron`](gron) | [tomnomnom/gron](https://github.com/tomnomnom/gron), JSON to greppable lines and back (Go, 1800 lines) | JSON of any shape, text building, sorting, streams, speed | in progress |

`compiler/tests/run.sh` checks that every port still compiles (and runs the
httpbin tests), so they keep up with the language.

How to run them:

```
cd ports/hey && lang run hey.lang -n 1000 -c 50 http://localhost:8080/
cd ports/httpbin && lang run main.lang -- --port 8080     # lang test tests.lang: its tests
```
