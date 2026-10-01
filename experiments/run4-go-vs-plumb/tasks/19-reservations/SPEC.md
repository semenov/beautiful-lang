# reservations

An HTTP JSON service for reserving seats at events, stored in SQLite.

## Running

```
PORT=8080 DB_PATH=/data/reservations.db app
```

Listen on `127.0.0.1:$PORT`. Store everything in the SQLite database at
`DB_PATH`; create the file and its tables if they don't exist. Data
(including idempotency keys) survives a restart. Many requests may arrive at
the same time; the seat counts must stay exact under any mix of concurrent
requests.

## Objects

- Event: `{"id": 1, "name": "Concert", "capacity": 100, "remaining": 97}`.
  `remaining` is `capacity` minus the seats of all active reservations, and
  is never negative.
- Reservation: `{"id": 1, "event_id": 1, "seats": 3, "status": "active"}`;
  `status` is `active` or `cancelled`.

Ids are positive integers chosen by the server. Strings are stored and
returned exactly as given.

## Endpoints

Request bodies are JSON objects; every response body is JSON.

- `POST /events` `{"name": "Concert", "capacity": 100}` → `201`, the event.
  `name`: a non-empty string. `capacity`: an integer 1 to 100000.
- `GET /events/<id>` → `200`, the event.
- `POST /events/<id>/reservations` `{"seats": 3}` → `201`, the new
  reservation. `seats`: an integer ≥ 1. All the seats are reserved or none:
  if fewer than `seats` remain, respond `409 {"error": "not enough seats"}`.
- `GET /reservations/<id>` → `200`, the reservation.
- `DELETE /reservations/<id>` → `200`, the reservation with status
  `cancelled`; its seats become available again.

### Idempotency

`POST /events/<id>/reservations` may carry an `Idempotency-Key` header (any
non-empty string). Keys are global, not per event. If a reservation was
already created with that key, respond `201` with that reservation (in its
current state) and reserve nothing more, as long as the event id and `seats`
are the same as in the first request; otherwise respond
`422 {"error": "idempotency key reused"}`. A request that failed (any
status other than 201) does not record its key. When several requests with
the same new key arrive at once, exactly one reservation is created and all
of them get it.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400 {"error": "invalid JSON"}`: the body is not a JSON object.
- `400 {"error": "invalid name"}`, `400 {"error": "invalid capacity"}`,
  `400 {"error": "invalid seats"}`: a missing field or a value of the wrong
  type or range (`"3"`, `2.5`, `0`, `null` are all invalid integers).
- `404 {"error": "not found"}`: unknown event or reservation, an id that is
  not a positive integer, or any other path.
- `405 {"error": "method not allowed"}`: another method on a known path.
- `409 {"error": "not enough seats"}`: see above.
- `409 {"error": "already cancelled"}`: cancelling a cancelled reservation.

Field checks (400) come before looking up the event (404).

## Example

```
$ curl -s -XPOST localhost:8080/events -d '{"name":"Concert","capacity":5}'
{"id": 1, "name": "Concert", "capacity": 5, "remaining": 5}
$ curl -s -XPOST localhost:8080/events/1/reservations -H 'Idempotency-Key: k-1' -d '{"seats":3}'
{"id": 1, "event_id": 1, "seats": 3, "status": "active"}
$ curl -s -XPOST localhost:8080/events/1/reservations -H 'Idempotency-Key: k-1' -d '{"seats":3}'
{"id": 1, "event_id": 1, "seats": 3, "status": "active"}
$ curl -s -XPOST localhost:8080/events/1/reservations -d '{"seats":3}'
{"error": "not enough seats"}
$ curl -s -XDELETE localhost:8080/reservations/1
{"id": 1, "event_id": 1, "seats": 3, "status": "cancelled"}
$ curl -s localhost:8080/events/1
{"id": 1, "name": "Concert", "capacity": 5, "remaining": 5}
```
