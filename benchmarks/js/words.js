let x = 2463534242
function rnd() { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x }
const VOCAB = 50_000, N = 5_000_000
const L = "abcdefghijklmnopqrstuvwxyz"
function word(k) { let s = ""; do { s += L[k % 26]; k = Math.floor(k / 26) } while (k > 0); return s }
const vocab = []
for (let k = 0; k < VOCAB; k++) vocab.push(word(k))
for (let r = 0; r < 5; r++) {
  const parts = new Array(N)
  for (let i = 0; i < N; i++) parts[i] = vocab[rnd() % VOCAB]
  const text = parts.join(" ")
  const counts = new Map()
  for (const w of text.split(" ")) counts.set(w, (counts.get(w) ?? 0) + 1)
  const top = [...counts]
    .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
    .slice(0, 5)
  console.log(text.length)
  for (const [w, c] of top) console.log(w, c)
}
