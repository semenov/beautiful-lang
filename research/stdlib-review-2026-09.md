# The standard library against Go and Node (2026-09-29)

A review of lang's standard library against what Go's standard library and
Node (with its most used npm packages) give programmers for backends and
command-line tools. Method: every module's `lang doc`, the guide, DESIGN.md
(the stdlib decisions: protocol clients, YAML, Markdown, JWT and so on are
packages on purpose), TODO.md and `research/npm-top-packages.md` (already
acted on: globs, `term`, dates and zones, `Decimal`, middleware, cookies,
forms, WebSockets, templates, archives, `.env`, log levels and fields,
signals, RSA/ECDSA). Where unsure, a small program was run (in /tmp).
Items already open in TODO.md are not re-reported, except where noted.

**Status:** first version; notes are being added area by area.

## Summary

The library is broad for its age: a typical CLI tool or JSON service can be
written without packages. The gaps that remain are mostly *depth* in modules
that exist, not missing modules. The weakest places:

1. **`regex`** is POSIX ERE: no `(?:...)`, no lazy `*?`, no named groups, no
   `\b`, and `\p{L}` compiles but silently matches nothing. Agents and
   people write RE2/JavaScript syntax by reflex.
2. **`crypto`** has no symmetric encryption (AES-GCM), no SHA-512 / HMAC-512,
   no HKDF, no streaming hash, no key generation.
3. **`process` and the program's own environment**: no current directory,
   home/config directories, pid, hostname; children can't inherit the
   terminal (no running `$EDITOR`, `git commit`, a pager), can't be signalled,
   and binary output doesn't fit `Output.stdout: String`.
4. **`time.DateTime`** has whole seconds only (fractions in ISO text are
   dropped) and no difference between two moments as a `Duration`.
5. **HTTP server structure**: no route groups / mounted sub-routers with
   their own middleware, no request-scoped values from middleware to the
   handler, no response compression, no streaming request bodies, no TLS.
6. **`files`**: no permissions change (`chmod +x`), symlinks, atomic write,
   temp file, file locks, watching.

## Gaps by importance

(see the table below; filled in as the review proceeds)
