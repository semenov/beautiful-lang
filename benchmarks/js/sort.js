let x = 2463534242
function rnd() { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x }
const N = 3_000_000, ROUNDS = 5
for (let r = 0; r < ROUNDS; r++) {
  const items = new Array(N)
  for (let i = 0; i < N; i++) items[i] = { id: i, key: rnd() }
  items.sort((a, b) => a.key - b.key)
  console.log(items[0].key, items[N >> 1].key, items[N - 1].key)
}
