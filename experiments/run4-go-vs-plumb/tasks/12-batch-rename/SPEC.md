# batch-rename

A command-line tool that renames many files in one directory by a pattern,
all or nothing.

## Usage

```
app DIR --regex PATTERN --to TEMPLATE [--dry-run]
app DIR --counter TEMPLATE [--start N] [--match PATTERN] [--dry-run]
```

Options and DIR may come in any order. Exactly one of `--regex` and
`--counter` must be given; `--to` only goes with `--regex`, and `--start`
and `--match` only with `--counter`.

- Only regular files directly in DIR are candidates. Subdirectories (and
  anything in them), symbolic links and other special files are never
  renamed. Candidates are processed in order of their names, ascending by
  code point.
- PATTERN is a regular expression using only this common syntax: literal
  characters, `.`, character classes like `[a-z]` and `[^0-9]`, `\d`, `\w`,
  `\s`, `\.` (and other escaped punctuation), `*`, `+`, `?`, `{m,n}`, groups
  `(...)`, `|`, `^` and `$`. It must match the **whole** file name;
  candidates it doesn't match are left alone.
- `--regex` mode: the new name is TEMPLATE with each `{N}` (N = one or
  more digits) replaced by the text of group N of the match; `{0}` is the
  whole name, and a group that did not take part in the match gives an
  empty string. `{{` stands for a literal `{` and `}}` for `}`. Any other
  `{` or `}`, or a group number higher than the number of groups in
  PATTERN, is a usage error.
- `--counter` mode: TEMPLATE must contain exactly one run of `#`
  characters, and no other `#`. The k-th candidate (k = 0, 1, …, in name
  order) gets TEMPLATE with that run replaced by the number N + k
  (`--start N`, default 1, a whole number ≥ 0), padded with leading zeros
  to the length of the run; longer numbers are not cut. `--match PATTERN`
  limits the candidates to names PATTERN matches (whole name); by default
  all candidates are renamed.
- A candidate whose new name equals its current name is left out of the
  batch (not renamed, not printed, not counted).

## Safety

The batch is checked as a whole before anything is renamed. If any of
these hold, nothing is renamed, nothing is printed to standard output, a
message is printed to standard error and the exit code is 1:

- a new name is empty, `.` or `..`, or contains `/` or a NUL character
  (`error: invalid name: NAME`) — files must never leave DIR;
- two candidates would get the same new name (`error: conflict: NAME`);
- a new name already exists in DIR as any entry — a file, directory or
  link, including files that this batch would rename away
  (`error: exists: NAME`).

## Output

For each renamed file, in name order: `OLD -> NEW`. Then a last line
`renamed N`. With `--dry-run`, nothing is changed, the same lines are
printed and the last line is `would rename N`. Exit code 0.

## Errors

- DIR doesn't exist or isn't a directory: `error: not a directory: DIR`,
  exit code 2.
- Bad usage (unknown option, missing value, both or neither mode, invalid
  PATTERN, invalid TEMPLATE, bad `--start`): a message starting with
  `usage:` to standard error, exit code 64.

## Example

```
$ ls photos
IMG_0003.JPG  IMG_0012.JPG  notes.txt
$ app photos --regex 'IMG_(\d+)\.JPG' --to 'photo-{1}.jpg'
IMG_0003.JPG -> photo-0003.jpg
IMG_0012.JPG -> photo-0012.jpg
renamed 2
$ app photos --counter 'pic-###.jpg' --match '.*\.jpg' --start 9
photo-0003.jpg -> pic-009.jpg
photo-0012.jpg -> pic-010.jpg
renamed 2
```
