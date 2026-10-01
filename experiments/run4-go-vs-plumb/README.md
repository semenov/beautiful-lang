# Run 4: do agents make fewer mistakes in Plumb than in Go?

The claim being tested: in Plumb, more of an agent's mistakes are caught by
the compiler, and the code a person has to review is smaller.

## Setup

- **Tasks** (`tasks/NN-name/`): CLI tools, text processing, HTTP services,
  SQLite, concurrency. Each has
  - `SPEC.md`, the only thing the writer sees;
  - `check.py`, hidden black-box tests (run the binary, or talk to it over
    HTTP). Every case follows from a sentence in the spec;
  - `ref.py`, a Python reference that passes every check (proves the checks
    and the spec agree, and that they don't favor either language).
- **Writers:** one fresh agent per task and language, the same model and the
  same prompt (`WRITER_PROMPT.md`), differing only in the language and its docs.
  Go: standard library plus `github.com/mattn/go-sqlite3`. Plumb: standard
  library; the agent gets `AGENTS.md` and `plumb doc`. Models know Go from
  training and Plumb only from the guide; that handicap is part of the result.
- **Compiling** goes through `bin/build`, which logs each attempt and keeps
  a snapshot of the source and binary (`solutions/<task>/<lang>/.builds/NNN/`).
  For Go a "build" is `go build` plus `go vet`, so Go gets credit for its
  standard checker too.

## What is measured

1. **Bugs that got past the compiler:** the hidden tests on the **first
   version that compiled** (before the agent tried its program) and on the
   **final** version (after the agent's own testing).
2. **Code to review:** non-blank, non-comment lines of program code (tests
   excluded).
3. **Tries to compile:** builds until the first success, and failed builds
   in total.

Afterwards each failing hidden test gets a cause (for example nil, ignored
error, data race, wrong spec reading, injection), to see which mistakes
each language lets through.

## Watching it

- `STATUS.md` is rewritten on every event: a totals table, one row per task,
  the latest events.
- `bin/watch` shows it live in a terminal.
- `results/events.jsonl` is the raw log (start, every build, done, checks).
