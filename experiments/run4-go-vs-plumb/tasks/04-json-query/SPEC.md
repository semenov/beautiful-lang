# json-query

A command-line filter for JSON Lines: keeps the records that match some
conditions and prints selected fields.

## Usage

```
app [--where PATH=VALUE]... [--select PATH,PATH...]
```

Reads JSON Lines from standard input and writes JSON Lines to standard
output.

- A **PATH** is one or more object keys joined by `.`: `user.address.city`
  means the key `city` of the object under `address` of the object under
  `user`. Keys in a PATH are not empty and contain no `.`. A PATH that runs
  into a missing key or into a value that is not an object finds nothing
  (there is no array indexing).
- `--where PATH=VALUE` (may be repeated; all must hold): split at the first
  `=` (VALUE may contain `=` and may be empty). A record matches when PATH
  finds a value and either it is a string whose content (after JSON
  unescaping) equals VALUE exactly, or it is a number, `true`, `false` or
  `null` whose JSON text, as written in the input, equals VALUE exactly
  (`--where n=1.10` matches `1.10` but not `1.1`). Objects and arrays never
  match. A PATH that finds nothing does not match.
- `--select PATH,PATH...`: print, for each matching record, an object whose
  keys are the given PATHs as written (`"b.c"` is one key), in the given
  order, with the value each PATH finds, or `null` if it finds nothing.
  Without `--select`, print the whole record.

## Input

- One JSON value per line (lines end with LF; a line may end with CR, which
  is whitespace). Lines that are empty or only whitespace are skipped.
- Each line must be a JSON object (RFC 8259). Keys within one object are
  unique.
- If a line is not valid JSON, print `error: line N: invalid JSON` to
  standard error; if it is valid JSON but not an object, print
  `error: line N: not an object`. N is the 1-based line number (skipped
  lines count). Then go on with the next line.

## Output

One line per matching record, in input order, as compact JSON:

- no whitespace outside strings; members and elements separated by `,`,
  keys and values by `:`;
- object keys in the same order as in the input;
- numbers written with exactly the text they had in the input
  (`12345678901234567890`, `1.10`, `1e400`, `-0` stay as they are);
- strings with `"` written as `\"`, `\` as `\\`, the control characters
  U+0008, U+0009, U+000A, U+000C, U+000D as `\b`, `\t`, `\n`, `\f`, `\r`,
  other characters below U+0020 as `\u00XX` (lowercase hex), and every
  other character as itself in UTF-8 (`é` in the input prints as `é`).

Exit code: 0 if every line was valid, 1 if any line was reported as an
error (the other lines are still processed and printed).

## Errors

An unknown option, a missing option value, a `--where` without `=`, or an
empty PATH or empty key in a PATH (`--where =x`, `--select a,,b`,
`--select a..b`): print a message starting with `usage:` to standard error,
print nothing to standard output and exit with code 64.

## Example

```
$ printf '%s\n' '{"id":1,"user":{"name":"Ann"},"total":12.50}' \
    '{"id":2,"user":{"name":"Bob"},"total":3}' '[1]' \
  | app --where user.name=Ann --select id,total,user.age
{"id":1,"total":12.50,"user.age":null}
error: line 3: not an object        (on standard error; exit code 1)
```
