# Tasks

Write each task as a separate Linen file named `task1.ln` … `task8.ln`.
Include `test` blocks where they make sense.

1. **Inventory.** A product has a name, a price and a stock count. Write a
   function that restocks a product by some amount, and a function that
   returns the names of all products with fewer than 5 items in stock, sorted
   from lowest stock to highest.
2. **CSV.** Parse text like `"name,age\nAda,36\nAlan,41"` into a list of
   people. Skip lines that are malformed. Fail with an error if the header line
   is missing or wrong.
3. **Shapes.** Define circles, rectangles and triangles (base and height).
   Compute the total area of a list of shapes, and describe each shape as
   "small" (area under 10), "medium" (under 100) or "large".
4. **Bank transfer.** Move an amount from one account to another. The amount
   must be positive. If there isn't enough money, fail with a clear error.
   Show a call site that handles the failure.
5. **GitHub users.** Fetch `https://api.github.com/users/{name}` for three
   usernames at the same time and decode the JSON (login, name, bio — name and
   bio may be missing). Print one line per user. If one fetch fails, print a
   fallback line for that user and still show the others.
6. **Interop.** Use the npm package `date-fns` to format today's date as
   `yyyy-MM-dd`. Use the untyped npm package `left-pad` to pad a number to 5
   digits. Export a function to JavaScript that returns a report string using
   both.
7. **Word stats.** Given a text, return the 3 most common words longer than 3
   letters with their counts, and the average word length.
8. **Stack.** A generic stack with push, pop (returns the top item, or nothing
   when empty) and peek. Show it working with numbers and with text.
