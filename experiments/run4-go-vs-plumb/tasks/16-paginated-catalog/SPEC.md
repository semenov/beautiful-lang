# paginated-catalog

A read-only HTTP JSON API over a product catalog, with filters, sorting and
cursor pagination.

## Running

```
PORT=8080 CATALOG_PATH=/data/catalog.json app
```

At start, read the catalog from `CATALOG_PATH`: a JSON array of products

```
{"id": "p0001", "name": "Desk lamp", "category": "home", "price_cents": 1999, "in_stock": true}
```

`id` is a non-empty string of ASCII letters, digits, `-` and `_`, unique in
the file; `name` and `category` are strings (any Unicode); `price_cents` is a
whole number ≥ 0; `in_stock` is a boolean. If the file can't be read, is not
valid JSON, or any product breaks these rules, print a line starting with
`error:` to standard error and exit with code 1 without listening. Otherwise
listen on `127.0.0.1:$PORT`. The catalog does not change while running.

## `GET /products`

Query parameters (all optional; unknown parameters are ignored):

- `category`: only products whose category equals this exactly
  (case-sensitive; the value is URL-decoded).
- `min_price`, `max_price`: whole numbers ≥ 0 (decimal digits only), in
  cents, inclusive bounds on `price_cents`.
- `in_stock`: `true` or `false`.
- `sort`: `id` (default), `price_asc`, `price_desc` or `name`. `id` and
  `name` sort ascending by Unicode code point. Products that tie on the sort
  field are ordered by `id` ascending.
- `limit`: page size, a whole number 1 to 100 (default 20).
- `cursor`: the `next_cursor` from a previous page.

Response `200`:

```
{"items": [<product>, ...], "next_cursor": "<opaque string>" | null}
```

Each item is the product with the same five fields as in the file.
`next_cursor` is `null` exactly when no more matching products follow this
page. Requesting with the cursor and the same `category`, `min_price`,
`max_price`, `in_stock` and `sort` returns the next page; `limit` may
differ between pages. Walking all pages yields every matching product
exactly once, in order, with no repeats and no gaps. A cursor may be used
any number of times and always gives the same page.

## `GET /products/<id>`

`200` with the product, or `404 {"error": "not found"}`.

## Errors

Error responses have the body `{"error": "<message>"}`.

- `400 {"error": "invalid parameter: <name>"}` when a parameter's value is
  invalid, for example `invalid parameter: limit` for `limit=0`, `limit=101`
  or `limit=ten`; `in_stock=yes`; `sort=popular`; `min_price=-5` or
  `min_price=9.99`. If both prices are given and `min_price` is greater than
  `max_price`, the name is `max_price`.
- `400 {"error": "invalid parameter: cursor"}` for a cursor that was not
  produced by this service, or that is used with a different `category`,
  `min_price`, `max_price`, `in_stock` or `sort` than the request that
  produced it.
- `404 {"error": "not found"}` for any other path; `405 {"error": "method
  not allowed"}` for a method other than GET on `/products` or
  `/products/<id>`.

## Example

```
$ curl -s 'localhost:8080/products?category=home&sort=price_asc&limit=2'
{"items": [{"id": "p0007", "name": "Mug", "category": "home", "price_cents": 450, "in_stock": true},
           {"id": "p0001", "name": "Desk lamp", "category": "home", "price_cents": 1999, "in_stock": true}],
 "next_cursor": "dG9rZW4tZXhhbXBsZQ"}
$ curl -s 'localhost:8080/products?category=home&sort=price_asc&cursor=dG9rZW4tZXhhbXBsZQ'
{"items": [{"id": "p0003", "name": "Rug", "category": "home", "price_cents": 8900, "in_stock": false}],
 "next_cursor": null}
```
