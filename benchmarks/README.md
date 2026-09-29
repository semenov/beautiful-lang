# Benchmarks

Five workloads, written in plain idiomatic code in JavaScript (run under Node,
Deno and Bun), Go, Rust and C. When a Linen compiler exists, it gets its own
column.

Every program uses the same pseudo-random generator (xorshift32) and prints a
checksum. `run.py` fails if any implementation prints something different, so
every column provably does the same work.

| Benchmark | What it does | What it stresses |
|---|---|---|
| `records` | 10 rounds: build a list of 10M `{x, y}` records by appending, then sum it | memory layout of records, list growth |
| `trees` | Binary trees up to depth 20 (the classic Benchmarks Game test): tens of millions of short-lived nodes | allocating and freeing many small objects |
| `sort` | 5 rounds: 3M `{id, key}` records with random keys, sorted by key | sorting records, comparison calls |
| `words` | 5 rounds: build a 5M-word text, split it, count words in a hash map, print the top 5 | strings, hash maps |
| `json` | 10 rounds: encode 300k user records to JSON and decode them back into typed values | JSON, the most common "real" task |

C has no `json` column: its standard library has no JSON.

## Running

```sh
python3 run.py                # everything, 3 runs each
python3 run.py trees sort     # a subset
RUNS=5 python3 run.py         # more runs
```

Needs macOS (for `/usr/bin/time -l`), plus `clang`, `go`, `cargo`, `node`,
`deno` and `bun`.

## Results

Apple M3 Max, 36 GB · Node 23.8 · Deno 2.9.6 · Bun 1.4.2 · Go 1.27.1 ·
Rust 1.98.1 (`--release`) · Apple clang 21 (`-O2`) · 2026-09-26.

Median of 3 runs. Each cell is wall time · peak memory (maximum resident set
size; macOS's "peak memory footprint" agrees within a few percent).

| Benchmark | Node | Deno | Bun | Go | Rust | C |
|---|---|---|---|---|---|---|
| records | 2.04 s · 2216 MB | 1.58 s · 2665 MB | 0.98 s · 2893 MB | 0.45 s · 395 MB | 0.28 s · 1530 MB¹ | 0.16 s · 1530 MB¹ |
| trees | 2.77 s · 681 MB | 2.64 s · 705 MB | 1.74 s · 566 MB | 2.77 s · 136 MB | 4.42 s · 66 MB² | 3.79 s · 66 MB² |
| sort | 7.31 s · 768 MB | 6.94 s · 1003 MB | 4.43 s · 683 MB | 1.61 s · 189 MB | 0.34 s · 71 MB | 1.27 s · 25 MB |
| words | 4.41 s · 1064 MB | 3.69 s · 1035 MB | 2.63 s · 1645 MB | 0.93 s · 213 MB | 0.67 s · 121 MB | 0.55 s · 119 MB |
| json | 1.39 s · 688 MB | 1.04 s · 704 MB | 0.76 s · 671 MB | 2.07 s · 265 MB | 0.76 s · 473 MB | — |

**¹ The macOS system allocator retains freed memory.** When a list grows
through a chain of `realloc` calls, the memory from each round is never
returned or reused, so the process accumulates about 160 MB per round. With
memory reserved up front, C stays at **161 MB** over the same 10 rounds. With
mimalloc instead of the system allocator, Rust uses **431 MB** (0.48 s). This
is the allocator's behavior, not the language's.

**² Allocating many small objects with `malloc`/`free` is slower than with a
garbage collector,** which allocates by bumping a pointer. With mimalloc,
Rust's `trees` takes **2.4 s · 96 MB**. That's faster than Node and Deno, but
still behind Bun.

## What this means

- **JavaScript runtimes** are the slowest in 4 of 5 tests and use **5–20× more
  memory** than native code. Bun is the best of the three, but it's in the
  same class.
- **Go** is fast, but its garbage collector keeps a reserve: it uses 2–4× the
  memory of Rust or C. In JSON it's the slowest of all, because
  `encoding/json` works through reflection.
- **Rust and C** use the least memory. Rust sorts about 4× faster than C's
  `qsort` (comparison inlined vs. called through a pointer) and is about as
  fast as JavaScript in JSON.

## Lessons for Linen

1. **Ship our own allocator.** Don't rely on the system `malloc`. Both
   anomalies above come from it. A mimalloc-class allocator is the baseline.
2. **Reference counting needs fast allocation of small objects.** The `trees`
   pattern is the weak spot of any design without a garbage collector.
   Reusing memory in place when a value has a single owner (Perceus) helps.
   Beyond that, we'll need per-thread free lists and perhaps arenas for
   short-lived data.
3. **Inline records and comparison functions.** `sort` shows what inlining is
   worth (Rust 0.34 s vs. C `qsort` 1.27 s). Generics compiled per type give
   us that automatically.
4. **JSON should be generated from types at compile time, not done through
   reflection** (Go's weak spot here). `json.decode<T>` already knows `T`.
