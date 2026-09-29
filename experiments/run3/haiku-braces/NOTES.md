# Implementation Notes

This document outlines places where the cheatsheet was ambiguous, silent, or where I made educated guesses about language semantics.

## Task 1: Inventory

- **fold() semantics**: Used `.fold(start: 0, step: ...)` to sum stock counts. The step function receives `(accumulator, element)` - assumed this based on functional programming conventions.
- **average_stock return type**: Returns Float even though stock counts are Int. Used `.to_float()` for conversion.

## Task 2: CSV Parsing

- **continue inside catch block**: Used `try parts[1].to_int() catch err { continue }` to skip malformed lines. The cheatsheet says catch blocks can end with control flow statements (continue/break/return/throw), but it's unclear whether a statement-level `try-catch` where catch ends with `continue` actually works as intended in a loop.
- **Error when header is missing**: Threw a generic Failure instead of a specific error type (not specified in task).

## Task 3: Bank Transfer

- **New type unwrapping syntax**: Used `.value` to unwrap AccountId (e.g., `AccountId(1).value`). The cheatsheet says "Unwrap: `id.value`" but this wasn't shown in any examples.
- **Error interface implementation**: Created custom error types implementing Error with a `message()` method. Assumed this is the correct pattern.
- **Map literal syntax**: Used `{ key: value, ... }` for Map literals. The cheatsheet shows `{"ada": 36}` but my usage with type names wasn't verified.

## Task 4: Word Count CLI

- **CLI options with defaults**: Created Options struct with `top: Int = 10` and `min_length: Int = 1`. The cheatsheet mentions "Fields with defaults can be left out" but doesn't clarify how this integrates with `cli.decode<Options>()`.
- **sorted_by + reversed chain**: Used `.sorted_by(...).reversed()` to sort descending. The cheatsheet doesn't show these as chainable, but assumed functional programming conventions apply.
- **Word tokenization**: Used `.split(" ")` for tokenization, which treats any whitespace as a single space. Real word counting might need more sophisticated handling.

## Task 5: Notes HTTP Service

- **Global mutable state**: Used `var note_counter` and `var notes` at module scope as shared state between requests. This violates the "everything is a value" principle and seems antithetical to a functional language design, but it's the only way to maintain note IDs and storage without explicit database or threading constructs.
- **HTTP handler signatures**: Assumed handler functions take `http.Request` and return `http.Response`, based on the example in the cheatsheet.
- **JSON body parsing**: Used `json.decode<CreateNoteRequest>(req.body)` assuming the body is already a string.
- **Router method**: Used `router.post(path, handler)` and `router.get(path, handler)` - syntax inferred from cheatsheet example.

## Task 6: GitHub Users

- **timeout() semantics**: Used `try time.timeout(time.seconds(5), () => ...) catch err { ... }` to enforce a 5-second limit per request. However, the semantics of timeout with concurrent task.wait() calls are unclear - whether timeout applies to the wait operation or the original fetch.
- **Handling missing fields**: GitHub's API returns optional `name` and `bio` fields. Assumed these decode to `Text?` with default `none`.
- **Fallback on timeout vs fetch error**: Caught all errors (both timeout and fetch failures) and printed a fallback line. The task says "If one fetch fails, print a fallback line" but didn't specify the timeout exception type.

## Task 7: SQLite Users

- **Database URL from environment**: Used `env.get("DATABASE_URL") ?? "sqlite::memory:"` to default to in-memory SQLite if the env var is not set.
- **Table creation idempotency**: Used `create table if not exists` to avoid errors on repeated runs.
- **Parameter binding**: Assumed `conn.execute(sql, params)` and `conn.query<T>(sql, params)` take a string literal SQL and a list of parameters, as shown in the cheatsheet.
- **Type projection**: Assumed `query<User>` maps columns to User struct fields by name automatically.

## Task 8: Log Processing Pipeline

- **Concurrent shared state**: Task requires four workers to concurrently count errors per service in a shared Map. However, the cheatsheet states "Tasks share mutable state **only** through `Shared<T>`". I initially tried to pass `&counts` (a mutable reference) to worker tasks, but this likely violates task semantics (no `return`/`break`/`continue` in task lambdas).
  - **AMBIGUITY**: The implementation uses `group.start()` with `&counts` passed to each worker, but this may not work if task lambdas can't modify captured mutable references the way regular functions can. A correct implementation might require `Shared<Map<Text, Int>>` and `update()` operations, but the cheatsheet doesn't show how to update a Shared Map (only Shared<Int> example).
- **Batch splitting**: Manually split lines into 4 batches and assigned to workers. This works for demonstration but doesn't truly parallelize line reading for very large files.

## Task 9: Storage

- **Interface composition**: Used `type X implements Storage, Listable { ... }` to implement multiple interfaces. The cheatsheet shows `implements Describable, Other` syntax, so assumed comma-separated interfaces work.
- **Interface type combinations**: Used `Storage + Listable` as a function parameter type for accepting types implementing both interfaces. The cheatsheet shows this notation in the "Interface as a type" section.
- **FileStorage implementation**: Assumed `files.list(dir)` returns file names in a directory, but edge cases (special characters in keys, directory structure) are not handled.
- **Try-catch in get()**: FileStorage.get() catches file read errors and returns `none`, but the Error interface requires a `message()` method. The mismatch assumes silent error handling is acceptable here.

## Task 10: Pricing Rules

- **Function types as fields**: Used `apply: fn(Decimal) -> Decimal` as a record field to store a function. The cheatsheet lists "Function types: `fn(Int) -> Text`, `fn(T) throws -> R`, `fn()`" but doesn't clarify whether functions can be stored as record fields.
- **Closures and capture**: `percent_off(10)` returns a PricingRule with a function that captures the `percent` parameter. The cheatsheet states "Lambdas capture variables **by copy**" and "Changing a captured `var` is an error", but doesn't show examples of capturing immutable values in closures.
- **Decimal arithmetic in closures**: Used `percent.to_float() / 100.0` inside a lambda capturing `percent`. The conversion and arithmetic were inferred from type constraints.
- **List of interfaces**: Used `List<Describable>` to hold both Product and Service objects. The cheatsheet confirms this is allowed, but the implementation assumes polymorphic dispatch works correctly.

## Cross-Task Observations

- **No function overloading**: Had to use distinct function names (e.g., `percent_off` vs `cap_at`) even though they have different signatures.
- **Named arguments beyond first parameter**: Followed the rule that with 3+ parameters, all arguments after the first must be named. This affected function calls in multiple tasks.
- **No implicit type conversions**: Repeatedly used `.to_float()`, `.to_int()`, `.to_text()` where type mismatches occurred, as the cheatsheet forbids implicit conversions.
- **Error handling patterns**: Mixed use of `throws`/`try` for recoverable errors and assumptions that bugs (panics, out-of-bounds) cannot be caught. Some tasks might incorrectly use `throw` where a bug should occur, or vice versa.
