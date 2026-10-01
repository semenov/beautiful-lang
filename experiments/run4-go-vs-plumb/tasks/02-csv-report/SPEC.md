# csv-report

A command-line tool that totals sales amounts from a CSV file, grouped by a
column.

## Usage

```
app --by COLUMN [FILE]
```

- With no FILE, read standard input.
- `--by COLUMN` (required): the name of the column to group by.

## Input

- CSV as in RFC 4180: fields separated by `,`; records end with LF or CRLF
  (the last record may have no line break). A field may be enclosed in
  double quotes `"`; a quoted field may contain commas, line breaks, and
  quotes written as `""`. Inside an unquoted field a `"` is an ordinary
  character. Field values are used exactly as written (no trimming), except
  for amounts (see below).
- The first record is the header: it names the columns, in any order. Column
  names are matched exactly (case-sensitive). The input must have a column
  named `amount` and the `--by` column.
- An empty line (nothing between two line breaks, or between the last line
  break and the end of input) is skipped; it is not a record.
- **Amount:** after removing leading and trailing ASCII spaces, an optional
  `-`, one or more digits, and optionally `.` followed by one or two digits
  (`12`, `-0.5`, `3.25`). Anything else (`+1`, `.5`, `5.`, `1.234`, `1e3`,
  `1,000.00`, empty) is invalid. Amounts are added exactly, in cents.
  Every amount and every total is below 10^14 in absolute value.

## Bad records

A record is **bad** if it has a different number of fields than the header,
or its amount is invalid, or a quoted field's closing quote is followed by
something other than `,` or a line break (then the record ends at the next
line break). A bad record is skipped, and a line
`line N: <reason>` is printed to standard error, where N is the line number
on which the record starts (the header is line 1; line breaks inside quoted
fields count). If a quoted field is not closed before the end of input, the
record that started it is bad (`line N: unterminated quote`) and the input
ends there.

## Output

A CSV (same quoting rules: a field is quoted if it contains `,`, `"`, CR or
LF, with `"` doubled; lines end with LF). First a header line
`COLUMN,total`, then one line per distinct value of the `--by` column:
the value and the total of its amounts, written with exactly two decimals
(`10.60`, `-0.50`, `0.00`; never `-0.00`). Lines are sorted by the value,
ascending by Unicode code point. An empty value is a group like any other.

Exit code: 0 if every record was used, 1 if any record was bad (the report
is still printed).

## Errors

- No header (empty input): print `error: empty input` to standard error,
  exit 2.
- A missing column: print `error: missing column NAME` to standard error
  and exit 2 (check `amount` first, then the `--by` column).
- A FILE that can't be read: print `error: cannot read FILE` (FILE as given)
  to standard error and exit 2.
- In all these cases nothing is printed to standard output.
- `--by` missing or without a value, an unknown option, or more than one
  FILE: print a message starting with `usage:` to standard error and exit
  with code 64.

## Example

```
$ cat sales.csv
region,product,amount
North,"Widget, large",10.50
South,Gadget,3.25
North,Gizmo,0.10
$ app --by region sales.csv
region,total
North,10.60
South,3.25
```
