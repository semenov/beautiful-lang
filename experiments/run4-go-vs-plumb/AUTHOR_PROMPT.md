You are writing tasks for a controlled experiment: the same task is given to
two coding agents, one writing Go and one writing Plumb (a new compiled
language). Hidden black-box tests then measure how many bugs each program
has. Your job is the tasks and the hidden tests, not solutions.

Read first:
- `{E}/README.md` (the experiment);
- the finished example `{E}/tasks/01-wordfreq/` (`SPEC.md`, `check.py`,
  `ref.py`) — follow its format and level of precision exactly;
- `{E}/tasks/_lib/checklib.py` (helpers: `Checks`, `c.run`, `c.server`,
  `c.case`, `c.test`, `free_port`).

For each task assigned to you below, create `{E}/tasks/<id>/` with:

1. `SPEC.md` — self-contained, 30-90 lines. Exact usage, input and output
   formats, exit codes, HTTP status codes and JSON shapes, error messages
   (at least their required prefix), and an example. Realistic: it should
   read like a ticket for a real tool or service. No hints about any
   programming language or library.
2. `check.py` — hidden tests, 12-25 cases, run as
   `python3 check.py <binary> <workdir>`, using checklib. **Every case must
   follow from an explicit sentence of the spec**: if a test needs a
   behavior, the spec states it. Cover the happy path, edge cases (empty
   input, Unicode, big input), error paths (missing files, bad input,
   unknown ids), and — where the task involves them — concurrency
   (many parallel requests, exact totals afterwards), resource limits and
   untrusted input (quotes and SQL-like text stored verbatim, path
   traversal refused). Don't build traps aimed at one language; test what a
   careful reviewer would expect of the program. Deterministic; the whole
   check finishes within 60 s.
3. `ref.py` — a Python reference (executable, `#!/usr/bin/env python3`,
   standard library only) that passes **every** case. Run
   `python3 check.py ref.py /tmp/run4-ref/<id>` until all pass, then run it
   2 more times to make sure it's stable. If the reference is awkward to
   make pass, the spec is probably unclear: fix the spec.

Conventions:
- Servers listen on 127.0.0.1, port from the `PORT` environment variable.
  Persistent data (SQLite file, stored files) goes in the current directory
  or a path given in the spec via an environment variable; `c.server()`
  runs the program with its working directory set to the workdir.
- Tasks that call other services (fetching, webhooks, upstreams): `check.py`
  starts its own little Python HTTP server on a free port (threads, in the
  check) and passes its URL to the program by argument or environment, as
  the spec says. Slow, failing and flaky endpoints come from there.
- SQLite is available to both languages; use it only where the spec says
  "store in SQLite at <path>".
- Don't run or write any Go or Plumb code. Don't touch other task
  directories or anything outside `{E}/tasks/`.

Your tasks:
{TASKS}

Finish with one line per task: id, number of cases, and "ref passes N/N (3 runs)".
