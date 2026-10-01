# config-merge

A command-line tool that builds a service's effective configuration from
layered JSON files and environment variables, checks it, and prints it.

## Usage

`app FILE...` — at least one FILE. Each FILE holds one JSON value
(RFC 8259) that must be an object.

## Merging

1. Start from the first file; merge each next file into the result, in
   order. Merging B into A: for each key of B, if both A and B have an
   object under that key, merge them the same way (deeply); otherwise B's
   value replaces A's (arrays are replaced, not concatenated; `null`
   replaces like any other value). Keys only in A stay.
2. Then apply environment overrides: every environment variable whose name
   starts with `APP_`, in ascending order of the variable name (byte order).
   The rest of the name is split on `__` (two underscores) into a path of
   keys, each lowercased (ASCII): `APP_DB__PORT` sets `db.port`,
   `APP_LOG_LEVEL` sets `log_level`. A variable whose path has an empty key
   (`APP_`, `APP_DB__`, `APP___X`) is ignored. Setting a path creates
   missing objects on the way; a value on the way that is not an object is
   replaced by a new object. The value is the variable's value parsed as
   JSON if it is valid JSON (`5433` is a number, `true` a boolean,
   `[1,2]` an array, `"x"` the string `x`), otherwise the string as is
   (`localhost`, an empty value is the empty string).

## Validation

The result must have `service.name` and `db.host`, both non-empty
strings, and `db.port`, a number with no fraction or exponent (digits only,
no sign) from 1 to 65535.

For every key that fails, in the order above, print
`error: invalid KEY` (KEY as above, e.g. `error: invalid db.port`) to
standard error — this includes a missing key — then exit with code 1,
printing nothing to standard output.

## Output

The result as JSON, keys of every object sorted ascending by Unicode code
point, indented with 2 spaces per level:

- a non-empty object is `{`, then one line per member `"key": value`
  (members separated by `,` at the end of the line), then `}` on its own
  line at the indentation of the object's first line; a non-empty array is
  the same with `[`, elements and `]`; `{}` and `[]` when empty;
- numbers are written with exactly the text they had in the file or
  variable (`1.50`, `12345678901234567890`);
- strings: `"` as `\"`, `\` as `\\`, U+0008, U+0009, U+000A, U+000C,
  U+000D as `\b`, `\t`, `\n`, `\f`, `\r`, other characters below U+0020 as
  `\u00XX` (lowercase hex), everything else as itself in UTF-8;
- the output ends with a line break. Exit code 0.

## Errors

- A FILE that can't be read: `error: cannot read FILE`; not valid JSON:
  `error: invalid JSON in FILE`; valid JSON but not an object:
  `error: FILE is not an object` (FILE as given). Print it to standard
  error, exit with code 2, print nothing to standard output. Files are
  checked in order; stop at the first bad one.
- No FILE, or an argument starting with `-`: print a message starting
  with `usage:` to standard error and exit with code 64.

## Example

```
$ cat base.json
{"service": {"name": "api", "tags": ["a", "b"]}, "db": {"host": "db1", "port": 5432}}
$ cat prod.json
{"service": {"tags": ["prod"]}, "db": {"host": "db.prod"}}
$ APP_DB__PORT=5433 APP_DEBUG=false app base.json prod.json
{
  "db": {
    "host": "db.prod",
    "port": 5433
  },
  "debug": false,
  "service": {
    "name": "api",
    "tags": [
      "prod"
    ]
  }
}
```
