# rate-limiter

An HTTP service that decides whether a client with a given API key may make
a request now, using a token bucket per key.

## Running

```
PORT=8080 CAPACITY=10 REFILL_PER_SEC=2.5 app
```

Listen on `127.0.0.1:$PORT`. All state is in memory. Many requests may
arrive at the same time, also for the same key.

- `CAPACITY`: a whole number ≥ 1, default `10`.
- `REFILL_PER_SEC`: a decimal number > 0 (may be fractional, such as `0.5`),
  default `1`.

If either variable is set to an invalid value, print a message starting with
`error:` to standard error and exit with code 2 without listening.

## Token buckets

Every key has its own bucket. A key's bucket is created full (`CAPACITY`
tokens) the first time the key is checked. Tokens are added continuously at
`REFILL_PER_SEC` per second (fractions of a token accumulate), never above
`CAPACITY`. Keys are compared exactly (case-sensitive, byte for byte); keys
are independent of each other.

## Endpoints

`POST /check` with a JSON object `{"key": "<api key>"}`. `key` must be a
non-empty string (any Unicode). Other fields are ignored.

- If the bucket holds at least one whole token, take one token and answer
  `200 {"allowed": true, "remaining": R}`, where R is the number of whole
  tokens left after taking one (rounded down).
- Otherwise answer `429 {"allowed": false, "remaining": 0, "retry_after": S}`
  with the header `Retry-After: S`, where S is the number of seconds until the
  bucket will hold one whole token, rounded up to a whole number, at least 1.
  A denied check takes no token.

Taking a token is atomic: when N checks for one key arrive at the same time
and the bucket holds T whole tokens (and nothing refills meanwhile), exactly
min(N, T) of them are allowed.

`GET /stats` answers `200 {"allowed": A, "denied": D, "keys": K}`: the
total numbers of allowed and denied checks since the start, over all keys,
and the number of distinct keys checked. Every answered `/check` with status
200 or 429 is counted exactly once; rejected requests (400) are not counted.

Every response body is JSON.

## Errors

- `400 {"error": "<message>"}`: the body is not a JSON object, or `key` is
  missing, not a string, or empty.
- `404 {"error": "not found"}`: any other path.
- `405 {"error": "method not allowed"}`: a method other than `POST` on
  `/check`, or other than `GET` on `/stats`.

## Example

```
$ CAPACITY=2 REFILL_PER_SEC=0.5 app &
$ curl -s -XPOST localhost:8080/check -d '{"key":"k1"}'
{"allowed": true, "remaining": 1}
$ curl -s -XPOST localhost:8080/check -d '{"key":"k1"}'
{"allowed": true, "remaining": 0}
$ curl -si -XPOST localhost:8080/check -d '{"key":"k1"}'
HTTP/1.1 429 Too Many Requests
Retry-After: 2

{"allowed": false, "remaining": 0, "retry_after": 2}
$ curl -s localhost:8080/stats
{"allowed": 2, "denied": 1, "keys": 1}
```
