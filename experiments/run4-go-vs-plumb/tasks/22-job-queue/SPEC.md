# job-queue

An HTTP service that runs submitted jobs in the background with a limited
number of workers.

## Running

```
PORT=8080 WORKERS=4 SHUTDOWN_GRACE_MS=5000 app
```

Listen on `127.0.0.1:$PORT`. All state is in memory. `WORKERS` (default 2)
is the maximum number of jobs running at the same time; `SHUTDOWN_GRACE_MS`
(default 5000) is explained under Shutdown. Many requests may arrive at the
same time.

## Jobs

A job is submitted as `{"ms": M, "input": "<text>"}`: `ms` is a whole number
from 0 to 60000, `input` a string (any Unicode). Running the job means:
wait M milliseconds, then produce the result: `input` reversed by Unicode
code point (`"héllo 👋"` → `"👋 olléh"`).

Jobs get ids 1, 2, 3, … in the order they are accepted, and start in that
order: a job starts as soon as a worker is free and every earlier queued
job has started. A job's status is `queued`, `running`, `done` or
`cancelled`.

## Endpoints

- `POST /jobs` with the job → `202 {"id": N}`.
- `GET /jobs/<id>` → `200 {"id": N, "status": S, "result": R}`; R is the
  result string when S is `done`, otherwise `null`.
- `POST /jobs/<id>/cancel` → `200 {"id": N, "status": "cancelled", "result": null}`
  for a `queued` or `running` job. A cancelled queued job never runs. A
  cancelled running job stops within 100 ms and frees its worker, so the
  next queued job starts within 100 ms of the cancel request.
- `GET /stats` → `200 {"queued": Q, "running": R, "done": D, "cancelled": C}`,
  the current number of jobs in each status. R never exceeds `WORKERS`.

Every response body is JSON.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400`: the body of `POST /jobs` is not a JSON object, or `ms` or `input`
  is missing or invalid (`ms` must be a JSON integer in range; `"100"` and
  `1.5` are invalid).
- `404`: no job with that id (including ids that are not positive integers),
  or any other path.
- `409`: cancelling a job that is `done` or already `cancelled`.

## Shutdown

On SIGTERM: stop accepting connections, never start a queued job, and let
the running jobs finish. As soon as no job is running, print one line to
standard output and exit with code 0:

```
shutdown: completed C, abandoned A
```

C is the number of jobs that were running at the signal and finished, A the
number that never finished (here: the queued ones). If running jobs are
still not finished `SHUTDOWN_GRACE_MS` after the signal, stop waiting: print
the same line (A now also counts the unfinished running jobs) and exit with
code 1.

## Example

```
$ curl -s -XPOST localhost:8080/jobs -d '{"ms": 500, "input": "abc"}'
{"id": 1}
$ curl -s localhost:8080/jobs/1
{"id": 1, "status": "running", "result": null}
$ curl -s localhost:8080/stats
{"queued": 0, "running": 1, "done": 0, "cancelled": 0}
$ sleep 1; curl -s localhost:8080/jobs/1
{"id": 1, "status": "done", "result": "cba"}
```
