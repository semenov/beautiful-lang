# Task Implementation Notes

## Guesses and Assumptions

### Task 1 (Inventory)
- Assumed `var` parameters can be reassigned with `.with()` method: `product = product.with(stock: ...)`
- The documentation shows `.with()` usage but not explicitly for `var` parameters

### Task 2 (CSV)
- Used `split(",")` for CSV parsing - documentation shows `split(sep)` but not CSV specifically
- Assumed malformed lines (wrong number of columns) should be silently skipped in the loop
- Assumed the header check should throw an error immediately if wrong format

### Task 3 (Shapes)
- Used `PI` constant from prelude (mentioned in STDLIB.md)
- Applied standard geometric formulas (circle: πr², rectangle: w*h, triangle: b*h/2)
- Used exponentiation operator `^` for squaring

### Task 4 (Bank transfer)
- Assumed `require` assertions generate errors with the message containing the condition text
- Used string interpolation in error messages to include variable values
- Assumed error messages from failed `require` can be checked with `.contains()`

### Task 5 (GitHub users)
- Struggled with `parallel` limitations: it doesn't allow `return` statements or multi-statement branches
- Workaround: used `catch` with inline `return none` to handle failures per-branch
- Assumed `json.decode<GitHubUser>` handles optional fields (name, bio) and makes them `Text?`
- The parallel block syntax for variable scoping was unclear - assumed all branches complete before using their results

### Task 6 (Interop)
- Used `import js` syntax with "package-name" and `as` alias for npm packages
- Used `extern js "left-pad"` block for untyped package with manual type signature
- Assumed `nothrow` keyword exists and marks functions that won't throw (not documented explicitly)
- Built date string manually from components since date-fns formatting wasn't type-checked
- Assumed time fields (year, month, day) are `Int` type

### Task 7 (Word stats)
- Assumed `count_each()` returns `Map<T, Int>` for word frequencies
- Used negation trick `(word, count) => 0 - count` for descending sort since no `sort_by_descending` appears to exist
- Used `.entries()` on Map to get tuples for sorting by value
- Assumed division of integers gives Float: `total_length / words.length`
- Used conditional in return to create optional average: `some(...)` vs `none`

### Task 8 (Stack)
- Generic stack with type parameter `Stack<T>`
- Assumed `items.pop()` on List<T> removes and returns the last element (documented in STDLIB)
- Assumed `items.append(x)` mutates the list in place (requires `var` parameter)
- Used optional return types for `pop` and `peek` to handle empty stack

### Task 9 (Storage)
- Defined two interfaces: `Storage` (get, put, delete) and `Listable` (keys with prefix)
- Implemented `MemoryStore` as a resource with a mutable Map field
- Assumed resource fields can be accessed and mutated directly
- Implemented function overloading by first parameter type for get/put/delete/keys
- Used constraint syntax `<S: Storage + Listable>` for functions requiring both interfaces
- Assumed `files.read_bytes()` returns Bytes and `files.exists()` checks file existence
- Error handling with custom error enum `StorageError`

### Task 10 (Pricing rules)
- Defined `PricingRule` as a type alias for function type `fn(Decimal) -> Decimal`
- Created factory functions that return PricingRule values
- Used `var result` in `combine_rules` to fold rules sequentially
- Defined separate types `Book` and `Movie` implementing the `Describable` interface
- Assumed interface values in `List<Describable>` allow calling interface methods directly

## Language Features Confirmed Working
- Pattern matching with `match/case`
- Optional types with `?` and operators `??`, `?.`, `is some()`
- Named parameters for functions with 3+ parameters
- Generic functions and types with `<T>`
- Interfaces as implicit structural types
- Type-based method dispatch and overloading
- Enum variants with associated data
- Error handling with `throws`, `try`, `catch`
- Record construction with named fields
- List/Map comprehension via method chaining
- For loops over lists, ranges, and Map entries
- String interpolation with `{}`
- Comments with `#`

## Uncertain Areas
- The exact error message format from failed `require` statements
- Whether `var` parameters to functions genuinely mutate the original or if mutation is local
- The exact behavior of `parallel` blocks with error propagation
- Whether `nothrow` is the correct keyword for marking safe foreign functions
- The exact scoping rules when combining Storage and Listable constraints
