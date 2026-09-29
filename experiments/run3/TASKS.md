# Tasks

Write each task as a separate file named `task1.lang` … `task10.lang`.
Include `test` blocks where they make sense. Every file that is a program
has a `fn main()`.

1. **Inventory.** A product has a name, a price (money) and a stock count.
   Keep products in a list. Write a function that restocks the product with a
   given name by some amount, changing the list the caller passed in. Write a
   function that returns the names of all products with fewer than 5 items in
   stock, sorted from lowest stock to highest. Also return the average stock
   across all products as a fractional number.

2. **CSV.** Parse text like `"name,age\nAda,36\nAlan,41"` into a list of
   people. Skip lines that are malformed (wrong number of columns, age not a
   number). Fail with an error if the header line is missing or wrong.
   Test both the happy path and the error.

3. **Bank transfer.** Accounts have an id and a balance (money) and are kept
   in a map by id. Account ids and customer ids are both numbers, but must
   never be mixed up. Move an amount from one account to another. The amount
   must be positive. If an account doesn't exist, or there isn't enough money,
   fail with a clear error of a distinct kind. Show a call site that tells the
   user which of the two problems happened.

4. **Word count CLI.** A command-line tool: `wordcount <file> [--top N]
   [--min-length L]`. It reads the file and prints the N most common words
   (default 10) that have at least L letters (default 1), one per line as
   `word: count`, then the average word length. If the file doesn't exist,
   print a clear message and exit with an error.

5. **Notes HTTP service.** An HTTP service on port 8080 with
   `POST /notes` (JSON body `{"title": ..., "text": ...}`, returns the created
   note with a new numeric id) and `GET /notes/:id` (returns the note, or 404
   if it doesn't exist). Keep notes in memory; requests are handled
   concurrently. Write tests that call the handlers without a network.

6. **GitHub users.** Fetch `https://api.github.com/users/{name}` for three
   usernames at the same time and decode the JSON (`login`, `name`, `bio`;
   `name` and `bio` may be missing). Print one line per user. If one fetch
   fails, print a fallback line for that user and still show the others. The
   whole thing must give up after 5 seconds.

7. **Users in SQLite.** Read the database URL from the `DATABASE_URL`
   environment variable. Create a `users` table (id, name, age), insert three
   users, then print the users older than an age given as the first
   command-line argument. Close the connection properly.

8. **Log processing pipeline.** Read a large log file line by line. Four
   workers process lines concurrently: each line that contains `ERROR` is
   counted per service (the service name is the second space-separated word).
   Print the counts per service at the end, sorted by count, highest first.

9. **Storage.** Define a key-value storage contract with get, put and delete.
   Write two implementations: one in memory, and one that keeps each key as a
   file in a directory. Only some storages can list their keys: write a
   function that copies every key with a given prefix from one storage to
   another. It must accept a memory storage as the source and a file storage
   as the destination. Test it.

10. **Pricing rules.** A pricing rule turns an order total (money) into a new
    total. Write a function that builds a "percent off" rule and one that
    builds a "never more than X" cap rule. Combine a list of rules into a
    single rule and apply it. Separately: define two unrelated types that can
    each describe themselves, and a function that prints a report for a list
    containing both.
