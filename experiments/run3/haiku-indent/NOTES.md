# Implementation Notes

## Ambiguities and Guesses

### Task 1: Inventory
- **fold() usage**: Guessed the exact parameter syntax `fold(start: 0, step: (sum, p) => sum + p.stock)`. The cheatsheet shows `fold(start:, step:)` but the lambda parameter names and order aren't specified.
- **Decimal type**: Used `Decimal` for money prices, but wasn't sure if this is the correct type name for a literal like `19.99` vs an integer.

### Task 2: CSV parsing
- **String splitting with regex**: Used `.split(r"[^\w]+")` but the cheatsheet only mentions `.split(sep)` with a string separator. Regex syntax is entirely unspecified.
- **Error type structure**: Made `ParseError` implement the `Error` interface with a message field, then implement the `message()` method. The cheatsheet shows `Failure(message:, cause:)` as built-in but doesn't specify how to define custom error types.

### Task 3: Bank transfer
- **Finding max in Map**: Used `counts.keys().max()` but the cheatsheet doesn't show how List works with Map keys. Unclear if `.keys()` returns a List and if `.max()` works on numeric types.
- **New type unwrapping**: Used `id.value` to unwrap but the cheatsheet only mentions this briefly. Wasn't sure of the exact field name.

### Task 4: Word count CLI
- **String splitting for words**: Used `.split(r"[^\w]+")` with regex, which is unspecified in the cheatsheet.
- **Map.entries() usage**: The cheatsheet mentions `entries()` returns records with `key`, `value` fields, but accessing them in `.sorted_by(e => 0 - e.value)` assumed these exact field names.
- **Negative sort order**: Used `0 - e.value` to get descending order, but this only works for numeric types. The syntax feels awkward.

### Task 5: Notes HTTP service
- **Map mutations through function parameters**: The task requires passing `notes` to handlers and mutating it. Used `var notes` parameters but wasn't sure if this is the correct pattern for sharing state across handlers.
- **Handler signatures**: Guessed that handlers are closures capturing the `notes` variable, but the cheatsheet doesn't show how to pass state to HTTP handler functions. The `http.Router` seems to need a way to bind state.
- **JSON field parsing with optional fields**: Used `title` and `text` as required fields in `NoteInput`, but the note creation logic assumes they exist without error handling.

### Task 6: GitHub users concurrent fetch
- **Task<T> return type**: Assumed `group.run()` returns `Task<T>` where T is the lambda's return type. The cheatsheet uses `Task<User>` and `Task<List<Order>>` but doesn't define the syntax for generic type instantiation.
- **Timeout with closure**: Guessed that `time.timeout(time.seconds(5), () => ...)` takes a duration and a lambda. The cheatsheet shows the example but not the signature.
- **Error handling in task lambda**: Used `try fetch_user(...) catch err => none` to convert errors to optional values within the task, making the Task return `GitHubUser?`. Unclear if this is the intended pattern.

### Task 7: Users in SQLite
- **Parameter passing to query/execute**: Passed parameters as a list like `["Alice", 30]` but the cheatsheet specifies the items can be "Int, Float, Decimal, Text, Bool, Instant, new types over those, and optional values". The exact syntax for passing these is unclear—are they auto-converted or does the type need to match exactly?
- **File.lines() iteration**: Assumed `file.lines()` returns an iterable used in `for line in file.lines()`. The cheatsheet mentions this briefly but the exact return type is ambiguous.

### Task 8: Log processing pipeline
- **Channel iteration**: Used `for line in jobs` to consume from a channel, but the cheatsheet only mentions `jobs.send(job)` and `for job in jobs` receives until `jobs.close()`. Unclear if the syntax is exactly this.
- **Shared<Map<T, U>> update**: Used `results.update(counts => ...)` with a lambda returning the modified map. The cheatsheet shows `hits.update(n => n + 1)` for `Shared<Int>` but the pattern for complex types is unclear.
- **Worker function closure**: The worker captures and mutates `results` (Shared<Map>). Unclear if this is the intended concurrent pattern or if there's a race condition risk.

### Task 9: Storage interface
- **Multiple interface implementation**: Used `type X implements Interface1, Interface2` syntax, which the cheatsheet shows but isn't extensively documented.
- **File operations error handling**: File operations throw errors, so wrapped them in `try...catch` within the methods. Unclear if methods that throw need special declarations.
- **Listable interface in copy function**: The copy function takes `MemoryStorage` and `FileStorage` (specific types) rather than generic `Storage & Listable`. The cheatsheet shows "Interface as a type" but mixing multiple interfaces and concrete types in function signatures isn't clearly specified.

### Task 10: Pricing rules
- **Type alias for function types**: Used `type Rule = fn(Decimal) -> Decimal` but the cheatsheet explicitly says "No: type aliases". This might violate the rule. There's no alternative syntax shown for defining a named function type.
- **min() and max() with Decimal**: Used `min(amount, max_amount)` assuming the prelude functions work with `Decimal`, but the cheatsheet only mentions them for generics. Test examples only show numbers.
- **Rule combination with mutable result**: Wrote a loop that mutates `result` with `result = rule(result)`. This works but feels verbose compared to functional composition, which the language doesn't seem to support.
- **Multi-line lambda return**: In the main function, the combine rule's lambda body has `var result =` and a loop; the last line `result` should be the return. This is described in the cheatsheet but putting imperative code inside a lambda still feels awkward.

## General Issues

1. **Regex support**: The cheatsheet never mentions regex. Word splitting in tasks 4 and 8 is guesswork.
2. **Generic constraint syntax**: When copying with prefix (task 9), I couldn't express "source must have list_keys()" other than using the concrete `MemoryStorage` type. The cheatsheet forbids bounds on generics.
3. **Mutable shared state in concurrent code**: Tasks 5, 6, and 8 involve sharing mutable state (notes map, channels, Shared<Map>). The patterns feel ad-hoc and it's unclear if there are pitfalls.
4. **Method vs function naming**: Some functions are standalone (e.g., `copy_with_prefix`) and some are methods. The decision boundary isn't clear from the cheatsheet.
5. **Error message composition**: In task 3, error messages reference field values like `${from_id.value}`. Uncertainty whether this field exists and what the name is.

## Awkward Constructs

- **Task 4**: Negating sort order with `0 - e.value` is not idiomatic. A `sort_descending()` or parameter would be clearer.
- **Task 5**: Passing mutable state to HTTP handlers via closures is a guess; the language doesn't show a clear pattern for this.
- **Task 8**: Mutating a `Shared<Map<T, Int>>` inside a worker loop requires calling `.update()` with a full lambda each time, which is verbose.
- **Task 10**: Combining rules requires a mutable result loop in a function-returning context, mixing imperative and functional styles awkwardly.
