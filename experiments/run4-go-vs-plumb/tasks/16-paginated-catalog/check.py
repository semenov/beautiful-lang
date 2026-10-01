import sys, os, json, random
from urllib.parse import urlencode, quote
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "_lib"))
from checklib import Checks, free_port

c = Checks()

rnd = random.Random(16)
CATS = ["books", "toys", "Café & Bar", "garden", "Books"]
NAMES = ["Lamp", "lamp", "Äpfel", "Zebra toy", "apple", "Ölkanne", "Écran", "mug", "Mug", "日本茶", "Beer 'IPA'", "chair"]
products = []
for i in range(1, 2001):
    products.append({"id": f"p{i:04d}", "name": rnd.choice(NAMES), "category": rnd.choice(CATS),
                     "price_cents": rnd.choice([0, 99, 450, 1999, 1999, 2500, 8900, 15000, 100000]),
                     "in_stock": rnd.random() < 0.6})
products.append({"id": "Z-last_1", "name": "zz", "category": "toys", "price_cents": 1999, "in_stock": True})
products.append({"id": "A-first", "name": "aa", "category": "toys", "price_cents": 1999, "in_stock": False})
file_order = products[:]
rnd.shuffle(file_order)
cat_path = c.work / "catalog.json"
cat_path.write_text(json.dumps(file_order, ensure_ascii=False), encoding="utf-8")

s = c.server({"CATALOG_PATH": str(cat_path)})

SORTS = {"id": lambda p: p["id"], "price_asc": lambda p: (p["price_cents"], p["id"]),
         "price_desc": lambda p: (-p["price_cents"], p["id"]), "name": lambda p: (p["name"], p["id"])}


def expected(category=None, min_price=None, max_price=None, in_stock=None, sort="id"):
    r = [p for p in products if (category is None or p["category"] == category)
         and (min_price is None or p["price_cents"] >= min_price)
         and (max_price is None or p["price_cents"] <= max_price)
         and (in_stock is None or p["in_stock"] == in_stock)]
    return sorted(r, key=SORTS[sort])


def get(params):
    return s.get("/products?" + urlencode(params))


def walk(params, limits):
    """Follow cursors; limits is a list cycled through. Returns (items, pages, problem)."""
    items, cursor, pages = [], None, 0
    while True:
        q = dict(params, limit=str(limits[pages % len(limits)]))
        if cursor is not None:
            q["cursor"] = cursor
        r = get(q)
        j = r.json()
        if r.status != 200 or not isinstance(j, dict):
            return items, pages, r
        pages += 1
        items += j["items"]
        cursor = j["next_cursor"]
        if cursor is None:
            return items, pages, None
        if pages > 500:
            return items, pages, "too many pages"


def ids(xs):
    return [p["id"] for p in xs]


@c.test("bad catalog files: exit 1 with error:")
def _():
    (c.work / "bad.json").write_text("[{\"id\": \"x\",")
    dup = [products[0], dict(products[1], id=products[0]["id"])]
    (c.work / "dup.json").write_text(json.dumps(dup))
    neg = [dict(products[0], price_cents=-1)]
    (c.work / "neg.json").write_text(json.dumps(neg))
    out = []
    for f in ("missing.json", "bad.json", "dup.json", "neg.json"):
        r = c.run([], env={"PORT": str(free_port()), "CATALOG_PATH": str(c.work / f)}, timeout=10)
        out.append(r)
    return all(r.code == 1 and r.err.startswith("error:") for r in out), out


@c.test("default page: first 20 by id")
def _():
    r = s.get("/products")
    j = r.json()
    return r.status == 200 and j["items"] == expected()[:20] and isinstance(j["next_cursor"], str), r


@c.test("get one product; unknown id is 404")
def _():
    r = s.get("/products/p0042")
    r2 = s.get("/products/nope")
    want = next(p for p in products if p["id"] == "p0042")
    return r.status == 200 and r.json() == want and r2.status == 404 and r2.json() == {"error": "not found"}, (r, r2)


@c.test("walk all by id, page size 100: no repeats, no gaps, no empty last page")
def _():
    items, pages, prob = walk({}, [100])
    return prob is None and items == expected() and pages == 21, (prob, pages, len(items))


@c.test("walk price_asc with ties, page size 37")
def _():
    items, pages, prob = walk({"sort": "price_asc"}, [37])
    return prob is None and ids(items) == ids(expected(sort="price_asc")), (prob, ids(items)[:10])


@c.test("walk price_desc with ties broken by id ascending")
def _():
    items, pages, prob = walk({"sort": "price_desc"}, [64])
    return prob is None and ids(items) == ids(expected(sort="price_desc")), (prob, ids(items)[:10])


@c.test("walk name sort by code point with unicode names")
def _():
    items, pages, prob = walk({"sort": "name"}, [50])
    return prob is None and ids(items) == ids(expected(sort="name")), (prob, ids(items)[:10])


@c.test("filters combined, changing limit between pages")
def _():
    params = {"category": "Café & Bar", "min_price": "450", "max_price": "8900", "in_stock": "true", "sort": "price_desc"}
    items, pages, prob = walk(params, [7, 1, 30])
    want = expected("Café & Bar", 450, 8900, True, "price_desc")
    return prob is None and len(want) > 20 and ids(items) == ids(want), (prob, len(items), len(want))


@c.test("category is case-sensitive; in_stock=false; min=max inclusive")
def _():
    a = get({"category": "Books", "limit": "100"}).json()["items"]
    b, _, p1 = walk({"in_stock": "false", "min_price": "1999", "max_price": "1999"}, [100])
    return (all(x["category"] == "Books" for x in a) and ids(a) == ids(expected("Books")[:100])
            and p1 is None and ids(b) == ids(expected(None, 1999, 1999, False))), (len(a), len(b))


@c.test("no matches: empty items and null cursor")
def _():
    r = get({"category": "none-such"})
    r2 = get({"min_price": "100001"})
    return r.status == 200 and r.json() == {"items": [], "next_cursor": None} and r2.json() == {"items": [], "next_cursor": None}, (r, r2)


@c.test("invalid parameters are 400 with the parameter name")
def _():
    cases = [({"limit": "0"}, "limit"), ({"limit": "101"}, "limit"), ({"limit": "ten"}, "limit"),
             ({"in_stock": "yes"}, "in_stock"), ({"sort": "popular"}, "sort"), ({"min_price": "-5"}, "min_price"),
             ({"min_price": "9.99"}, "min_price"), ({"max_price": "abc"}, "max_price"),
             ({"min_price": "500", "max_price": "100"}, "max_price")]
    bad = []
    for q, name in cases:
        r = get(q)
        if not (r.status == 400 and r.json() == {"error": f"invalid parameter: {name}"}):
            bad.append((q, r))
    return not bad, bad


@c.test("garbage cursors are rejected")
def _():
    rs = [s.get("/products?cursor=" + quote(x, safe="")) for x in ("garbage", "eyJrIjpbMSwyXX0", "' OR 1=1 --", "%%%")]
    return all(r.status == 400 and r.json() == {"error": "invalid parameter: cursor"} for r in rs), rs


@c.test("cursor with different sort or filters is rejected")
def _():
    cur = get({"sort": "price_asc", "category": "toys", "limit": "5"}).json()["next_cursor"]
    rs = [get({"sort": "price_desc", "category": "toys", "cursor": cur}),
          get({"sort": "price_asc", "category": "books", "cursor": cur}),
          get({"sort": "price_asc", "cursor": cur}),
          get({"sort": "price_asc", "category": "toys", "in_stock": "true", "cursor": cur})]
    ok = get({"sort": "price_asc", "category": "toys", "limit": "5", "cursor": cur, "unknown": "x"})
    want = expected("toys", sort="price_asc")[5:10]
    return (all(r.status == 400 and r.json() == {"error": "invalid parameter: cursor"} for r in rs)
            and ok.status == 200 and ok.json()["items"] == want), (rs, ok)


@c.test("a cursor can be reused and gives the same page")
def _():
    cur = get({"sort": "name", "limit": "10"}).json()["next_cursor"]
    a, b = get({"sort": "name", "limit": "10", "cursor": cur}), get({"sort": "name", "limit": "10", "cursor": cur})
    return a.status == 200 and a.body and a.json() == b.json() and ids(a.json()["items"]) == ids(expected(sort="name")[10:20]), (a, b)


@c.test("404 for unknown paths, 405 for other methods")
def _():
    r404 = [s.get("/"), s.get("/product"), s.get("/products/p0001/x")]
    r405 = [s.post("/products", {}), s.req("DELETE", "/products/p0001")]
    return (all(r.status == 404 and r.json() == {"error": "not found"} for r in r404)
            and all(r.status == 405 and r.json() == {"error": "method not allowed"} for r in r405)), (r404, r405)


c.finish()
