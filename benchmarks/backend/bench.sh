#!/bin/sh
# Runs the notes service in Plumb and in Go on fresh databases under the same
# load; prints requests per second, latency, server CPU per request and the
# server's peak memory.   ./bench.sh [connections] [seconds]
cd "$(dirname "$0")"
C=${1:-64}
D=${2:-10}
PLUMB_BIN=${PLUMB_BIN:-../../compiler/target/release/plumb}
$PLUMB_BIN build plumb/server.plumb -o /tmp/be-plumb || exit 1
(cd go && go build -o /tmp/be-go .) || exit 1
(cd load && go build -o /tmp/be-load .) || exit 1
cpu() { ps -o time= -p $1 | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*1000+$3*10; else print (($1*60+$2)*60+$3)*1000+$4*10 }'; }
for impl in plumb go; do
  dir=$(mktemp -d)
  port=$([ $impl = plumb ] && echo 8300 || echo 8301)
  DB=$dir/notes.db PORT=$port /usr/bin/time -l /tmp/be-$impl 2>$dir/err >/dev/null &
  sleep 1
  pid=$(pgrep -n -f /tmp/be-$impl$)
  /tmp/be-load -url http://127.0.0.1:$port -c $C -d 1s >/dev/null # warm up, and the setup
  before=$(cpu $pid)
  out=$(/tmp/be-load -url http://127.0.0.1:$port -c $C -d ${D}s -users 20 -notes 1)
  after=$(cpu $pid)
  kill -TERM $pid; wait 2>/dev/null
  reqs=$(echo "$out" | sed -E 's/.* of ([0-9]+).*/\1/')
  rss=$(grep "maximum resident" $dir/err | awk '{printf "%d MB", $1/1048576}')
  echo "$impl: $out; CPU $(echo "scale=1; ($after - $before) * 1000 / $reqs" | bc) us per request; peak $rss"
  rm -rf $dir
done
