# Linen

*Code that reads like a note to a colleague.*

Linen is a small, statically typed language. Its code reads like plain prose.
Blocks come from indentation. Calls read as sentences. Almost all syntax is
made of words, not symbols. Every construct has exactly one way to be written,
which makes Linen easy for people to read and easy for AI agents to write
without mistakes.

```
-- Reduce a total by a discount, never below zero.
to apply discount Discount to total Number gives Number
  needs total is at least 0
  when discount
    is NoDiscount
      give total
    is Percent rate r
      give total - total * r / 100
    is Fixed amount a
      give 0 if a is above total else total - a

check apply (Percent rate 10) to 200 is 180
check apply (Fixed amount 50) to 30 is 0
```

There are no colons, semicolons, commas, dots, curly-brace blocks, `=` or `==`,
and no `&&`, `->` or `?`.

---

## Principles

1. **Words over symbols.** Symbols are kept only where everyone already knows
   them: arithmetic, quotes and brackets. Everything else is a word.
2. **Sentences as signatures.** A function is declared the same way it is
   called: `move book from shelf to box`.
3. **At most one unlabeled argument.** Every other argument is named by a
   preposition, so arguments can't be silently swapped.
4. **One way to write it.** The canonical format is part of the language
   (`linen fmt` is not optional). There is nothing to argue about in review.
5. **Local truth.** No null, no exceptions, no implicit conversions, no
   operator overloading, no inheritance, no macros. What you see on the line
   is what happens.
6. **Intent lives in the code.** Contracts (`needs`, `promises`), tests
   (`check`) and doc comments sit next to the code they describe.

---

## A tour

### Names and values

```
let name be "Ada"                 -- cannot change
var count be 0                    -- can change
set count to count + 1

let limit Number be 10            -- the type is optional where it can be inferred
let ready be yes                  -- booleans are yes and no
let primes be [2 3 5 7 11]        -- list items are separated by spaces
let greeting be "Hello, {name}"   -- interpolation uses {…} inside strings
```

Names are `kebab-case`: `total-price`, `read-file`. Types and constructors are
`Capitalized`. Because a hyphen inside a word is part of the name, subtraction
always has spaces around it: `a - b`.

### Functions are sentences

A definition starts with `to`, followed by the verb, an optional unlabeled
parameter, and any number of parameters labeled with prepositions.

```
to greet person Text
  say "Hello, {person}"

to move item Book from source Shelf to target Box
  ...

to area of shape Shape gives Number
  ...
```

Call it the way you'd say it:

```
greet "Ada"
move war-and-peace from top-shelf to box
say area of circle
```

Labels come from a fixed set of prepositions:
`of to from with by at in into on for as over than until via per`.
Because the set is fixed, a reader always knows which words are labels.

By convention, noun-like functions take `of` (`size of list`, `sum of prices`)
and verb-like functions take their object directly (`sort list`, `say text`).

### The one rule of arguments

> **An argument is a single term: a literal, a name, a possessive like
> `order's total`, a list `[...]`, a constructor, a nested call, or anything in
> parentheses.**

A call's arguments nest to the right, and each label attaches to the nearest
function that accepts it:

```
say size of items                      -- say (size of items)
split lowercase text by " "            -- split (lowercase text) by " "
reverse sort scores by (given s give s's points)
```

Operators bind looser than calls, so the obvious reading is always the right
one:

```
if size of items is above 3            -- (size of items) is above 3
let n be size of items + 1             -- (size of items) + 1
move (first of books) to shelf         -- a bigger argument? use parentheses or a name
```

### Possessives instead of dots

```
say order's customer's email
set account's balance to account's balance - fee
```

### Records and choices

```
type Point has
  x Number
  y Number

type Shape is one of
  Circle radius Number
  Rectangle width Number height Number
```

A capitalized word starts a constructor. After it, field names and values
alternate:

```
let origin be Point x 0 y 0
let box be Rectangle width 3 height 4

let ada be User          -- the long form: one field per line
  name "Ada Lovelace"
  email "ada@example.com"
  born 1815

let moved be origin with x 5   -- a copy of origin with a different x
```

Generic types use `of`: `List of Text`, `Map from Text to Number`,
`Text or nothing`.

### Deciding: `if` and `when`

```
if age is at least 18
  say "welcome"
else if has-guardian
  say "welcome, with company"
else
  say "sorry"
```

Comparisons are words: `is`, `is not`, `is above`, `is below`,
`is at least`, `is at most`, `is in`, `is not in`.

`when` checks one value against a list of `is` clauses. A clause can be a
shape to match against or a comparison:

```
to area of shape Shape gives Number
  when shape
    is Circle radius r
      give pi * r ^ 2
    is Rectangle width w height h
      give w * h

to feeling of degrees Number gives Text
  when degrees
    is below 0
      give "freezing"
    is below 20
      give "cool"
    else
      give "warm"
```

The compiler checks that `when` covers every case.

For a one-line choice: `give "adult" if age is at least 18 else "child"`.

### Repeating

```
for book in library
  say book's title

for n from 1 to 10 by 2
  say n

repeat 3 times
  say "hip hip hooray"

while fuel is above 0
  set fuel to fuel - burn-rate
```

### Collections with `each`

A single expression covers what other languages do with `map`, `filter` and
list comprehensions:

```
let adults be each person in people where person's age is at least 18
let names be each person in people give person's name
let total be sum of (each item in cart give item's price * item's quantity)
```

An anonymous function uses `given`:

```
sort people by (given p give p's age)
```

The same word, `give`, produces a value everywhere: from a function, from a
`given`, and from each step of `each`.

### Nothing and failure

Linen has no null and no exceptions. A value that may be missing has type
`Text or nothing`. A function that may fail says so in its signature:

```
to load-settings from path Text gives Settings may fail
  let text be try read-file path          -- if this fails, fail too
  let port be parse-number text else 8080 -- or use a fallback
  if port is below 1
    fail "port must be positive, got {port}"
  give Settings port port
```

- `try` passes a failure up to the caller.
- `else` gives a fallback, both for failures and for `nothing`:
  `let nick be user's nickname else user's name`.
- `attempt … recover` handles a whole block:

```
attempt
  let settings be try load-settings from "app.conf"
  start-server with settings
recover problem
  say "could not start: {problem's message}"
```

The compiler won't let you ignore a call that `may fail`.

### Contracts and checks

```
-- Take money out of an account.
to withdraw amount Number from account Account gives Account
  needs amount is above 0
  needs amount is at most account's balance
  promises result's balance is at least 0
  give account with balance (account's balance - amount)

check withdraw 30 from (Account balance 100) is Account balance 70
```

`needs` is checked when the function is called, `promises` when it returns
(`result` is the value being returned). `linen check` runs every `check` in the
project. Checks double as usage examples for both people and agents.

### Doing things together

```
together
  let paris be try forecast for "Paris"
  let tokyo be try forecast for "Tokyo"
say "{paris's degrees}° in Paris, {tokyo's degrees}° in Tokyo"
```

Everything inside `together` runs at the same time. The block ends when all of
it has finished, and its names stay visible afterwards. If any part fails, the
rest is cancelled.

### Modules

```
use web
use text-tools

public type Invoice has ...
public to total of invoice Invoice gives Number ...
```

Every file is a module. Definitions are private unless marked `public`.
Statements at the top level of the main file make up the program.

---

## The whole alphabet

**Punctuation**, and nothing else:

| Symbol          | Meaning                               |
|-----------------|---------------------------------------|
| `"…"`           | text, with `{…}` for interpolation    |
| `'s`            | possessive: field access              |
| `( )`           | grouping                              |
| `[ ]`           | lists                                 |
| `+ - * / ^`     | arithmetic                            |
| `--`            | comment                               |
| `-` inside word | part of a name: `read-file`           |

**Keywords** (about 40):

```
to  type  has  is  one  of  use  public
let  var  set  be  give  gives  given  each  where
if  else  when  for  in  while  repeat  times
try  fail  may  attempt  recover  together
needs  promises  check  result
and  or  not  mod  above  below  least  most
yes  no  nothing
```

**Prepositions** used as labels:
`of to from with by at in into on for as over than until via per`

---

## Grammar sketch

```
file        = { definition | statement }
definition  = ["public"] "to" verb [param] { label param } ["gives" type] ["may" "fail"] block
            | ["public"] "type" Name ["of" Name] ( "has" fields | "is" "one" "of" variants )
param       = name [type]
statement   = "let" name [type] "be" expr
            | "var" name [type] "be" expr
            | "set" place "to" expr
            | "give" expr | "fail" expr
            | "if" expr block { "else" "if" expr block } ["else" block]
            | "when" expr INDENT { "is" pattern block } ["else" block] DEDENT
            | "for" name ( "in" expr | "from" expr "to" expr ["by" expr] ) block
            | "while" expr block | "repeat" expr "times" block
            | "attempt" block "recover" name block
            | "together" block
            | "needs" expr | "promises" expr | "check" expr
            | expr
expr        = choice [ "else" expr ]                       -- fallback
choice      = logic [ "if" logic "else" expr ]
logic       = compare { "and" compare } | compare { "or" compare } | "not" logic
compare     = sum [ "is" ["not"] ( relation sum | pattern ) ]
relation    = "above" | "below" | "at" "least" | "at" "most" | "in" | ε
sum         = product { ("+" | "-") product }
product     = power { ("*" | "/" | "mod") power }
power       = call [ "^" power ]
call        = verb [arg] { label arg } | Name { field arg } | "try" call | term
arg         = call | term
term        = literal | name | term "'s" name | "(" expr ")" | "[" { term } "]"
            | "given" name { name } "give" expr
            | "each" name "in" expr ["where" expr] ["give" expr]
block       = NEWLINE INDENT { statement NEWLINE } DEDENT
```

Mixing `and` with `or` requires parentheses, so nobody has to remember which
one binds tighter.

---

## Why agents like it

- **Small, closed vocabulary.** A few dozen keywords, a fixed set of labels and
  one canonical format. There's very little to guess.
- **Sentence-shaped calls.** Labeled arguments make call sites explain
  themselves, so misplaced arguments are rare and easy to spot.
- **Line-local edits.** One statement per line and indentation for structure.
  A diff touches exactly the lines that changed meaning.
- **Everything is explicit.** Failure is in the signature, missing values are
  in the type, and returns say `give`. There's no hidden control flow.
- **Specs next to code.** `needs`, `promises` and `check` give an agent a spec
  to work to and a fast way to test it.
- **Errors that suggest fixes.** Every compiler error includes a concrete fix,
  for example `say a + b` → *"say takes one term; did you mean `say (a + b)`?"*

---

See [`examples/`](examples) for complete programs.
