# kv-counter

An in-memory HTTP key-value store with atomic counters and compare-and-set.

## Running

```
PORT=8080 app
```

Listen on `127.0.0.1:$PORT`. All data is in memory. Many requests may arrive
at the same time, also for the same key; every operation on a key is atomic.

## Keys and values

- A **key** is the rest of the path after `/kv/`, `/incr/` or `/cas/`,
  URL-decoded as UTF-8 (`/kv/caf%C3%A9%20x` is the key `café x`). It must be
  non-empty. Keys are compared exactly. The query string is not part of the
  key.
- A **value** is a JSON string (any Unicode, stored and returned exactly) or
  a JSON integer in the signed 64-bit range (−9223372036854775808 to
  9223372036854775807). Integers stay integers: `5` and `"5"` are different
  values. Any other JSON (fractions such as `1.5`, `true`, `null`, arrays,
  objects, integers out of range) is not a valid value.

Every response body is JSON.

## Endpoints

| Request | Success |
|---|---|
| `GET /kv/<key>` | `200 {"key": K, "value": V}` |
| `PUT /kv/<key>` with `{"value": V}` | `200 {"key": K, "value": V}` (creates or replaces) |
| `DELETE /kv/<key>` | `204`, empty body |
| `POST /incr/<key>?by=N` | `200 {"key": K, "value": NEW}` |
| `POST /cas/<key>` with `{"expected": E, "value": V}` | `200 {"key": K, "value": V}` |
| `GET /snapshot` | `200 {"count": C, "items": {K: V, ...}}` |

- `incr` adds N to the integer stored at the key and returns the new value.
  `by` is an integer in the 64-bit range, may be negative, default `1`. A
  missing key counts as `0` (it is created).
- `cas` stores V only if the current value equals E (same type and value);
  `"expected": null` means "the key must not exist". The request body must
  contain both fields.
- `snapshot` returns every key and its value at one moment; C is the
  number of keys.

## Errors

Error responses have the body `{"error": "<message>"}`, and change nothing.

- `400`: the body is not a JSON object, `value` (or `expected`) is missing or
  not a valid value (`expected` may also be `null`), `by` is not an integer in
  the 64-bit range, or the key is empty.
- `404 {"error": "not found"}`: `GET` or `DELETE` of a key that does not
  exist; any path other than the ones above.
- `405 {"error": "method not allowed"}`: another method on a path above.
- `409`: `incr` on a key whose value is not an integer (strings, including
  `"5"`), or whose result would leave the 64-bit range: the body is
  `{"error": "<message>", "value": CURRENT}`.
- `409`: `cas` when the current value does not equal E: the body is
  `{"error": "<message>", "value": CURRENT}`, CURRENT being `null` if the key
  does not exist.

## Example

```
$ curl -s -XPUT localhost:8080/kv/greeting -d '{"value":"hi"}'
{"key": "greeting", "value": "hi"}
$ curl -s -XPOST 'localhost:8080/incr/visits?by=5'
{"key": "visits", "value": 5}
$ curl -s -XPOST localhost:8080/cas/visits -d '{"expected":5,"value":0}'
{"key": "visits", "value": 0}
$ curl -s -XPOST localhost:8080/incr/greeting
{"error": "value is not an integer", "value": "hi"}
$ curl -s localhost:8080/snapshot
{"count": 2, "items": {"greeting": "hi", "visits": 0}}
```
