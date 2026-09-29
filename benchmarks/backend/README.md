# A backend like a real app's, in Lang and in Go

A small notes service: users, token login (HMAC), notes per user in SQLite
(WAL), JSON in and out, validation, an auth check in every handler, and a
log line per request. `lang/server.lang` (158 lines) and `go/main.go`
(net/http, database/sql with mattn/go-sqlite3 (the same C SQLite),
encoding/json, log/slog; 250 lines) have the same API.

`load/` sets up users and notes, then sends a mix over keep-alive
connections: 70% "list my 20 newest notes", 20% "get one note", 10% "create
a note". `./bench.sh [connections] [seconds]` runs both on fresh databases
and prints requests per second, latency, the server's CPU time per request
and its peak memory.

## Results (Apple M-series, 2026-09-29, 64 connections, 10 s)

```
lang: 28704 req/s, p50 2.045ms, p99 4.863ms, 0 failed of 287042; CPU 90.1 us per request; peak 10 MB
go: 17126 req/s, p50 2.408ms, p99 21.983ms, 0 failed of 171263; CPU 343.6 us per request; peak 44 MB
```

The load generator runs on the same machine, so requests per second are
limited by it too; CPU per request and memory are the better comparison.

## What this found in Lang

- **A bug:** a query with a bound `limit ?` failed with "the query result
  has no column". Column names were read before the first step, and SQLite
  may prepare the statement again on that step, freeing them.
- **A trap:** `conn.last_id()` after `execute` is racy when tasks share a
  connection (another insert can come in between). Now `conn.insert(...)`
  returns the id.
- **Still to do:** one connection serves every request (SQLite serializes
  them). Go's database/sql keeps a pool; a pool would let reads run in
  parallel under WAL (see TODO.md).
