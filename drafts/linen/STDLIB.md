# Linen standard library

Everything in the **prelude** is available without an import. Modules like
`files` or `web` need `import files`.

Every function here can be called with method syntax: `xs.map(f)` is the same
as `map(xs, f)`. In the signatures, `var` marks a parameter that the function
changes. Such a function can only be called on a `var`.

---

## Prelude

| Function | Meaning |
|---|---|
| `print(value)` | Print any value, followed by a line break. |
| `PI`, `E` | Mathematical constants (`Float`). |
| `min(a, b)`, `max(a, b)` | The smaller or larger of two numbers, texts or tuples. |
| `parse_int(text) throws -> Int` | `"42"` → `42`. Fails on anything else: `try parse_int(t) catch 0`. |
| `parse_float(text) throws -> Float` | |
| `parse_decimal(text) throws -> Decimal` | |
| `Error(message)` | The standard error value, with field `message: Text`. |
| `decode<T>(value: Dynamic) throws -> T` | Check an untyped value and turn it into `T`. |

---

## Numbers: `Int`, `Float`, `Decimal`

| | |
|---|---|
| `a + b`, `a - b`, `a * b`, `a ^ b` | `Int` widens to `Float`/`Decimal` automatically. |
| `a / b` | Two `Int`s give a `Float`. `Decimal / Decimal` gives a `Decimal`. |
| `a.div(b) -> Int` | Whole-number division. |
| `a % b` | Remainder. |
| `x.abs()` | Absolute value. |
| `x.round() -> Int`, `x.floor() -> Int`, `x.ceil() -> Int` | |
| `x.round_to(places) -> Decimal` | For `Decimal`. |
| `n.to_float()`, `n.to_decimal()` | Explicit conversion. |
| `x.to_text() -> Text` | The same text as `"{x}"`. |
| `x.clamp(low: a, high: b)` | Limit to a range. |

---

## Text

`Text` is an immutable Unicode string.

| | |
|---|---|
| `t.length` | **Field** with the number of characters. |
| `t.is_empty() -> Bool` | |
| `t[i] -> Text` | One character. An index out of range is a bug that stops the program. |
| `t.slice(from: a, to: b) -> Text` | Characters from `a` up to, but not including, `b`. |
| `t.lower()`, `t.upper()`, `t.trim()` | |
| `t.split(sep) -> List<Text>` | `"a,b".split(",")` → `["a", "b"]`. |
| `t.split_words() -> List<Text>` | Split on whitespace and punctuation. |
| `t.lines() -> List<Text>` | Split on line breaks. |
| `t.contains(part)`, `t.starts_with(part)`, `t.ends_with(part)` | |
| `t.index_of(part) -> Int?` | |
| `t.replace(old: a, new: b) -> Text` | Replace all occurrences. |
| `t.repeat(n) -> Text` | |
| `t.pad_start(width: n, fill: c)`, `t.pad_end(width: n, fill: c)` | |
| `t.chars() -> List<Text>` | |
| `t.bytes() -> Bytes` | UTF-8 bytes. |
| `a + b` | Join two texts. |

---

## `List<T>`

Lists are values, so every function returns a new list. Only the ones marked
**(var)** change the list in place.

**Reading**

| | |
|---|---|
| `xs.length` | **Field.** |
| `xs.is_empty() -> Bool` | |
| `xs[i] -> T` | An index out of range is a bug. |
| `xs.get(i) -> T?` | Safe access. |
| `xs.first() -> T?`, `xs.last() -> T?` | |
| `xs.contains(x) -> Bool` | |
| `xs.index_of(x) -> Int?` | |
| `xs.find(pred) -> T?` | The first element that matches. |
| `xs.any(pred)`, `xs.all(pred)` | |
| `xs.count(pred) -> Int` | |

**Transforming**

| | |
|---|---|
| `xs.map(f)`, `xs.filter(pred)`, `xs.flat_map(f)` | |
| `xs.parallel_map(f)` | Like `map`, but every element runs as its own task. Keeps the order. |
| `xs.sort()` | Numbers, `Text` and tuples of them. |
| `xs.sort_by(key)` | Ascending by key. For descending order, add `.reverse()`. |
| `xs.reverse()` | |
| `xs.take(n)`, `xs.drop(n)` | The first `n` elements, or everything after them. |
| `xs.slice(from: a, to: b)` | Up to, but not including, `b`. |
| `xs.unique()` | |
| `xs.indexed() -> List<(Int, T)>` | `for (i, x) in xs.indexed()`. |
| `xs.zip(ys) -> List<(T, U)>` | |
| `xs.chunks(n) -> List<List<T>>` | |
| `a + b` | Join two lists. |

**Summarizing**

| | |
|---|---|
| `xs.sum()` | For numbers. `0` for an empty list. |
| `xs.min() -> T?`, `xs.max() -> T?` | |
| `xs.min_by(key) -> T?`, `xs.max_by(key) -> T?` | |
| `xs.average() -> Float?` | For numbers. `none` for an empty list. |
| `xs.fold(start: s, step: f)` | `f(acc, x)` for every element. |
| `xs.group_by(key) -> Map<K, List<T>>` | |
| `xs.count_each() -> Map<T, Int>` | How often each element occurs. |
| `xs.join(sep) -> Text` | For `List<Text>`. |
| `xs.to_set() -> Set<T>` | |

**Changing in place (var)**

| | |
|---|---|
| `xs.append(x)` | Add at the end. |
| `xs.insert(item: x, at: i)` | |
| `xs.remove_at(i) -> T` | |
| `xs.pop() -> T?` | Remove and return the last element. |
| `xs.clear()` | |
| `xs[i] = x` | |
| `xs += ys` | |

---

## `Map<K, V>`

| | |
|---|---|
| `m.length` | **Field.** |
| `m.is_empty()` | |
| `m[key] -> V?` | `none` if the key is missing. |
| `m.contains_key(key)` | |
| `m.keys() -> List<K>`, `m.values() -> List<V>` | In insertion order. |
| `m.entries() -> List<(K, V)>` | For sorting or transforming: `m.entries().sort_by((k, v) => v)`. |
| `for (key, value) in m` | |
| `m.map_values(f) -> Map<K, W>` | |
| `m.filter((k, v) => …) -> Map<K, V>` | |
| `m.with(key, value) -> Map<K, V>` | A copy with one entry changed. |
| `m[key] = value` **(var)** | |
| `m.remove(key) -> V?` **(var)** | |

## `Set<T>`

| | |
|---|---|
| `s.length`, `s.is_empty()`, `s.contains(x)` | |
| `s.union(t)`, `s.intersect(t)`, `s.minus(t)` | |
| `s.to_list()` | Sorted if `T` can be sorted. |
| `s.add(x)`, `s.remove(x)` **(var)** | |

## Optional values `T?`

| | |
|---|---|
| `x ?? fallback` | |
| `x?.field`, `x?.f()` | |
| `x is some(v)`, `x is none` | |
| `x.or_throw(message) throws -> T` | Turn a missing value into an error. |

## `Bytes`

`b.length`, `b.to_text() throws -> Text` (as UTF-8), `b.slice(from:, to:)`,
`a + b`.

---

## Modules

### `files`

| | |
|---|---|
| `files.read(path) throws -> Text` | |
| `files.read_bytes(path) throws -> Bytes` | |
| `files.write(path, text) throws` | Replaces the file. |
| `files.write_bytes(path, bytes) throws` | |
| `files.append(path, text) throws` | |
| `files.exists(path) -> Bool` | |
| `files.list(dir) throws -> List<Text>` | File names in the directory. |
| `files.delete(path) throws` | |
| `files.make_dir(path) throws` | Also creates missing parent directories. |

### `web`

| | |
|---|---|
| `web.get(url) throws -> Text` | The response body. Fails on a network error or a status of 400 and above. |
| `web.post(url, body) throws -> Text` | |
| `web.request(req: Request) throws -> Response` | Full control: method, headers, status. |

`Response` has `status: Int`, `headers: Map<Text, Text>` and `body: Text`.

### `json`

| | |
|---|---|
| `json.decode<T>(text) throws -> T` | |
| `json.encode(value) -> Text` | |
| `json.parse(text) throws -> Dynamic` | When the shape is unknown. |

### `time`

| | |
|---|---|
| `time.now() -> Time` | |
| `time.today() -> Date` | |
| `t.format(pattern) -> Text` | `"yyyy-MM-dd HH:mm"`. |
| `t.year`, `t.month`, `t.day`, `t.hour`, `t.minute` | Fields. |
| `t.add(days: n)`, `t.add(hours: n)`, … | |
| `time.sleep(seconds)` | |

When building for JavaScript, `Time` corresponds to `Date`.

### `env`

`env.get(name) -> Text?`, `env.args() -> List<Text>`,
`env.exit(code)`.

### `random`

`random.int(from: a, to: b) -> Int` (inclusive), `random.float() -> Float`,
`xs.shuffle()`, `xs.pick() -> T?`.

---

## Common guesses from other languages

The compiler suggests the right name when you use one of these.

| Instead of | Use |
|---|---|
| `push`, `add` on a list | `append` |
| `len(xs)`, `xs.size()`, `xs.count` | `xs.length` |
| `skip` | `drop` |
| `includes`, `has` | `contains` |
| `toLowerCase`, `lowercase` | `lower` |
| `str(x)`, `toString`, `to_string` | `to_text` |
| `int(x)`, `Int(x)`, `parseInt` | `parse_int` |
| `Float(x)`, `float(x)` | `to_float` |
| `reduce` | `fold` |
| `substring`, `substr` | `slice` |
| `for i in 0..n` | `0..<n` or `1..=n` |
| `x.unwrap()`, `x!` | `x ?? fallback` or `try x.or_throw("…")` |
