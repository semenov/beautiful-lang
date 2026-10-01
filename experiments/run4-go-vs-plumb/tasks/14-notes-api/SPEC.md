# notes-api

An HTTP JSON API for short notes, stored in SQLite.

## Running

```
PORT=8080 DB_PATH=/data/notes.db app
```

Listen on `127.0.0.1:$PORT`. Store all data in the SQLite database at
`DB_PATH`; create the file and its tables if they don't exist. Data must
survive a restart of the program. Many requests may arrive at the same time.

## Notes

A note is `{"id": 1, "title": "Shopping", "body": "milk", "tag": "home"}`.

- `id`: a positive integer chosen by the server, unique, never reused.
- `title`: a string of 1 to 200 characters (Unicode code points).
- `body`: a string, may be empty.
- `tag`: a string or `null`.

All strings are stored and returned exactly as given (any Unicode, quotes,
`%`, SQL-like text). Every response with a body is JSON with
`Content-Type: application/json`.

## Endpoints

| Request | Success |
|---|---|
| `POST /notes` | `201`, the created note |
| `GET /notes` | `200`, `{"notes": [...]}` |
| `GET /notes/<id>` | `200`, the note |
| `PATCH /notes/<id>` | `200`, the updated note |
| `DELETE /notes/<id>` | `204`, empty body |

- `POST /notes` takes a JSON object. `title` is required; `body` defaults to
  `""`, `tag` defaults to `null`. Other fields are ignored.
- `PATCH /notes/<id>` takes a JSON object with any of `title`, `body`, `tag`.
  A field that is left out keeps its value. `"tag": null` clears the tag.
  `title` and `body` may not be `null`. Other fields are ignored.
- `GET /notes` returns all notes, ordered by `id` ascending. With `?q=TEXT`
  it returns only the notes whose `title` or `body` contains TEXT as a
  literal, case-sensitive substring. TEXT is URL-decoded (`%25` is `%`, `+`
  is a space); every character in it, including `%`, `_`, `\` and quotes,
  matches only itself. An empty `q` matches every note.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400 {"error": "invalid JSON"}`: the request body of POST or PATCH is not a
  JSON object.
- `400 {"error": "invalid field: <name>"}`: a field has the wrong type or
  value, or `title` is missing in POST (for example
  `invalid field: title`). Nothing is changed.
- `404 {"error": "not found"}`: no note with that id, an `<id>` that is not
  a positive integer, or any path other than `/notes` and `/notes/<id>`.
- `405 {"error": "method not allowed"}`: a method not listed above on
  `/notes` or `/notes/<id>` (for example `PUT /notes/1` or `DELETE /notes`).

## Example

```
$ curl -s -XPOST localhost:8080/notes -d '{"title":"Shopping","body":"milk"}'
{"id": 1, "title": "Shopping", "body": "milk", "tag": null}
$ curl -s -XPATCH localhost:8080/notes/1 -d '{"tag":"home"}'
{"id": 1, "title": "Shopping", "body": "milk", "tag": "home"}
$ curl -s 'localhost:8080/notes?q=mil'
{"notes": [{"id": 1, "title": "Shopping", "body": "milk", "tag": "home"}]}
$ curl -s -o /dev/null -w '%{http_code}\n' -XDELETE localhost:8080/notes/1
204
$ curl -s localhost:8080/notes/1
{"error": "not found"}
```
