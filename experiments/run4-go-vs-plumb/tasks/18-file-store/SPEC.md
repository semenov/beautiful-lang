# file-store

An HTTP service that stores files in a directory.

## Running

```
PORT=8080 STORE_DIR=/data/files MAX_BYTES=1048576 app
```

Listen on `127.0.0.1:$PORT`. `STORE_DIR` is created if it doesn't exist.
`MAX_BYTES` is a whole number ≥ 0: the largest file size accepted. Many
requests may arrive at the same time.

Each file is kept as `STORE_DIR/<name>`, with exactly the uploaded bytes.
The store is whatever is in that directory: files placed there by other
means (with valid names) are listed and served too, so everything survives
a restart. The service may use names starting with `.` in `STORE_DIR` for
its own temporary files; those are never listed or served.

## Names

The `<name>` is everything in the path after `/files/` (so `/files/a/b`
asks for the name `a/b`), percent-decoded (`a%20b` is `a b`). The decoded
name must be 1 to 100 characters long, use only ASCII letters,
digits, `.`, `_` and `-`, and must not start with `.`. Anything else
(`..`, `.env`, `a/b`, `../x`, `..%2Fx`, `a b`, `ü.txt`, an empty name) gets
`400 {"error": "invalid name"}` and touches nothing on disk.

## Endpoints

- `PUT /files/<name>`: the request body (any bytes, given with a
  `Content-Length`) becomes the file's content. `201` if the file is new,
  `200` if it replaced an existing file. Response body:
  `{"name": "<name>", "size": <bytes>, "sha256": "<64 lowercase hex>"}`.
  A body larger than `MAX_BYTES` gets `413 {"error": "too large"}` and
  nothing is stored or changed (an existing file keeps its old content).
  A file of exactly `MAX_BYTES` bytes, and an empty file, are accepted.
- `GET /files/<name>`: `200` with the exact bytes and
  `Content-Type: application/octet-stream`.
- `DELETE /files/<name>`: `204`, empty body.
- `GET /files`: `200 {"files": [{"name": ..., "size": ..., "sha256": ...}, ...]}`,
  every stored file, ordered by name (ascending by byte value).

`GET` or `DELETE` of a name that is valid but not stored gets
`404 {"error": "not found"}`.

Readers never see a partly written file: while a `PUT` is in progress, a
`GET` of that name returns either the complete old content or the complete
new content (or `404` if there was no old file). When several `PUT`s to the
same name run at once, the file ends up with exactly one of their bodies.

## Other errors

Error responses have the body `{"error": "<message>"}`. A path other than
`/files` and `/files/<name>` gets `404 {"error": "not found"}`. Another
method gets `405 {"error": "method not allowed"}` (for example `POST /files/a`
or `DELETE /files`).

## Example

```
$ curl -s -XPUT --data-binary 'hello' localhost:8080/files/greeting.txt
{"name": "greeting.txt", "size": 5, "sha256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}
$ curl -s localhost:8080/files
{"files": [{"name": "greeting.txt", "size": 5, "sha256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}]}
$ curl -s -XPUT --data-binary 'x' localhost:8080/files/..%2Fetc
{"error": "invalid name"}
```
