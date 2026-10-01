# caching-proxy

An HTTP proxy for GET requests to one upstream service, with an in-memory
cache.

## Running

```
PORT=8080 UPSTREAM=http://catalog.internal:9000/api CACHE_TTL_MS=30000 \
CACHE_MAX_ENTRIES=1000 UPSTREAM_TIMEOUT_MS=5000 app
```

Listen on `127.0.0.1:$PORT`. `UPSTREAM` is required: `http://host:port`,
optionally followed by a path prefix, without a trailing slash.
`CACHE_TTL_MS` (default 60000), `CACHE_MAX_ENTRIES` (default 1000) and
`UPSTREAM_TIMEOUT_MS` (default 5000) are whole numbers ≥ 1. Many requests
may arrive at the same time.

## Proxying

`GET <path>[?<query>]` is answered from the cache or by requesting
`GET UPSTREAM<path>[?<query>]`: the path and query exactly as received
(not decoded or re-encoded). `GET /items/a%20b?id=1` with
`UPSTREAM=http://h:9000/api` requests `http://h:9000/api/items/a%20b?id=1`.

The **cache key** is the path and query exactly as received; `?id=1` and
`?id=2` are different entries.

| Upstream answer | Proxy answer | Cached |
|---|---|---|
| `200` | `200`, the same body bytes and `Content-Type` | yes |
| any other status below 500 | the same status, body and `Content-Type` | no |
| a status 500–599 | `502 {"error": "upstream error", "status": <its status>}` | no |
| connection failed | `502 {"error": "upstream unavailable"}` | no |
| no complete answer within `UPSTREAM_TIMEOUT_MS` | `504 {"error": "upstream timeout"}` | no |

Every answer to a GET has the header `X-Cache: MISS` if this request caused
the upstream request, and `X-Cache: HIT` otherwise.

## Cache rules

- **TTL:** an entry is fresh for `CACHE_TTL_MS` after its upstream answer
  arrived. A fresh entry is served without asking the upstream; a stale one
  is fetched again.
- **Size:** at most `CACHE_MAX_ENTRIES` entries. When a new entry would
  exceed it, remove the least recently used entry first. Storing an entry and
  serving it from the cache both count as a use.
- **Coalescing:** while an upstream request for a key is in progress, other
  requests for the same key do not start their own: they wait for it and get
  the same answer (status, body and `Content-Type`; errors too, with
  `X-Cache: HIT`). N parallel requests for an uncached key make exactly one
  upstream request.

## Errors

- `405 {"error": "method not allowed"}`: any method other than GET; nothing
  is sent upstream.

## Example

```
$ curl -si localhost:8080/items?id=7
HTTP/1.1 200 OK
Content-Type: application/json
X-Cache: MISS

{"id": 7, "name": "lamp"}
$ curl -si localhost:8080/items?id=7 | grep X-Cache
X-Cache: HIT
$ curl -s localhost:8080/broken
{"error": "upstream error", "status": 503}
```
