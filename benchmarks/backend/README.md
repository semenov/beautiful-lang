# A backend like a real app's, in Plumb and in Go

A small notes service: users, token login (HMAC), notes per user in SQLite
(WAL), JSON in and out, validation, an auth check in every handler, and a
log line per request. `plumb/server.plumb` (167 lines) and `go/main.go`
(net/http, database/sql with mattn/go-sqlite3 (the same C SQLite),
encoding/json, log/slog; 251 lines) have the same API.

`load/` sets up users and notes, then sends a mix over keep-alive
connections: 70% "list my 20 newest notes", 20% "get one note", 10% "create
a note". `./bench.sh [connections] [seconds]` runs both on fresh databases
and prints requests per second, latency, the server's CPU time per request
and its peak memory.

## Results (Apple M-series, 64 connections, 10 s)

```
2026-09-30, with the connection pool (reads in parallel):
plumb: 44219 req/s, p50 899µs, p99 8.348ms, 0 failed of 442190; CPU 80.1 us per request; peak 15 MB
go: 16123 req/s, p50 2.574ms, p99 22.082ms, 0 failed of 161232; CPU 350.1 us per request; peak 46 MB

2026-09-29, one SQLite connection:
Plumb: 28704 req/s, p50 2.045ms, p99 4.863ms, 0 failed of 287042; CPU 90.1 us per request; peak 10 MB
go: 17126 req/s, p50 2.408ms, p99 21.983ms, 0 failed of 171263; CPU 343.6 us per request; peak 44 MB
```

The load generator runs on the same machine, so requests per second are
limited by it too; CPU per request and memory are the better comparison.

## One CPU in Docker (2026-09-29)

`./docker.sh [connections] [seconds] [quota|pin]` runs each server in a
container limited to one CPU (`--cpus=1`, a quota as Kubernetes CPU limits
use, or `--cpuset-cpus`, one pinned CPU), with the load from another
container on other CPUs. Apple M-series, Docker's Linux VM, 64 connections,
10 s:

```
pinned to one CPU
plumb:    21944 req/s, p50 2.864ms, p99 6.353ms; CPU 44.7 us per request; 13 MB
go:       20431 req/s, p50 3.086ms, p99 7.41ms;  CPU 48.2 us per request; 26 MB

a quota of one CPU (--cpus=1)
plumb:    21661 req/s, p50 2.742ms, p99 11.4ms;  CPU 46.1 us per request; 13 MB
go:        1620 req/s, p50 5.795ms, p99 204ms;   CPU 622 us per request;  42 MB, 56 threads
go-pool4:  6048 req/s, p50 3.633ms, p99 78.9ms;  CPU 165 us per request;  47 MB
go, GOMAXPROCS=1, pool of 4: 20390 req/s, p99 7.4ms
```

Tuned, the two are close: Plumb ~7% more requests, about the same CPU per
request, half the memory (and Go runs SQLite with `synchronous=NORMAL`,
Plumb with the safer default). With default settings under a quota, Go
runs more threads than the quota pays for, and database/sql opens a
connection per waiting request, so writers pile up in SQLite's busy
waits; setting GOMAXPROCS=1 and a pool size fixes it.

What it found in Plumb: the worker count came from the online CPUs (14 in
the VM), ignoring the container. Now it takes the CPUs the process may use
(affinity) and the cgroup quota, rounded up; `PLUMB_WORKERS` overrides it.
Under the quota that took Plumb from 13818 to 21661 req/s and its p99 from
40 ms to 11 ms (19 threads to 4).

## What this found in Plumb

- **A bug:** a query with a bound `limit ?` failed with "the query result
  has no column". Column names were read before the first step, and SQLite
  may prepare the statement again on that step, freeing them.
- **A trap:** `conn.last_id()` after `execute` is racy when tasks share a
  connection (another insert can come in between). Now `conn.insert(...)`
  returns the id.
- **Still to do:** one connection serves every request (SQLite serializes
  them). Go's database/sql keeps a pool; a pool would let reads run in
  parallel under WAL (see TODO.md).
