function make(d) { return d === 0 ? { l: null, r: null } : { l: make(d - 1), r: make(d - 1) } }
function check(t) { return t.l === null ? 1 : 1 + check(t.l) + check(t.r) }
const MAX = 20
console.log(`stretch ${check(make(MAX + 1))}`)
const long = make(MAX)
for (let d = 4; d <= MAX; d += 2) {
  const iters = 1 << (MAX - d + 4)
  let total = 0
  for (let i = 0; i < iters; i++) total += check(make(d))
  console.log(`${iters} trees of depth ${d}: ${total}`)
}
console.log(`long lived: ${check(long)}`)
