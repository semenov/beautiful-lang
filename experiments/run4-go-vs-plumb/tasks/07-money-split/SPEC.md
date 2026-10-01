# money-split

A command-line tool that settles shared expenses: who owes whom how much.

## Usage

`app [FILE]` — with no FILE, read standard input.

## Input

One expense per line; lines end with LF or CRLF. Empty lines, lines of only
spaces and tabs, and lines whose first non-blank character is `#` are
ignored. An expense has three fields separated by one or more spaces or
tabs (leading and trailing blanks are ignored):

```
PAYER AMOUNT SPLIT
alice 90.00 alice,bob,carol
bob 25 alice:2,bob
```

- PAYER: a **name** — one or more characters, none of them space, tab,
  `,`, `:` or `#`. Names are case-sensitive (`Ann` and `ann` differ).
- AMOUNT: digits, optionally `.` and one or two digits; more than 0 and at
  most 1000000000.00.
- SPLIT: one or more participants separated by `,` (no spaces), each
  `NAME` or `NAME:SHARES`, SHARES a whole number from 1 to 1000 (default
  1). A name may appear only once in a SPLIT. The payer need not be a
  participant.

## Splitting

An expense of T cents among participants with shares s1..sk (S = their sum)
charges participant i ⌊T × si / S⌋ cents. The R cents left over (T minus the
sum of those) go one each to the first R participants in the order they are
listed in SPLIT. The payer is credited T.

A person's **balance** is everything they were credited minus everything
they were charged, over all expenses.

## Settling

Transfers are chosen by repeating, while any balance is not zero: take the
debtor with the most negative balance and the creditor with the most
positive balance (ties: the name that comes first by Unicode code point),
transfer the smaller of the two amounts from the debtor to the creditor,
and update both balances.

## Output

```
balances:
  NAME AMOUNT
transfers:
  FROM -> TO AMOUNT
```

`balances:` lists every person named in any expense, one per line, sorted
by name ascending by Unicode code point; a balance is written with exactly
two decimals and a sign: `+60.00`, `-0.05`, and `0.00` for zero. Under
`transfers:` the transfers in the order chosen, amounts with two decimals
and no sign (`bob -> alice 30.00`); nothing if there are none. Each line is
indented by two spaces. Exit code 0.

## Errors

- A line that is not a valid expense: print `error: line N: <reason>`
  (N is the 1-based line number; ignored lines count) to standard error and
  exit with code 1, printing nothing to standard output. Report only the
  first bad line.
- A FILE that can't be read: print `error: cannot read FILE` to standard
  error (FILE as given), exit with code 2.
- More than one argument, or an argument starting with `-`: print a message
  starting with `usage:` to standard error and exit with code 64.

## Example

```
$ printf 'alice 90.00 alice,bob,carol\nbob 30 alice:2,bob\n' | app
balances:
  alice +40.00
  bob -10.00
  carol -30.00
transfers:
  carol -> alice 30.00
  bob -> alice 10.00
```
