# business-days

A command-line tool for business-day arithmetic.

## Usage

```
app between START END [--holidays FILE] [--weekend DAYS]
app add DATE N [--holidays FILE] [--weekend DAYS]
```

Options may appear anywhere after the command name.

- Dates are written `YYYY-MM-DD`: exactly 4 digits, `-`, 2 digits, `-`,
  2 digits, and must be a real date in the Gregorian calendar, years 0001 to
  9999 (`2024-02-29` is valid, `2023-02-29`, `1900-02-29` and `2025-04-31`
  are not, `2025-4-01` is malformed).
- A **business day** is a date that is not a weekend day and not a holiday.
- `--weekend DAYS`: the weekend days, a comma-separated list of names from
  `mon tue wed thu fri sat sun` (lowercase; repeats allowed). Default
  `sat,sun`. An empty value (`--weekend ""`) means no weekend days. Naming
  all seven days is an error.
- `--holidays FILE`: one date per line. `#` starts a comment that runs to
  the end of the line. Whitespace around a date is ignored; lines that are
  empty after removing the comment and whitespace are ignored. A holiday
  falling on a weekend day changes nothing.

### between

Print the number of business days d with START ≤ d < END (START counted,
END not). If END is before START, print the negative of the number of
business days d with END ≤ d < START. START = END gives `0`.

### add

N is a whole number, optionally with a leading `-` (`0`, `5`, `-3`).

- N > 0: print the N-th business day after DATE (DATE itself is never
  counted: from a Friday with the default weekend, `add FRI 1` is the next
  Monday).
- N < 0: print the |N|-th business day before DATE.
- N = 0: print DATE if it is a business day, otherwise the first business
  day after it.

Output is the date in `YYYY-MM-DD` form, one line.

## Errors

- An invalid date on the command line: `error: invalid date: TEXT` to
  standard error, exit code 2.
- Holidays file can't be read: `error: cannot read FILE`, exit code 2.
- A holidays line that is not a valid date: `error: FILE:LINE: invalid date`
  (LINE is the 1-based line number), exit code 2.
- Unknown command or option, wrong number of arguments, a missing option
  value, an N that is not a whole number, an unknown weekend day name, or
  all seven days as weekend: a message starting with `usage:` to standard
  error, exit code 64.

On every error nothing is printed to standard output.

## Example

```
$ cat hol.txt
# public holidays
2025-12-25   # Christmas
2025-12-26
$ app between 2025-12-22 2025-12-29 --holidays hol.txt
3
$ app add 2025-12-24 1 --holidays hol.txt
2025-12-29
$ app add 2025-12-21 0
2025-12-22
```
