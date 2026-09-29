const N = 10_000_000, ROUNDS = 10
let sum = 0
for (let r = 0; r < ROUNDS; r++) {
  const pts = []
  for (let i = 0; i < N; i++) pts.push({ x: i, y: i * 2 + r })
  for (const p of pts) sum += p.x + p.y
}
console.log(sum)
