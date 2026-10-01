# Run 4: Go vs Plumb, live status

Updated 2026-10-01T23:23:56. 25 tasks × 2 languages: 93 checked, 0 awaiting checks, 7 being written, 0 not started.

## Totals (checked cells only)

| | tasks | hidden tests passed, first compiled version | final version | all tests pass | build attempts (failed) | builds until first success | lines of code |
|---|---|---|---|---|---|---|---|
| Go (Sonnet) | 25 | 438/438 | 438/438 | 25/25 | 28 (0) | 25 | 7278 |
| Plumb (Sonnet) | 25 | 429/438 | 438/438 | 25/25 | 87 (45) | 69 | 6109 |
| Go (Haiku) | 22 | 293/399 | 344/399 | 4/22 | 112 (19) | 38 | 6151 |
| Plumb (Haiku) | 21 | 255/383 | 316/383 | 2/21 | 191 (105) | 112 | 4922 |

## Per task

`✍️` writing, `🔨` last build ok, `🔴` last build failed, `⏳` done, waiting for checks, `✅` checked. `b7/2✗` 7 builds, 2 failed; `120L` lines of code; `1st 9/12` hidden tests passed by the first version that compiled; `fin 12/12` by the final one.

| task | Go (Sonnet) | Plumb (Sonnet) | Go (Haiku) | Plumb (Haiku) |
|---|---|---|---|---|
| 01-wordfreq | ✅ b1 178L 1st 18/18 fin 18/18 | ✅ b4/3✗ 122L 1st 18/18 fin 18/18 | ✅ b2 129L 1st 14/18 fin 14/18 | ✅ b4/1✗ 133L 1st 17/18 fin 18/18 |
| 02-csv-report | ✅ b1 275L 1st 23/23 fin 23/23 | ✅ b1 274L 1st 23/23 fin 23/23 | ✅ b10/1✗ 222L 1st 16/23 fin 21/23 | ✅ b8/6✗ 340L 1st 8/23 fin 8/23 |
| 03-log-stats | ✅ b1 267L 1st 18/18 fin 18/18 | ✅ b2/1✗ 209L 1st 18/18 fin 18/18 | ✅ b5/1✗ 134L 1st 13/18 fin 17/18 | ✅ b21/10✗ 232L 1st 2/18 fin 14/18 |
| 04-json-query | ✅ b1 513L 1st 22/22 fin 22/22 | ✅ b4/3✗ 417L 1st 22/22 fin 22/22 | ✅ b5 279L 1st 12/22 fin 15/22 | ✅ b12/9✗ 237L 1st 21/22 fin 21/22 |
| 05-config-merge | ✅ b1 526L 1st 22/22 fin 22/22 | ✅ b2 441L 1st 22/22 fin 22/22 | ✅ b3 284L 1st 12/22 fin 13/22 | ✅ b13/10✗ 260L 1st 18/22 fin 19/22 |
| 06-line-diff | ✅ b1 184L 1st 24/24 fin 24/24 | ✅ b5/4✗ 227L 1st 24/24 fin 24/24 | ✅ b6/2✗ 288L 1st 20/24 fin 22/24 | ✅ b9/3✗ 281L 1st 3/24 fin 22/24 |
| 07-money-split | ✅ b1 237L 1st 17/17 fin 17/17 | ✅ b1 269L 1st 17/17 fin 17/17 | ✅ b4/1✗ 288L 1st 17/17 fin 17/17 | ✅ b9/5✗ 198L 1st 5/17 fin 13/17 |
| 08-dup-finder | ✅ b2 228L 1st 24/24 fin 24/24 | ✅ b1 183L 1st 24/24 fin 24/24 | ✅ b4/1✗ 155L 1st 23/24 fin 24/24 | ✅ b12/5✗ 149L 1st 18/24 fin 22/24 |
| 09-todo-sqlite | ✅ b1 451L 1st 16/16 fin 16/16 | ✅ b5/4✗ 309L 1st 16/16 fin 16/16 | ✅ b7/1✗ 406L 1st 2/16 fin 14/16 | ✅ b11/7✗ 380L 1st 13/16 fin 15/16 |
| 10-business-days | ✅ b3 262L 1st 17/17 fin 17/17 | ✅ b3/1✗ 240L 1st 10/17 fin 17/17 | ✅ b2 248L 1st 16/17 fin 16/17 | ✅ b8/5✗ 236L 1st 14/17 fin 14/17 |
| 11-parallel-fetch | ✅ b1 269L 1st 21/21 fin 21/21 | ✅ b3/2✗ 221L 1st 21/21 fin 21/21 | ✅ b6/1✗ 201L 1st 14/21 fin 17/21 | ✅ b7/2✗ 130L 1st 17/21 fin 20/21 |
| 12-batch-rename | ✅ b1 322L 1st 18/18 fin 18/18 | ✅ b4/3✗ 365L 1st 18/18 fin 18/18 | ✅ b6 363L 1st 2/18 fin 14/18 | ✅ b10/8✗ 288L 1st 15/18 fin 15/18 |
| 13-markdown-toc | ✅ b1 197L 1st 18/18 fin 18/18 | ✅ b1 306L 1st 18/18 fin 18/18 | ✅ b6/1✗ 248L 1st 12/18 fin 14/18 | ✅ b7/3✗ 306L 1st 9/18 fin 15/18 |
| 14-notes-api | ✅ b1 329L 1st 17/17 fin 17/17 | ✅ b5/3✗ 203L 1st 17/17 fin 17/17 | ✅ b7/2✗ 348L 1st 15/17 fin 16/17 | ✅ b6/2✗ 176L 1st 13/17 fin 13/17 |
| 15-auth-sessions | ✅ b1 338L 1st 17/17 fin 17/17 | ✅ b2/1✗ 187L 1st 17/17 fin 17/17 | ✅ b7/4✗ 391L 1st 14/17 fin 15/17 | ✅ b7/2✗ 230L 1st 14/17 fin 15/17 |
| 16-paginated-catalog | ✅ b1 397L 1st 15/15 fin 15/15 | ✅ b5/3✗ 261L 1st 15/15 fin 15/15 | ✅ b4 305L 1st 14/15 fin 15/15 | ✅ b7/5✗ 323L 1st 14/15 fin 14/15 |
| 17-csv-import-api | ✅ b1 309L 1st 14/14 fin 14/14 | ✅ b5/4✗ 298L 1st 14/14 fin 14/14 | ✅ b4/1✗ 411L 1st 12/14 fin 12/14 | ✅ b11/8✗ 211L 1st 12/14 fin 12/14 |
| 18-file-store | ✅ b1 228L 1st 14/14 fin 14/14 | ✅ b1 159L 1st 14/14 fin 14/14 | ✅ b10/1✗ 248L 1st 13/14 fin 13/14 | ✅ b6/2✗ 171L 1st 11/14 fin 12/14 |
| 19-reservations | ✅ b1 341L 1st 16/16 fin 16/16 | ✅ b4/3✗ 228L 1st 16/16 fin 16/16 | ✅ b3/1✗ 411L 1st 14/16 fin 14/16 | ✅ b10/5✗ 258L 1st 12/16 fin 12/16 |
| 20-rate-limiter | ✅ b1 169L 1st 14/14 fin 14/14 | ✅ b1 155L 1st 14/14 fin 14/14 | ✅ b3 187L 1st 11/14 fin 14/14 | ✅ b4/1✗ 127L 1st 12/14 fin 14/14 |
| 21-kv-counter | ✅ b1 285L 1st 18/18 fin 18/18 | ✅ b1 223L 1st 18/18 fin 18/18 | ✅ b4 333L 1st 13/18 fin 13/18 | ✅ b9/6✗ 256L 1st 7/18 fin 8/18 |
| 22-job-queue | ✅ b1 279L 1st 12/12 fin 12/12 | ✅ b14/5✗ 252L 1st 10/12 fin 12/12 | 🔨 b3/1✗ 357L | 🔨 b8/7✗ 200L |
| 23-webhook-relay | ✅ b1 214L 1st 12/12 fin 12/12 | ✅ b6/2✗ 231L 1st 12/12 fin 12/12 | 🔨 b7/1✗ 234L | 🔨 b6/5✗ 210L |
| 24-caching-proxy | ✅ b1 193L 1st 15/15 fin 15/15 | ✅ b3/1✗ 133L 1st 15/15 fin 15/15 | 🔨 b2 283L | 🔨 b6/3✗ 224L |
| 25-metrics-window | ✅ b1 287L 1st 16/16 fin 16/16 | ✅ b4/2✗ 196L 1st 16/16 fin 16/16 | ✅ b4/1✗ 272L 1st 14/16 fin 14/16 | 🔨 b7/2✗ 201L |

## Latest events

- 23:23:56 25-metrics-window/plumb-haiku paused: stopped by request (quota)
- 23:23:56 24-caching-proxy/plumb-haiku paused: stopped by request (quota)
- 23:23:56 24-caching-proxy/go-haiku paused: stopped by request (quota)
- 23:23:56 23-webhook-relay/plumb-haiku paused: stopped by request (quota)
- 23:23:56 23-webhook-relay/go-haiku paused: stopped by request (quota)
- 23:23:56 22-job-queue/plumb-haiku paused: stopped by request (quota)
- 23:23:56 22-job-queue/go-haiku paused: stopped by request (quota)
- 23:23:47 25-metrics-window/plumb-haiku build #7 ok
- 23:23:44 23-webhook-relay/go-haiku build #7 ok
- 23:23:41 21-kv-counter/plumb-haiku check final: 8/18
- 23:23:41 21-kv-counter/plumb-haiku check first: 7/18
- 23:23:40 21-kv-counter/plumb-haiku done
