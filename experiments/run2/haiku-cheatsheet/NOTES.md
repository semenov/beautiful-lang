# Implementation Notes

This document lists assumptions made while implementing the tasks due to documentation gaps.

## Task 1: Inventory
No major assumptions. Straightforward use of record types and method syntax.

## Task 2: CSV
- Assumed `lines()` splits text by newline characters
- Assumed `parse_int()` throws an error on invalid input (allowing it to be caught)
- Assumed list indexing with `[i]` is supported for accessing elements

## Task 3: Shapes
- Assumed `PI` constant is globally available (shown in cheat sheet example)
- Assumed `^` is the power/exponentiation operator
- Floating point comparisons in tests use ranges to avoid precision issues

## Task 4: Bank Transfer
- Used resources instead of types for mutable Account state, based on rule about immutable records
- Assumed resource fields can be modified in-place with `field = value` syntax
- Assumed resources can be instantiated with named parameters like `Account(balance: 100.0)`

## Task 5: GitHub Users
- Assumed `web.get(url)` returns JSON text (or can be used directly as text)
- Assumed `json.decode<T>()` automatically handles optional fields as `T?`
- Assumed optional JSON fields become `Text?` types in the decoded struct
- Assumed `parallel` construct with multiple `let` statements executes them concurrently
- Assumed `catch none` works inside `parallel` to capture errors as optional values

## Task 6: Interop
- Assumed `js.Date()` returns the current date as a JavaScript Date object
- Assumed `date-fns.format()` accepts a Date and format string directly
- Assumed `left-pad` npm package signature is `(str, length, char)` for padding
- Assumed `import js "pkg"` automatically converts dashes to underscores in the module name
- Not sure if parameter names in extern declarations must match the actual JS API

## Task 7: Word Stats
- Assumed `count_each()` on a list returns `Map<T, Int>` with element counts
- Assumed `entries()` on a Map returns `List<(K, V)>` of key-value tuples
- Assumed tuple unpacking in lambdas works: `(w, c) => ...` unpacks tuple into two values
- Assumed `sum()` works on lists of Float values
- Assumed `.to_float()` converts Int to Float for division calculations

## Task 8: Stack
- Assumed `List.pop()` returns `T?` (the popped item, or none if empty)
- Assumed `List.last()` returns `T?` (the last element, or none if empty)
- Assumed generic resources can be instantiated with type parameters: `Stack<Int>()`

## Task 9: Storage
- Assumed interfaces can define multiple methods that will be automatically satisfied
- Assumed type constraints with multiple bounds work: `<S: Storage + Listable>`
- Assumed function overloading is possible (same function name, different receiver types)
- Assumed `files.read()`, `files.write()`, `files.delete()`, and `files.list()` exist in the files module
- Assumed `.remove(key)` on Map modifies in-place (destructive operation on mutable Map)

## Task 10: Pricing Rules
- Assumed function types can be stored in variables and lists: `fn(Float) -> Float`
- Assumed closures/lambdas can capture variables from enclosing scope (e.g., `percent` in percent_off)
- Used enum for heterogeneous collection instead of interface-typed list because unclear how to make a mixed list of different types that all implement an interface
- Assumed `if x is some(v)` pattern matching works for optional values
- Assumed `match` expression works with enum cases
