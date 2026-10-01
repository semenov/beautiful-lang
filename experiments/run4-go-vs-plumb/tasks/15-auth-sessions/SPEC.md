# auth-sessions

An HTTP service for user accounts and login sessions, stored in SQLite.

## Running

```
PORT=8080 DB_PATH=/data/auth.db SESSION_TTL_SECONDS=3600 app
```

Listen on `127.0.0.1:$PORT`. Store users and sessions in the SQLite
database at `DB_PATH`; create the file and its tables if they don't exist.
Users and unexpired sessions survive a restart. `SESSION_TTL_SECONDS` is a
whole number ≥ 1 (default 3600 when unset). Many requests may arrive at the
same time.

## Accounts

- `username`: 3 to 32 characters, only ASCII letters, digits and `_`.
  Usernames are unique **case-insensitively** (`Alice` and `alice` are the
  same user); a user is shown with the spelling used at registration.
- `password`: 8 to 128 characters (Unicode code points), any characters.
- Passwords are never stored as given: store only a salted hash made with a
  slow password-hashing function (for example PBKDF2, bcrypt or scrypt). The
  password text must not appear anywhere in the database file.

## Endpoints

Request bodies are JSON objects. Every response with a body is JSON.

- `POST /register` `{"username": "Alice", "password": "s3cret-pass"}` →
  `201 {"username": "Alice"}`.
- `POST /login` `{"username": "alice", "password": "s3cret-pass"}` →
  `200 {"token": "<token>"}`. The username is matched case-insensitively.
  Each login creates a new session with a new token: at least 128 random
  bits, written as a string of letters and digits. A user may have many
  sessions at once.
- `GET /me` with the header `Authorization: Bearer <token>` →
  `200 {"username": "Alice"}`.
- `POST /logout` with the same header → `204`, empty body. That token stops
  working; the user's other sessions keep working.

A session expires `SESSION_TTL_SECONDS` seconds after the login that
created it; after that its token is treated as invalid.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400 {"error": "invalid JSON"}`: the body is not a JSON object.
- `400 {"error": "invalid username"}` / `400 {"error": "invalid password"}`:
  on register, the field is missing, not a string, or breaks the rules above
  (the username is checked first). On login, a missing or non-string field
  gives the same errors; the length and character rules are not checked.
- `409 {"error": "username taken"}`: register with a username that exists in
  any letter case. When several registrations of the same name race, exactly
  one succeeds.
- `401 {"error": "invalid credentials"}`: login with an unknown user or a
  wrong password. Both cases give exactly this response.
- `401 {"error": "unauthorized"}`: `/me` or `/logout` with a missing,
  malformed, unknown, logged-out or expired token.
- `404 {"error": "not found"}`: any other path.
- `405 {"error": "method not allowed"}`: a known path with another method.

## Example

```
$ curl -s -XPOST localhost:8080/register -d '{"username":"Alice","password":"s3cret-pass"}'
{"username": "Alice"}
$ curl -s -XPOST localhost:8080/login -d '{"username":"ALICE","password":"s3cret-pass"}'
{"token": "q8Xr0bW2cJ4kT9mZ1vN6yH3sD7fL5aPe"}
$ curl -s localhost:8080/me -H 'Authorization: Bearer q8Xr0bW2cJ4kT9mZ1vN6yH3sD7fL5aPe'
{"username": "Alice"}
$ curl -s -XPOST localhost:8080/login -d '{"username":"bob","password":"whatever1"}'
{"error": "invalid credentials"}
```
