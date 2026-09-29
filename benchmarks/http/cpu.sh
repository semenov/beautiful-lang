#!/bin/sh
# CPU time a server spends per request: run the load, read the server's CPU
# time before and after.  ./cpu.sh <pid> <url> [connections]
pid=$1; url=$2; c=${3:-64}
t() { ps -o time= -p $pid | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*1000+$3*10; else print (($1*60+$2)*60+$3)*1000+$4*10 }'; }
before=$(t)
out=$(/tmp/hb-load -c $c -d 5s $url)
after=$(t)
reqs=$(echo "$out" | sed -E 's/.*\(([0-9]+) requests.*/\1/')
echo "$out; server CPU $(( after - before )) ms, $(echo "scale=2; ($after - $before) * 1000 / $reqs" | bc) us per request"
