# Conduit (RealWorld) backend in lang

The [RealWorld](https://realworld-docs.netlify.app/specifications/backend/introduction/)
"Conduit" API (a Medium clone's backend) with SQLite: users and JWT
authentication, profiles and following, articles with tags, favorites,
comments, the feed, pagination. Compared with the Go reference,
[golang-gin-realworld-example-app](https://github.com/gothinkster/golang-gin-realworld-example-app).

Gaps found in the language and its library: [GAPS.md](GAPS.md).

## Build and run

```
lang build main.lang -o conduit
CONDUIT_SECRET=change-me ./conduit --port 3000 --db conduit.db
```

(or `lang run main.lang -- --port 3000`). The API is under
`http://localhost:3000/api`. `--db :memory:` keeps nothing on disk.
Without `CONDUIT_SECRET` a random JWT secret is made at start, so tokens
don't survive a restart. Passwords are stored with `crypto.hash_password`
(salted PBKDF2-SHA256, 210000 rounds).

| file | what |
|---|---|
| `main.lang` | flags, the secret, the database, `http.serve` |
| `server.lang` | the routes |
| `api.lang` | `App` (connection + secret + write lock), `ApiError` and the error middleware, JSON in and out |
| `auth.lang` | JWTs (the `jwt` package from `packages/jwt`) |
| `schema.lang` | tables and the `article_view` view |
| `users.lang`, `profiles.lang`, `articles.lang`, `comments.lang` | the endpoints |
| `tests.lang` | router tests without a network: `lang test tests.lang` |
| `tests/api-tests.sh` | the official API suites against a running server |
| `repro/` | small programs showing the gaps |

## API tests

```
tests/api-tests.sh           # hurl, then Postman (newman via npx)
tests/api-tests.sh hurl
tests/api-tests.sh postman
```

The script builds the server, starts it on an empty database and runs:

- **Hurl** (`tests/hurl/*.hurl`, copied from
  [gothinkster/realworld specs/api/hurl](https://github.com/gothinkster/realworld/tree/main/specs/api/hurl)).
  This is the official suite now: the Postman collection and
  `run-api-tests.sh` were removed from the realworld repository in 2026.
  Needs `hurl`.
- **Postman/newman**: the last full Postman collection
  (realworld commit `5cd08ae`, Feb 2026), downloaded by the script and run
  with `npx newman`.

Results (2026-09-29):

| suite | this port | Go reference (same machine) |
|---|---|---|
| hurl, 13 files / 154 requests | **13/13 files, 154/154 requests** | 0/13 files (it follows the older spec: 200 instead of 201, `""` instead of `null`, ...) |
| Postman, 86 requests / 488 assertions | **486/488 assertions**, 86/86 requests | 415/449 assertions |

The two Postman failures are one request: it expects a second article
with the same title to be refused with 409. The newer hurl suite (the
source of truth) requires the opposite ("Duplicate titles are allowed
(each gets a unique slug)"), so the port follows hurl.

Choices where the spec leaves room: tags come back sorted (both suites
accept it; Postman requires it); a title change gives a new slug; an
invalid or expired token is a 401 everywhere, also on endpoints where
auth is optional (the Go reference treats it as anonymous there); `limit`/`offset` that aren't whole numbers are a 422; passwords
need 8 characters (NIST 800-63B, as the hurl suite checks on update).

## Size

| | lines (without blanks and comments) | total |
|---|---|---|
| lang port (9 files, without tests) | 883 | 1078 |
| Go reference (without `_test.go`) | 1395 | 1737 |

The Go reference leans on gorm (no SQL); the port writes its SQL (78
lines of schema) and has no ORM.
