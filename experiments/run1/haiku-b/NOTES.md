# Implementation Notes

## Assumptions Made When Documentation Was Silent

### Task 1: Inventory
- Assumed `.with()` method exists on types to create modified copies (mentioned in docs for records)
- Assumed `.filter()`, `.sort_by()`, `.map()` work on `List<T>`
- Assumed `.length` property or `.is_empty()` method on lists

### Task 2: CSV Parsing
- Assumed `Text.split(separator)` method exists to split strings
- Assumed `parse_int(text)` function exists in prelude
- Assumed array indexing with `[index]` syntax
- Assumed lists have `.is_empty()` and `.push()` methods (and these mutate or I need to reassign)
- Assumed `continue` keyword works in loops to skip to next iteration
- Assumed `throw Error(message)` constructor signature
- Used `catch` with no value on line to skip errors when parsing

### Task 3: Shapes
- Assumed `^` operator for exponentiation
- Assumed `.sum()` method on `List<Float>` to sum elements
- Assumed `3.14159` as approximation for PI (no `PI` constant mentioned)
- Assumed pattern matching with `if` guards in `match` arms

### Task 4: Bank Transfer
- Assumed `require` statement as documented works with boolean conditions
- Assumed tuple return syntax `(Type1, Type2)` for multiple returns
- Assumed `require` violations throw automatically or can be caught

### Task 5: GitHub Users
- Assumed `https.get(url)` exists in `node:https` module (as mentioned in docs for Node built-ins)
- Assumed `json.decode<Type>()` takes generic type parameter and returns decoded value
- Assumed `parallel` block allows each statement to be evaluated concurrently
- Assumed `catch` in parallel can return a default value
- Assumed `some()` and `none` are constructors for optional values (mentioned in docs)
- Assumed `.contains()` method on Text

### Task 6: Interop
- Assumed `import js "package"` syntax works as shown in docs
- Assumed `Date` constructor available from JavaScript
- Assumed `format()` method takes format string
- Assumed `pub` keyword exports functions to JavaScript as shown in docs
- Assumed `extern` block allows declaring untyped package signatures
- Assumed `nothrow` keyword indicates function won't throw

### Task 7: Word Statistics
- Assumed `.lower()` method converts Text to lowercase
- Assumed `.group_by(lambda)` returns Map or similar with `.key` and `.value` fields
- Assumed `.reverse()` method reverses lists
- Assumed `.take(n)` returns first n elements
- Assumed integer division for average (might truncate or convert to Float)
- Assumed `Map.key` and `Map.value` field access or entry structure

### Task 8: Generic Stack
- Assumed generic syntax `Stack<T>` and function `func<T>()` declarations
- Assumed `.take(index)` returns elements from 0 to index-1 (like slice)
- Assumed tuple unpacking in `let (var1, var2) = func()` syntax
- Assumed `.length` property on lists for accessing size
- Assumed `-1` indexing or arithmetic works

## Language Features Used

- Pattern matching with `match`/`case`
- Optional types with `T?` and `some()`/`none`
- Generics with angle brackets `<T>`
- Enums with constructor arguments
- Types (records) with fields
- `pub` for exports
- `throws` and `try`/`catch` for error handling
- `parallel` blocks
- List methods: `.filter()`, `.map()`, `.sort_by()`, `.reverse()`, `.take()`, `.sum()`, `.group_by()`, `.is_empty()`
- String methods: `.split()`, `.lower()`, `.contains()`, `.length`
- Lambdas with `=>` syntax
- Immutable `let` and mutable `var` bindings
- String interpolation with `{}`
- Type annotations and generics
- `require` contracts
- `test` blocks with `expect`
