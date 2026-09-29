# What we do about the critique (2026-09-29)

The critique is `critique.md` (38 complaints, each checked by running
code). For each: **fix** (a bug, a hole the docs already promise to
close, a performance cliff, wrong docs), **explain** (the design is
right; README's "Choices that surprise people" says why), or **ask**
(a change to the language model: Vlad decides). The test for every
answer: does it keep the language simple and fast?

| # | Complaint | Answer |
|---|---|---|
| 1 | a change to a copy is silently lost | **fix**: the error DESIGN.md promises ("you changed a copy that is never used afterward"); the loop-variable hint points to an index loop |
| 2 | enum patterns bind by position | **fix**: a binding named like a *different* field of the variant is an error (`Rect(height, width)`) |
| 3 | string indexing quadratic for non-ASCII | **fix**: a string remembers its last (character, byte) position, so walking it is linear |
| 4 | copy-on-write makes some loops quadratic | **explain** the rule (a change to a value someone else still holds copies it) + the idioms that avoid it; a compiler note later |
| 5 | `Shared` is one mutex | **fix**: `with v = s.read() { }` for readers (a readers-writer lock) |
| 6 | generics without bounds | **explain**: asked; they stay without bounds (pass a function) |
| 7 | SQL only as one literal | **explain**: asked; stays one literal for now |
| 8 | an optional prints as `none` in text | **ask** (make it an error) |
| 9 | `counts[w] += 1` compiles, then panics | **ask** (make it an error) |
| 10 | no private fields | **explain**: asked; fields stay visible (most types are data) |
| 11 | wrapping an error hides its type | **ask** (`is` looks through `cause`) |
| 12 | lambdas capture a snapshot silently | **ask** (an error when the captured `var` changes later) |
| 13 | two ways to call a 2-parameter function | **ask** |
| 14 | floored `div` and `%` undocumented | **fix** the docs (floored is right: `(-1) % 7 == 6`) |
| 15 | `is none or ...` doesn't narrow | **fix**: flow typing through `or`/`and`, as DESIGN.md promises |
| 16 | sorting keys | **fix**: a new type over an ordered type is ordered (`UserId`); no `to_string` hint in a sort key; the rest later |
| 17 | `Decimal` rounds and overflows | **fix** the docs now; 38 digits (128-bit) later |
| 18 | a lambda keeps a closed `with` resource | **fix**: an error when a `with` variable is captured by a lambda that outlives the block |
| 19 | deadlock detection in servers | **fix** the docs (it catches only "everyone waits") |
| 20 | fire-and-forget from a handler | **explain** + a worked example (a channel to a worker from `main`) |
| 21 | `plumb test` doesn't test the project | **fix**: `plumb test` runs every test in the project; unknown flags are errors |
| 22 | DESIGN.md claims that aren't true | **fix**: Shared-in-Shared error, `Duration` ordered, the literal claim corrected |
| 23 | `is` on a never-thrown type crashes cc | **fix** (compiler bug) |
| 24 | no call stack on panic, no location on errors | **fix later** (TODO) |
| 25 | no unused-variable / unused-import checks | **ask** (make them errors, as Go) |
| 26 | lax number parsing | **fix**: `to_int`/`to_float` refuse spaces, `_`, NaN and infinity |
| 27 | fuzzy JSON keys, first duplicate wins | **fix**: a duplicate key is an error; **explain** the fuzzy match |
| 28 | regex group numbering, runtime-only errors | **fix**: `groups[0]` is the whole match (as `$0`); regex literals checked at compile time |
| 29 | wrong hints | **fix** each |
| 30 | mutating through an interface-typed element | **fix later** (TODO) |
| 31 | no nested functions | **ask** |
| 32 | ranges only in `for` | **explain**: asked; left as they are (counting down is a `while`) |
| 33 | module names take variable names; no shadowing | **explain** |
| 34 | inconsistent member shapes | **fix** `chunks(0)`; **explain** the rest |
| 35 | no operators for user types | **explain** (and `Duration <` from 22) |
| 36 | whole-program C compile | **fixed** in part (cached runtime, build cache); more in TODO |
| 37 | string building in a loop is quadratic | **fix**: `s = "${s}..."` appends in place when `s` is unique |
| 38 | small things | **explain** |
