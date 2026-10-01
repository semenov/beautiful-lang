# parallel-fetch

A command-line tool that fetches a list of URLs concurrently and reports
what came back.

## Usage

```
app [--concurrency N] [--timeout SECONDS] [FILE]
```

- URLs are read from FILE, or from standard input when FILE is not given:
  one URL per line. Whitespace around a line is removed; empty lines and
  lines starting with `#` are ignored. The same URL may appear more than
  once; every line is fetched separately.
- Each URL is fetched with one `GET` request (no retries). Redirects
  (301, 302, 303, 307, 308 with a `Location` header, which may be relative)
  are followed, at most 10 of them per URL; the status and size reported
  are those of the final response.
- `--concurrency N` (default 4, a whole number ≥ 1): at most N URLs are
  being fetched at any moment, and while URLs remain the tool keeps N
  fetches going (it starts the next one as soon as one finishes).
- `--timeout SECONDS` (default 10, a decimal number > 0 such as `0.5` or
  `3`): the limit for one URL, from the start of its fetch to the last byte
  of the final response body, redirects included. A slow server that keeps
  sending a few bytes at a time still hits the limit.

## Output

One line per URL, in input order (not completion order), with the URL as
written in the input:

```
URL STATUS BYTES
URL error KIND
```

STATUS is the final HTTP status code (any code, including 404 and 500, is
a response, not an error), BYTES the number of bytes in the final response
body. When there is no response, KIND is one of:

- `invalid-url` — not `http://` or `https://` followed by a host; no
  request is made;
- `timeout` — the `--timeout` limit was reached;
- `connect` — the connection could not be made (for example, refused);
- `redirects` — more than 10 redirects;
- `other` — anything else (for example, the server closed the connection
  without a complete response).

Then one summary line: `total T ok A bad-status B failed C`, where A
counts responses with a 2xx status, B responses with any other status, and
C the URLs with an error; T = A + B + C.

Exit code 0 when every URL got a 2xx response (or there were no URLs),
otherwise 1.

## Errors

- FILE can't be read: `error: cannot read FILE` to standard error, exit
  code 2, nothing on standard output.
- An unknown option, a missing or invalid option value, or more than one
  FILE: a message starting with `usage:` to standard error, exit code 64.

## Example

```
$ printf 'http://localhost:8000/a\n\n# skip\nftp://x\nhttp://localhost:8000/missing\n' | app --concurrency 2
http://localhost:8000/a 200 1234
ftp://x error invalid-url
http://localhost:8000/missing 404 9
total 3 ok 1 bad-status 1 failed 1
```
