#!/bin/sh
# The notes service in Plumb and in Go, each in a Docker container limited
# to one CPU (as a small cloud instance or a Kubernetes pod with
# `cpu: 1`); the load comes from another container on other CPUs.
#   ./docker.sh [connections] [seconds] [limit]
# limit: "quota" (--cpus=1: a share of the time of all CPUs, as Kubernetes
# does) or "pin" (--cpuset-cpus: one CPU).
cd "$(dirname "$0")" || exit 1
C=${1:-64}
D=${2:-10}
LIMIT=${3:-quota}
root=$(cd ../.. && pwd)
out=/tmp/be-docker
mkdir -p $out
set -e
# Plumb: the compiler built for Linux, then the server
docker run --rm -v "$root":/src -v $out:/out -w /src lang-linux sh -c \
  'cd compiler && CARGO_TARGET_DIR=target-linux cargo build --release -q 2>/dev/null && cd ../benchmarks/backend && ../../compiler/target-linux/release/plumb build plumb/server.plumb -o /out/plumb'
# Go: with cgo for SQLite, on Alpine like the image the servers run in
docker run --rm -v "$root/benchmarks/backend":/b -v $out:/out -v be-gocache:/root/.cache -v be-gomod:/go/pkg/mod -w /b/go golang:1.27-alpine sh -c \
  'apk add -q gcc musl-dev >/dev/null && CGO_ENABLED=1 go build -o /out/go .'
(cd load && GOOS=linux CGO_ENABLED=0 go build -o $out/load .)
set +e
docker network create be-net >/dev/null 2>&1
if [ "$LIMIT" = pin ]; then lim="--cpuset-cpus=0"; else lim="--cpus=1"; fi
# go-pool4: Go with its connection pool limited to 4, as Plumb's on one CPU
for impl in plumb go go-pool4; do
  docker rm -f be-server >/dev/null 2>&1
  bin=${impl%-pool4}
  conns=$([ $impl = go-pool4 ] && echo 4 || echo 0)
  docker run -d --name be-server --network be-net $lim -v $out:/out -e PORT=8300 -e DB=/tmp/notes.db -e MAX_CONNS=$conns lang-linux /out/$bin >/dev/null
  sleep 1
  run_load() { docker run --rm --network be-net --cpuset-cpus=4-11 -v $out:/out lang-linux /out/load -url http://be-server:8300 "$@"; }
  run_load -c $C -d 1s >/dev/null # warm up, and the setup
  cpu0=$(docker exec be-server sh -c 'grep usage_usec /sys/fs/cgroup/cpu.stat | cut -d" " -f2')
  res=$(run_load -c $C -d ${D}s -users 20 -notes 1)
  cpu1=$(docker exec be-server sh -c 'grep usage_usec /sys/fs/cgroup/cpu.stat | cut -d" " -f2')
  peak=$(docker exec be-server sh -c 'cat /sys/fs/cgroup/memory.peak' | awk '{printf "%d MB", $1/1048576}')
  threads=$(docker exec be-server sh -c 'ls /proc/1/task | wc -l')
  reqs=$(echo "$res" | sed -E 's/.* of ([0-9]+).*/\1/')
  echo "$impl: $res; CPU $(echo "scale=1; ($cpu1 - $cpu0) / $reqs" | bc) us per request; peak $peak (container); $threads threads"
  docker rm -f be-server >/dev/null
done
