#!/usr/bin/env python3
"""Makes the benchmark inputs in /tmp/lt: big.json (53 MB), big.stream (one
user per line), med5000.json. Then: gron -m big.json > go.gron,
gron -m med5000.json > med5000.gron (with the Go version)."""
import json, random, os
os.makedirs('/tmp/lt', exist_ok=True)
R = random.Random(7)
def user(i):
    return {"id": i, "name": f"User {i}", "email": f"user{i}@example.com", "active": R.random() < .5,
            "score": round(R.uniform(-1000, 1000), 3),
            "tags": [R.choice(["a", "b", "ç", "日本", "x y"]) for _ in range(R.randint(0, 5))],
            "address": {"street": f"{R.randint(1, 999)} Main St", "city": R.choice(["Leeds", "Zürich", "Москва", "東京"]),
                        "geo": {"lat": R.uniform(-90, 90), "lng": R.uniform(-180, 180)}},
            "friends": [{"id": R.randint(0, 10**6), "since": "2020-01-0%d" % R.randint(1, 9)} for _ in range(R.randint(0, 4))],
            "meta": None if R.random() < .3 else {"note": "line\nbreak \"quoted\" \\ tab\t", "big": 12345678901234567890, "e": 1.5e-7}}
users = [user(i) for i in range(130000)]
json.dump({"users": users, "count": len(users)}, open('/tmp/lt/big.json', 'w'), ensure_ascii=False)
json.dump({"users": users[:5000], "count": 5000}, open('/tmp/lt/med5000.json', 'w'), ensure_ascii=False)
with open('/tmp/lt/big.stream', 'w') as f:
    for u in users:
        f.write(json.dumps(u, ensure_ascii=False) + '\n')
