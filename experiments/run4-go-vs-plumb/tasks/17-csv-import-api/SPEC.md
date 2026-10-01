# csv-import-api

An HTTP service that imports users from CSV uploads into SQLite, all rows or
none.

## Running

```
PORT=8080 DB_PATH=/data/users.db app
```

Listen on `127.0.0.1:$PORT`. Store users in the SQLite database at
`DB_PATH`; create the file and its table if they don't exist. Data survives
a restart. Many requests may arrive at the same time.

## `POST /import`

The request body is the CSV file itself (UTF-8). Format:

- Records are separated by `\n` or `\r\n`; a final line break is optional.
- Fields are separated by `,`. A field may be enclosed in double quotes; a
  quoted field may contain `,`, line breaks, and `""` (one literal `"`).
  Fields are taken exactly as written (no trimming).
- The first record is the header. It must contain exactly the three column
  names `email`, `name`, `age`, in any order. Data rows are numbered from 1
  (the first record after the header).

Each data row is checked; a row may have several errors, one per field:

| field | rule | message |
|---|---|---|
| `row` | the row has exactly 3 fields (if not, no other check is done for it) | `wrong number of fields` |
| `email` | exactly one `@`; non-empty part before it; the part after it has a `.` that is neither its first nor its last character; no spaces or other whitespace; at most 254 characters | `invalid email` |
| `email` | not already stored (case-insensitive) | `email already exists` |
| `email` | not used by an earlier row of the same file (case-insensitive) | `duplicate email in file` |
| `name` | 1 to 100 characters (Unicode code points) | `invalid name` |
| `age` | only decimal digits, value 13 to 120 | `invalid age` |

For an email only the first failing check in the table applies (so a stored
email gives `email already exists` on every row that uses it).

- No errors: store every row in **one transaction** and respond
  `201 {"imported": N}`. Emails are stored lowercased; names exactly as
  given; age as a number. A file with a header and no data rows gives
  `{"imported": 0}`.
- Any error: store nothing and respond `422`:
  `{"imported": 0, "errors": [{"row": 2, "field": "age", "message": "invalid age"}, ...]}`,
  ordered by row, and within a row in the order `row`, `email`, `name`, `age`.
- Concurrent imports behave as if they ran one after another: two imports
  that share an email can never both succeed; the later one gets
  `email already exists` for it.
- `400 {"error": "invalid csv"}`: the body is empty, is not UTF-8, has an
  unterminated quoted field, a `"` inside an unquoted field or right after a
  closing quote, or the header is not as described.

## `GET /users`

`200 {"users": [{"email": "ann@example.com", "name": "Ann", "age": 34}, ...]}`,
all stored users ordered by email (ascending, by code point).

Any other path gives `404 {"error": "not found"}`; another method on
`/import` or `/users` gives `405 {"error": "method not allowed"}`.

## Example

```
$ printf 'email,name,age\nAnn@Example.com,Ann,34\nbob@example,"Smith, Bob",12\n' |
    curl -s -XPOST --data-binary @- localhost:8080/import
{"imported": 0, "errors": [{"row": 2, "field": "email", "message": "invalid email"},
                           {"row": 2, "field": "age", "message": "invalid age"}]}
$ printf 'email,name,age\nAnn@Example.com,Ann,34\n' | curl -s -XPOST --data-binary @- localhost:8080/import
{"imported": 1}
$ curl -s localhost:8080/users
{"users": [{"email": "ann@example.com", "name": "Ann", "age": 34}]}
```
