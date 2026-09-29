#!/bin/bash
# Compares the plumb port with the Go original on the shipped test data.
GO=/tmp/port-src/gron/gron
PLUMB_GRON=/tmp/port-gron/gron
TD=/tmp/port-src/gron/testdata
pass=0; fail=0
check() { # name, command args...
  local name="$1"; shift
  local a b ca cb
  a=$("$GO" "$@" 2>&1); ca=$?
  b=$("$PLUMB_GRON" "$@" 2>&1); cb=$?
  if [ "$a" == "$b" ] && [ $ca == $cb ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: $name (exit $ca vs $cb)"; diff <(echo "$a") <(echo "$b") | head -5; fi
}
checkin() { # name, input file, args...
  local name="$1"; local in="$2"; shift 2
  local a b ca cb
  a=$("$GO" "$@" < "$in" 2>&1); ca=$?
  b=$("$PLUMB_GRON" "$@" < "$in" 2>&1); cb=$?
  if [ "$a" == "$b" ] && [ $ca == $cb ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: $name (exit $ca vs $cb)"; diff <(echo "$a") <(echo "$b") | head -5; fi
}
for f in "$TD"/*.json /tmp/port-gron/testdata/*.json; do
  n=$(basename "$f")
  check "gron $n" -m "$f"
  check "gron -c $n" -c "$f"
  check "gron --json $n" -m --json "$f"
  check "gron -s $n" -m -s "$f"
  check "gron -s --json $n" -m -s --json "$f"
  check "gron -s -c $n" -c -s "$f"
  checkin "gron stdin $n" "$f" -m
  # round trip
  a=$("$GO" -m "$f" 2>/dev/null | "$GO" -u -m 2>&1); b=$("$PLUMB_GRON" -m "$f" 2>/dev/null | "$PLUMB_GRON" -u -m 2>&1)
  if [ "$a" == "$b" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: roundtrip $n"; diff <(echo "$a") <(echo "$b") | head -5; fi
  a=$("$GO" -m --json "$f" 2>/dev/null | "$GO" -u -m --json 2>&1); b=$("$PLUMB_GRON" -m --json "$f" 2>/dev/null | "$PLUMB_GRON" -u -m --json 2>&1)
  if [ "$a" == "$b" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: json roundtrip $n"; diff <(echo "$a") <(echo "$b") | head -5; fi
  # no-sort: compare as sorted sets of lines
  a=$("$GO" -m --no-sort "$f" 2>&1 | sort); b=$("$PLUMB_GRON" -m --no-sort "$f" 2>&1 | sort)
  if [ "$a" == "$b" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "DIFF: no-sort $n"; fi
done
for f in "$TD"/*.gron /tmp/port-gron/testdata/*.gron; do
  n=$(basename "$f")
  check "ungron $n" -u -m "$f"
  check "ungron -c $n" -u -c "$f"
  checkin "values $n" "$f" -v
done
for f in "$TD"/*.jgron /tmp/port-gron/testdata/*.jgron; do
  n=$(basename "$f")
  check "ungron --json $n" -u -m --json "$f"
done
check "missing file" /nonexistent.json
check "version" --version
check "bad flag" --bogus
echo "pass=$pass fail=$fail"
