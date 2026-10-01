# log-stats

A command-line tool that summarizes web server access logs per endpoint.

## Usage

```
app [--min-count N] [FILE...]
```

- With no FILE, read standard input. With one or more FILEs, read all of
  them, in order, as one log.
- `--min-count N` (default 1): leave out endpoints seen fewer than N times
  (they still count in the totals line).

## Input

One request per line; lines end with LF or CRLF. A valid line is exactly
these ten parts, separated by single spaces (a *token* is one or more
characters other than space):

```
CLIENT IDENT USER [TIME] "METHOD TARGET PROTOCOL" STATUS BYTES LATENCY
203.0.113.9 - - [10/Oct/2024:13:55:36 +0000] "GET /api/users?id=3 HTTP/1.1" 200 2326 45
```

- CLIENT, IDENT, USER: tokens. TIME: `[`, any characters except `]`, `]`.
- The request in double quotes: METHOD is one or more uppercase ASCII
  letters `A`–`Z`; TARGET is a token not containing `"`; PROTOCOL is
  `HTTP/` followed by one or more digits and dots.
- STATUS: three digits, from 100 to 599.
- BYTES: digits, or `-`.
- LATENCY: the response time in milliseconds, one or more digits (leading
  zeros allowed, `007` is 7; values are below 10^12).

Nothing may come before CLIENT or after LATENCY. An empty line is ignored.
Any other line that does not match is **malformed**: it is counted and
otherwise ignored.

## Statistics

The **endpoint** of a request is METHOD, a space, and the TARGET up to (not
including) the first `?` (`GET /api/users`). For each endpoint:

- `count`: number of requests.
- `error_rate`: the percentage of requests with STATUS 500–599, written
  with exactly two decimals, `%` appended. It is exactly
  100 × errors / count, rounded to the nearest 0.01, halves rounded up
  (1 error in 800 requests is `0.13%`).
- `p50`, `p95`: nearest-rank percentiles of the latencies. Sort the
  endpoint's n latencies ascending; the P-th percentile is the value at
  1-based position ⌈P × n / 100⌉, computed exactly (for n = 20, p95 is the
  19th value; for n = 1, every percentile is that value).
- `max`: the largest latency.

## Output

One line per endpoint (with at least `--min-count` requests), most requests
first, equal counts ordered by endpoint ascending by Unicode code point:

```
ENDPOINT count=C error_rate=R% p50=A p95=B max=M
```

Numbers are written in decimal without leading zeros. Then one last line:
`total=T malformed=M`, where T is the number of valid lines and M the number
of malformed ones. Exit code 0, also when there were malformed lines.

## Errors

- A FILE that can't be read: print `error: cannot read FILE` to standard
  error (FILE as given), exit 2, print nothing to standard output.
- An unknown option, a missing option value, or a `--min-count` that is not
  a whole number ≥ 1: print a message starting with `usage:` to standard
  error and exit with code 64.

## Example

```
$ cat access.log
1.2.3.4 - - [10/Oct/2024:13:55:36 +0000] "GET /api/users?id=3 HTTP/1.1" 200 512 40
1.2.3.4 - - [10/Oct/2024:13:55:37 +0000] "GET /api/users HTTP/1.1" 503 - 120
1.2.3.4 - - [10/Oct/2024:13:55:38 +0000] "POST /api/login HTTP/1.1" 200 64 15
garbage
$ app access.log
GET /api/users count=2 error_rate=50.00% p50=40 p95=120 max=120
POST /api/login count=1 error_rate=0.00% p50=15 p95=15 max=15
total=3 malformed=1
```
