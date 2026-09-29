# Linen Language Test - Task Implementation Notes

This document records assumptions and guesses made while implementing the 10 tasks due to ambiguities or gaps in the language documentation.

## General Assumptions

### `require` without messages
**Task 1, 4:** The docs show `require total >= 0` with no message parameter. I assumed `require` statements don't take error messages and fail silently as contract violations (not errors).

### Generic type construction
**Task 8:** Used `let stack: Stack<Int> = Stack()` to construct a generic resource. The docs don't explicitly show how to construct generics, so I inferred that bare `Stack()` works with type inference.

### Tuple destructuring in lambdas
**Task 3, 7, 9:** Used `(word, count) => ...` to destructure tuples in lambdas. The docs confirm this works: "When a lambda receives a tuple, `(a, b) =>` unpacks it."

### Descending sort order
**Task 7:** Used `.sort_by((word, count) => 0 - count)` to sort in descending order by count. The docs state `sort_by` does ascending order, and suggested `.reverse()`, but negation should also work since negative numbers sort first.

### Variable capture in lambdas
**Task 10:** Assumed lambdas capture parameters from enclosing functions (e.g., `percent` in `percent_off`). The docs state "Captured variables are copied when the lambda is created," so this should work.

## Task-Specific Uncertainties

### Task 2 (CSV)
- Assumed `json.decode` failures return an `Error` type with a `message` field when inside a `throws Error` function.
- No guidance on whether header matching should be exact (case-sensitive, order-sensitive). Assumed exact match.

### Task 5 (GitHub users)
- Assumed the GitHub API returns JSON matching the `GitHubUser` type fields exactly (login is required, name and bio are optional).
- The `web.get` function is documented to fail on status >= 400, which should handle API errors.

### Task 6 (Interop)
- Had to guess the API of the `date-fns` npm package. Used `date_fns.format(js.Date(), pattern)` based on typical JavaScript convention.
- Used `extern` to declare the untyped `left-pad` package without relying on `.d.ts` files.
- Assumed `js.Date()` creates a Date object (the docs confirm `js.*` accesses JavaScript globals).

### Task 7 (Word stats)
- Assumed `text.split_words()` splits on whitespace and punctuation (confirmed by docs: "Split on whitespace and punctuation").
- Used `count_each()` which returns `Map<T, Int>`, then `.entries()` to get sortable tuples.
- Assumed `average()` on a list of word lengths returns `Float?` (confirmed by docs, returns `none` for empty list).

### Task 8 (Stack)
- Used `resource` instead of `type` to allow mutation via the `var items` field.
- Assumed pop() on a List removes and returns the last element (confirmed by docs).

### Task 9 (Storage)
- Assumed generic constraints with multiple interfaces use `<S: Storage + Listable>` syntax (shown in docs).
- Created two implementations: MemoryStore (resource with mutable Map) and FileStore (resource with dir field).
- Assumed `resource` types can have function definitions that implement interfaces.
- The `copy_prefix` function uses `<S: Storage + Listable>` to accept only storages that support both interfaces.

### Task 10 (Pricing rules)
- Defined `PricingRule = fn(Decimal) -> Decimal` as a type alias for functions.
- Used interface-based polymorphism: `Describable` interface with implementations for `Product` and `Discount`.
- Assumed lists can hold interface types: `List<Describable>` can contain both Product and Discount values.

## Known Limitations

1. **No compiler verification:** These solutions assume correct syntax based on documentation but cannot be verified without a running Linen compiler.

2. **Simplified error handling:** Some examples (like Task 5) don't fully use `parallel` for concurrent execution because the interaction between `parallel`, `try`, and `catch` is not entirely clear from the docs.

3. **Resource initialization:** Used `FileStore` with a `dir` field assuming it can be passed in the constructor like a record, but the docs don't explicitly cover resource constructors beyond the example.

4. **Test assertions:** Used `expect` for test assertions based on the docs, but some complex scenarios (like testing that `require` fails) may not work as expected since contracts stop the program rather than throwing errors that can be caught.
