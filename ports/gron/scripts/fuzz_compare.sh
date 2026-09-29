#!/bin/bash
# Runs both versions on the files made by fuzz.py: gron, --json, -s, -c,
# and ungron of the Go output. Usage: fuzz_compare.sh DIR
GO=/tmp/port-src/gron/gron
LG=/tmp/port-gron/gron
D=$1
pass=0; fail=0
same() { # name, then a and b as "$out|$code"
  if [ "$2" == "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: $1"; diff <(echo "$2") <(echo "$3") | head -4; fi
}
for f in "$D"/*.json; do
  for args in "-m" "-m --json" "-c" "-m --no-sort"; do
    a=$($GO $args "$f" 2>&1; echo "|$?"); b=$($LG $args "$f" 2>&1; echo "|$?")
    if [ "$args" == "-m --no-sort" ]; then a=$(echo "$a" | sort); b=$(echo "$b" | sort); fi
    same "$args $f" "$a" "$b"
  done
  g=$($GO -m "$f" 2>/dev/null)
  a=$(echo "$g" | $GO -u -m 2>&1; echo "|$?"); b=$(echo "$g" | $LG -u -m 2>&1; echo "|$?")
  same "ungron $f" "$a" "$b"
  a=$(echo "$g" | $GO -u -c 2>&1; echo "|$?"); b=$(echo "$g" | $LG -u -c 2>&1; echo "|$?")
  same "ungron -c $f" "$a" "$b"
  a=$(echo "$g" | $GO -v 2>&1; echo "|$?"); b=$(echo "$g" | $LG -v 2>&1; echo "|$?")
  same "values $f" "$a" "$b"
  j=$($GO -m --json "$f" 2>/dev/null)
  a=$(echo "$j" | $GO -u -m --json 2>&1; echo "|$?"); b=$(echo "$j" | $LG -u -m --json 2>&1; echo "|$?")
  same "ungron --json $f" "$a" "$b"
  # a damaged gron text: drop one character from a random line
  h=$(echo "$g" | awk 'BEGIN{srand('"$RANDOM"')} {if (rand() < 0.2 && length($0) > 3) {i=int(rand()*length($0))+1; print substr($0,1,i-1) substr($0,i+1)} else print}')
  a=$(echo "$h" | $GO -u -m 2>&1; echo "|$?"); b=$(echo "$h" | $LG -u -m 2>&1; echo "|$?")
  same "ungron damaged $f" "$a" "$b"
done
for f in "$D"/*.stream; do
  for args in "-m -s" "-m -s --json" "-c -s"; do
    a=$($GO $args "$f" 2>&1; echo "|$?"); b=$($LG $args "$f" 2>&1; echo "|$?")
    same "$args $f" "$a" "$b"
  done
done
echo "pass=$pass fail=$fail"
