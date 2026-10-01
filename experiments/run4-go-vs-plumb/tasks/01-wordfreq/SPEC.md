# wordfreq

A command-line tool that prints the most common words in text.

## Usage

```
app [--top N] [--min-length L] [FILE...]
```

- With no FILE, read standard input. With one or more FILEs, read all of
  them, in order, as one text.
- A **word** is a maximal run of Unicode letters (`L*` categories) and
  apostrophes `'`. Leading and trailing apostrophes are not part of the word
  (`'tis'` → `tis`); a run that is only apostrophes is not a word.
  Everything else separates words. Words are compared after lowercasing
  (Unicode simple lowercase: `Ärger` → `ärger`).
- `--min-length L` (default 1): ignore words shorter than L characters
  (Unicode code points, not bytes).
- `--top N` (default 10): print at most N words.
- Output: one line per word, `word count`, most common first; equal counts
  are ordered by the word, ascending by code point. Then one last line:
  `total W unique U`, where W is the number of words counted (after the
  `--min-length` filter) and U the number of distinct ones.

## Errors

- A FILE that can't be read: print `error: cannot read FILE` to standard
  error (FILE as given) and exit with code 2, printing nothing to standard
  output.
- An unknown option, a missing option value, or a value that is not a whole
  number ≥ 0 (for `--top`) or ≥ 1 (for `--min-length`): print a message
  starting with `usage:` to standard error and exit with code 64.
- Input that is not valid UTF-8: treat each invalid byte as a separator.

## Example

```
$ printf "The cat and the hat. THE END" | app --top 2
the 3
and 1
total 7 unique 5
```
