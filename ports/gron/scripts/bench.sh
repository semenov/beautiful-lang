#!/bin/bash
# Time and peak memory of the Go original and the plumb port.
# Needs /tmp/lt/big.json (53 MB), big.stream, go.gron (its gron output) and
# med5000.gron (made by scripts/make_bench_data.py). If /tmp/lt/gron_nohttp
# exists (a build without the URL branch, see GAPS.md) it is measured too.
GO=/tmp/port-src/gron/gron
LG=/tmp/port-gron/gron
cd /tmp/lt
run() { # label, args... ; stdin from $IN if set
  local label="$1"; shift
  local impls="go plumb"; [ -x /tmp/lt/gron_nohttp ] && impls="go plumb nohttp"
  for impl in $impls; do
    local bin=$GO; [ $impl == plumb ] && bin=$LG; [ $impl == nohttp ] && bin=/tmp/lt/gron_nohttp
    best=999999; mem=0
    for i in 1 2 3; do
      /usr/bin/time -l $bin "$@" < ${IN:-/dev/null} > /dev/null 2> /tmp/lt/t.txt
      r=$(awk '/real/ {print $1}' /tmp/lt/t.txt); m=$(awk '/maximum resident/ {print $1}' /tmp/lt/t.txt)
      best=$(echo "$r $best" | awk '{print ($1<$2)?$1:$2}'); mem=$m
    done
    printf "| %-22s | %-6s | %6.2f s | %7d MB |\n" "$label" $impl $best $((mem/1048576))
  done
}
echo "| case | impl | time (best of 3) | peak RSS |"
echo "|---|---|---:|---:|"
run "gron 53 MB" -m big.json
run "gron --no-sort" -m --no-sort big.json
run "gron --json" -m --json big.json
run "gron -c" -c big.json
run "gron -s (130k lines)" -m -s big.stream
IN=go.gron run "gron -v (3.3M lines)" -v
run "ungron 128k lines" -u -m med5000.gron
