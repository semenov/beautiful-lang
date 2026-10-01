# dup-finder

A command-line tool that finds duplicate files under a directory tree by
their content.

## Usage

```
app [--min-size BYTES] DIR
```

- Walk DIR recursively, including hidden files and directories (names
  starting with `.`).
- Only regular files are considered. Symbolic links are skipped and never
  followed, whether they point to files or to directories. Other special
  files (sockets, pipes, devices) are skipped too.
- `--min-size BYTES` (default 1): ignore files smaller than BYTES bytes. So
  by default empty files are ignored. BYTES is a whole number ≥ 0.
- Two files are duplicates when their contents are byte-for-byte identical;
  compare them by SHA-256 of the content. A **group** is a set of two or
  more files with the same content. Files must be read in a streaming way:
  files may be larger than available memory.

## Output

Paths are printed relative to DIR, with `/` between components and no
leading `./` (a file `DIR/sub/a.txt` is printed as `sub/a.txt`), exactly
as the names are on disk.

For each group print:

1. a header line `sha256:HEX size SIZE`, where HEX is the lowercase hex
   SHA-256 of the content and SIZE the size of one file in bytes;
2. one line per file in the group: two spaces, then the path. Paths are
   sorted ascending by their bytes (UTF-8);
3. an empty line.

Groups are ordered by size, largest first; groups of equal size by HEX,
ascending.

After all groups print one last line:
`groups G files F wasted W`, where G is the number of groups, F the total
number of files in all groups, and W the bytes that removing all but one
file of each group would free (the sum of SIZE × (files in group − 1)).

With no duplicates the output is just `groups 0 files 0 wasted 0`.

## Errors

- A file or directory inside DIR that can't be read (for example, no
  permission): print `error: cannot read PATH` to standard error (PATH
  relative to DIR, as above), leave it out, and continue. After printing
  the normal output, exit with code 1. Otherwise exit with code 0.
- DIR does not exist or is not a directory: print
  `error: not a directory: DIR` (DIR as given) to standard error, print
  nothing to standard output, and exit with code 2.
- An unknown option, a missing option value, a `--min-size` value that is
  not a whole number ≥ 0, or not exactly one DIR: print a message starting
  with `usage:` to standard error and exit with code 64.

## Example

```
$ printf hello > d/a.txt; printf hello > d/sub/b.txt; printf other > d/c.txt
$ app d
sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 size 5
  a.txt
  sub/b.txt

groups 1 files 2 wasted 5
```
