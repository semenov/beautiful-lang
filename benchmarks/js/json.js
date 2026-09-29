let x = 2463534242
function rnd() { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x }
const N = 300_000, ROUNDS = 10
const users = []
for (let i = 0; i < N; i++)
  users.push({ id: i, name: "user" + i, score: rnd() % 1000, active: i % 3 === 0, tags: ["alpha", "beta"] })
let total = 0, length = 0
for (let r = 0; r < ROUNDS; r++) {
  const text = JSON.stringify(users)
  length = text.length
  for (const u of JSON.parse(text)) if (u.active) total += u.score
}
console.log(length, total)
