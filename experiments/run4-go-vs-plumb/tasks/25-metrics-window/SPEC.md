# metrics-window

An HTTP service that ingests metric samples and answers aggregates over a
sliding time window. Timestamps come from the clients.

## Running

```
PORT=8080 WINDOW_MS=60000 app
```

Listen on `127.0.0.1:$PORT`. All state is in memory. `WINDOW_MS` (default
60000) is a whole number ≥ 1. Many requests may arrive at the same time.

## Samples and the window

A sample is `{"name": N, "value": V, "ts": T}`: `name` a non-empty string
(any Unicode, compared exactly), `value` a JSON number (integer or
fractional, may be negative; handled as a 64-bit float), `ts` a JSON
integer ≥ 0, in milliseconds.

The service has one clock for all names, **now**: the largest `ts` of any
sample received so far (none before the first sample). Samples are processed
one at a time; for each sample, first set now = max(now, T); then, if
T ≤ now − `WINDOW_MS`, the sample is **late**: it is dropped and counted.
Otherwise it is accepted.

The **window** holds the accepted samples with now − `WINDOW_MS` < T ≤ now;
older samples leave it as now advances.

## Endpoints

- `POST /metrics` with one sample object or a JSON array of samples →
  `200 {"accepted": A, "dropped": D}` for this request. An array is
  processed in its order. If any sample in the request is invalid, the whole
  request is rejected with `400` and nothing is processed.
- `GET /aggregate?name=<name>` (URL-encoded) → `200` with the aggregates
  over the samples of that name in the window:
  `{"name": N, "count": C, "sum": S, "min": MIN, "max": MAX, "p50": P50, "p99": P99}`.
  With no samples, `count` and `sum` are `0` and the rest are `null`.
- `GET /stats` → `200 {"accepted": A, "dropped": D, "now": T}`: totals since
  the start (all names), and the clock (`null` before the first sample).

**Percentiles** use the nearest-rank method: sort the C values ascending;
the p-th percentile is the value at 1-based position ⌈p/100 × C⌉. For
values 1…10, p50 is 5 and p99 is 10; for values 1 and 2, p50 is 1.
Percentiles, `min` and `max` are always one of the values.

Every response body is JSON.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400`: the body is not a JSON object or array of objects; a sample's
  `name` is missing, not a string, or empty; `value` is missing or not a
  number (`"5"`, `true` and `null` are not numbers); `ts` is missing, not an
  integer (`1.5`, `"10"`) or negative. Also `GET /aggregate` without `name`.
- `404`: any other path.
- `405`: another method on `/metrics`, `/aggregate` or `/stats`.

## Example

```
$ WINDOW_MS=1000 app &
$ curl -s -XPOST localhost:8080/metrics -d '[{"name":"lat","value":12,"ts":1000},
    {"name":"lat","value":7,"ts":1200},{"name":"lat","value":30,"ts":1900}]'
{"accepted": 3, "dropped": 0}
$ curl -s 'localhost:8080/aggregate?name=lat'
{"name": "lat", "count": 3, "sum": 49, "min": 7, "max": 30, "p50": 12, "p99": 30}
$ curl -s -XPOST localhost:8080/metrics -d '{"name":"lat","value":1,"ts":2100}'
{"accepted": 1, "dropped": 0}
$ curl -s 'localhost:8080/aggregate?name=lat'
{"name": "lat", "count": 3, "sum": 38, "min": 1, "max": 30, "p50": 7, "p99": 30}
$ curl -s -XPOST localhost:8080/metrics -d '{"name":"lat","value":5,"ts":1100}'
{"accepted": 0, "dropped": 1}
```
