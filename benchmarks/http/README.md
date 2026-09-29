# HTTP server: Plumb vs Go net/http vs fasthttp

`load.go` is a small keep-alive load generator; `cpu.sh` measures the CPU
time a server spends per request (more telling than requests per second
when the load generator runs on the same machine). Servers answer
`GET /hello` with "hello": `hello.plumb`, `gostd/`, `fast/`.

```
go build -o /tmp/hb-load load.go
plumb build hello.plumb -o /tmp/hb-plumb && /tmp/hb-plumb &
./cpu.sh $(pgrep -f /tmp/hb-plumb) http://127.0.0.1:8200/hello 64
```

## Results (Apple M-series, 2026-09-29, 64 connections)

| server | req/s | CPU per request | memory |
|---|---|---|---|
| Plumb | 133k | 45 us | 9 MB |
| Plumb, after the scheduler rewrite (2026-09-29) | 145k | 37 us | 9 MB |
| Go net/http | 133k | 42 us | 25 MB |
| fasthttp | 138k | 39 us | 16 MB |

Requests per second are the same for all three: the load generator on the
same machine is the limit. CPU per request is within 15%.

## What makes fasthttp faster than net/http, and what applies to us

fasthttp's gains over net/http come from not allocating per request:
pooled request/response objects (`sync.Pool`), reused buffers, headers
kept as byte slices into the read buffer instead of a `map[string][]string`
of new strings, lazy parsing (the URI and arguments only when asked), no
`context`/`http.Header` allocations, a worker pool instead of growing new
goroutine stacks, and responses written with one buffered write. All of
this is about Go's garbage collector: fewer allocations, fewer GC cycles.

We have no garbage collector, and a profile of our server under load
(`sample` on macOS) shows the user-space request work is already a tiny
share of the time (5 of ~7,000 samples in `lt_http_conn`). What we already
do in the fasthttp spirit: the head and small bodies go out in one write,
files go out with `sendfile`, connection stacks are reused.

The time goes to the kernel (loopback TCP) and to handing a ready socket
from the poller thread to a worker: a kqueue/epoll re-registration per
wait (`EV_ONESHOT`), a condition-variable wake-up, and the global run
queue's mutex. A first attempt (spinlock run queue, readying in batches)
changed nothing measurable (43.5 us before and after) and was reverted.

What would help, from Go's runtime rather than fasthttp: register each
socket once, edge-triggered, instead of on every wait; let idle workers
poll for I/O themselves instead of a separate poller thread (no hand-off);
per-worker run queues with stealing. These are in TODO.md.
