# todo-sqlite

A command-line todo list. Data is stored in SQLite at the path given by the
environment variable `TODO_DB`; the file and its tables are created on first
use, and every invocation sees what earlier invocations stored.

## Usage

```
app add TITLE [--due YYYY-MM-DD] [--tag TAG]...
app list [--tag TAG] [--overdue --today YYYY-MM-DD]
app done ID
app rm ID
app search TEXT
```

Options may come before or after the positional argument of a command.

- `add`: create a todo, print `added ID`. TITLE is any non-empty text
  without newline characters (`\n`, `\r`), stored exactly as given: quotes,
  `%`, `_`, backslashes and SQL-like text are ordinary characters. IDs are
  whole numbers: the first todo gets 1, each new todo gets the next number,
  and an ID is never reused, even after its todo is removed. `--due` sets a
  due date; it must be a real calendar date (`2025-02-30` is invalid,
  `2024-02-29` is valid). `--tag` may repeat; a TAG is one or more of the
  characters `A-Z a-z 0-9 _ -`; repeated tags are stored once.
- `list`: print all todos, open and done. `--tag TAG`: only todos with
  exactly that tag (case-sensitive). `--overdue`: only todos that are not
  done and have a due date strictly before the `--today` date. `--overdue`
  and `--today` must be given together.
- `done ID`: mark the todo done, print `done ID`. Marking a done todo again
  is fine and prints the same.
- `rm ID`: delete the todo, print `removed ID`.
- `search TEXT`: print the todos whose title contains TEXT as a substring,
  case-sensitive, every character matched literally (`%` matches only `%`,
  `_` only `_`). TEXT must be non-empty.

## Output of list and search

One line per todo, ordered by ID ascending:

```
ID [ ] TITLE
ID [x] TITLE (due YYYY-MM-DD) #tag1 #tag2
```

`[x]` marks a done todo, `[ ]` an open one. ` (due DATE)` is present only if
the todo has a due date; then ` #TAG` for each tag, tags sorted ascending by
code point. When nothing matches, print nothing (exit code 0).

## Errors

- `TODO_DB` unset or empty: print `error: TODO_DB is not set` to standard
  error and exit with code 2.
- `done` or `rm` with an ID that does not exist (never existed, or was
  removed): print `error: no such todo: ID` to standard error and exit with
  code 3.
- Anything else malformed — unknown command or option, missing or extra
  arguments, an empty TITLE or one with a newline, an invalid date, an
  invalid TAG, an ID that is not a whole number ≥ 1, an empty search TEXT,
  `--overdue` without `--today` or the reverse: print a message starting
  with `usage:` to standard error and exit with code 64. Nothing is stored.

On every error nothing is printed to standard output.

## Example

```
$ export TODO_DB=/tmp/todo.db
$ app add "Buy milk" --tag home --due 2026-03-01
added 1
$ app add "50% off: ask Bob's \"deal\""
added 2
$ app done 2
done 2
$ app list
1 [ ] Buy milk (due 2026-03-01) #home
2 [x] 50% off: ask Bob's "deal"
$ app list --overdue --today 2026-03-02
1 [ ] Buy milk (due 2026-03-01) #home
```
