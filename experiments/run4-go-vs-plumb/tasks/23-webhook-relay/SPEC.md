# webhook-relay

An HTTP service that accepts events and delivers each one to a target URL,
signed, with retries.

## Running

```
PORT=8080 TARGET_URL=http://hooks.internal:9000/in SECRET=s3cret \
MAX_ATTEMPTS=5 BACKOFF_MS=1000 ATTEMPT_TIMEOUT_MS=5000 app
```

Listen on `127.0.0.1:$PORT`. All state is in memory. `TARGET_URL` and
`SECRET` are required; `MAX_ATTEMPTS` (default 5), `BACKOFF_MS` (default
1000) and `ATTEMPT_TIMEOUT_MS` (default 5000) are whole numbers ≥ 1. Many
requests may arrive at the same time.

## Accepting events

`POST /events` with a JSON object that has a `source` (a non-empty string)
and a `payload` (any JSON value); other fields are ignored. The answer is
`202 {"id": N}`; ids are 1, 2, 3, … in the order events are accepted.

## Delivering

Each event is delivered with `POST TARGET_URL` and:

- the body: exactly the bytes of the accepted `POST /events` body;
- `Content-Type: application/json`;
- `X-Event-Id: N`;
- `X-Signature: sha256=<hex>`: the HMAC-SHA256 of the body, keyed with the
  UTF-8 bytes of `SECRET`, as 64 lowercase hex digits.

An attempt succeeds if the target answers with a 2xx status within
`ATTEMPT_TIMEOUT_MS`. Any other status, a connection error, or no complete
answer in time is a failed attempt. After failed attempt k, wait
`BACKOFF_MS × 2^(k−1)` milliseconds (with `BACKOFF_MS=100`: 100, 200, 400, …)
and try again, up to `MAX_ATTEMPTS` attempts in total; then the event has
failed for good.

**Order:** events with the same `source` are delivered one at a time in the
order they were accepted: an event's first attempt starts only after the
previous event of that source is delivered or has failed for good. Events of
different sources do not wait for each other: a source whose target keeps
timing out must not delay any other source. When nothing is ahead of an
event, its first attempt starts within 100 ms of accepting it.

## Status

`GET /events/<id>` → `200 {"id": N, "source": S, "status": T, "attempts": A}`,
where T is `pending` (not yet delivered or failed for good), `delivered` or
`failed`, and A is the number of attempts started so far.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400`: the body is not a JSON object, `source` is missing, not a string,
  or empty, or `payload` is missing. The event is not accepted.
- `404`: no event with that id, or any other path.

## Example

```
$ curl -s -XPOST localhost:8080/events -d '{"source":"billing","payload":{"amount":5}}'
{"id": 1}
# the target receives POST /in with that exact body,
# X-Event-Id: 1 and X-Signature: sha256=<hmac of the body>
$ curl -s localhost:8080/events/1
{"id": 1, "source": "billing", "status": "delivered", "attempts": 1}
```
