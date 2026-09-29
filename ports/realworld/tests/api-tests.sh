#!/usr/bin/env bash
# Builds the server, starts it on a fresh database and runs the RealWorld
# API test suites against it:
#   - hurl/*.hurl: the official suite today (gothinkster/realworld specs/api/hurl)
#   - the last full Postman collection (realworld commit 5cd08ae, Feb 2026),
#     with newman, when node's npx is installed
#
#   tests/api-tests.sh            both
#   tests/api-tests.sh hurl       only hurl
#   tests/api-tests.sh postman    only newman
set -u
cd "$(dirname "$0")/.."
PLUMB_BIN="${PLUMB_BIN:-../../compiler/target/release/plumb}"
PORT="${PORT:-3917}"
WHICH="${1:-all}"
WORK="$(mktemp -d)"

"$PLUMB_BIN" build main.plumb -o "$WORK/conduit" || exit 1
CONDUIT_SECRET=test-secret "$WORK/conduit" --port "$PORT" --db "$WORK/test.db" 2> "$WORK/server.log" &
SERVER=$!
trap 'kill $SERVER; rm -rf "$WORK"' EXIT
for _ in $(seq 50); do
  curl -s -o /dev/null "http://localhost:$PORT/api/tags" && break
  sleep 0.1
done

STATUS=0
UID_VAL="$(date +%s)$$"
if [ "$WHICH" = all ] || [ "$WHICH" = hurl ]; then
  hurl --test --jobs 1 \
    --variable "host=http://localhost:$PORT" \
    --variable "uid=$UID_VAL" \
    tests/hurl/*.hurl || STATUS=1
fi

if [ "$WHICH" = all ] || [ "$WHICH" = postman ]; then
  COLLECTION="$WORK/Conduit.postman_collection.json"
  curl -sfL -o "$COLLECTION" \
    https://raw.githubusercontent.com/gothinkster/realworld/5cd08ae/api/Conduit.postman_collection.json || exit 1
  USERNAME="u$UID_VAL"
  npx -y newman run "$COLLECTION" \
    --delay-request 1 \
    --global-var "APIURL=http://localhost:$PORT/api" \
    --global-var "USERNAME=$USERNAME" \
    --global-var "EMAIL=$USERNAME@mail.com" \
    --global-var "PASSWORD=password" || STATUS=1
fi
exit $STATUS
