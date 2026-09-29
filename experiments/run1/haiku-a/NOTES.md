# Implementation Notes: Linen Test Programs

This document lists assumptions and guesses made during implementation due to missing or ambiguous documentation.

## Cross-task Assumptions

### Collections and Methods
- `.is_empty()` - method on List<T> to check if list is empty
- `.map()` - method on List<T> for transformation
- `.filter()` - method on List<T> for filtering
- `.sort_by()` - method on List<T> taking a lambda for sorting
- `.reverse()` - method on List<T> for reversing order
- `.take(n)` - method on List<T> for taking first n elements
- `.drop(n)` - method on List<T> for removing first n elements
- `.sum()` - method on List<T> for summing; assumed to work with lambdas
- `.length` - property on List<T> for list size
- Indexing with `[i]` - assumed to work for lists
- `+` operator - assumed to concatenate lists
- `+=` operator - assumed to append to mutable list variables

### String Methods
- `.split(sep)` - split string by separator
- `.lower()` - convert to lowercase
- `.length` - length of string
- `.contains(s)` - check if contains substring
- `.to_string()` - convert values to Text (assumed on Int)

### Type Construction and Copying
- `.with(field: value)` - copy record with one field changed (from documentation)
- Record/enum construction - follows pattern from docs
- Generic types - `Stack<T>`, `List<T>`, etc. use angle brackets

### Error and Optional Handling
- `some(value)` - wrap value in optional
- `none` - represents no value
- `is some(pattern)` - pattern matching for optionals
- `try/catch` - error handling with automatic propagation
- `throw Error(message)` - throw error with message

## Task-Specific Guesses

### Task 1: Inventory
- No special guesses; uses documented patterns

### Task 2: CSV Parsing
- `parse_int()` - assumed function to parse string to Int (not documented)
- `var` keyword used for mutable list accumulation
- List iteration with `for i in 1..length` uses inclusive range
- Empty line skipping with `continue`

### Task 3: Shapes
- `3.14159` used as approximation for PI (no PI constant documented)
- `^` operator for exponentiation (shown in example but not explicitly documented)
- `.sum()` method can be called after `.map()` to sum results

### Task 4: Bank Transfer
- Tuple return type `(Account, Account)` - assumed syntax
- `ensure` postcondition with `result` variable (from documentation)
- `result[0]` and `result[1]` indexing on tuple (assumed)

### Task 5: GitHub Users
- `fetch()` function - assumed available from node-fetch or global (not shown in syntax overview)
- `.json()` method on response object - JavaScript interop (not explicitly shown)
- `parallel` block with `is some(u)` pattern matching for optional handling

### Task 6: Interop
- `Date()` constructor - assumed to create today's date (JavaScript class constructor)
- `date_fns.format()` function signature - from npm package, types assumed correct
- `Float()` constructor for type conversion
- `.contains()` method on Text type

### Task 7: Word Stats
- `group_by()` return value structure - assumed to work with `.map()` using pair-like access `pair[0]` and `pair[1]`
- Word filtering logic - `w != ""` to skip empty strings
- `Float()` constructor for numeric type conversion

### Task 8: Stack
- Generic function syntax with `<T>` type parameters
- `some()` and `none` as explicit value constructors
- Tuple return from `pop()` - `(T?, Stack<T>)`
- Generic method calls like `create<Int>()`

## Syntax Uncertainties

1. **List Concatenation** - Used `[value] + stack.items` assuming `+` concatenates lists
2. **Mutable Accumulation** - Used `var result: List<T> = []` and `result += [item]`, assuming this works
3. **Group By Result** - Assumed `group_by()` returns something iterable/mappable that provides key-value pairs
4. **Generic Syntax** - Assumed `<T>` works for both type definitions and function calls
5. **Error Equality** - In tests comparing error messages, assumed `.message` property on Error type
6. **Operator Precedence** - Assumed standard precedence for arithmetic and comparison operators

## Missing/Unclear Features

- How to properly handle list accumulation in functional style (if `+=` doesn't work)
- Exact signature of `group_by()` result type
- Whether string methods like `.lower()`, `.contains()` exist or need importing
- Exact API for npm package interop beyond the examples given
- How to import and use standard functions like `parse_int()`
