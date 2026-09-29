#!/bin/sh
# Runs every program in tests/run under the memory checker:
# - a program with `fn main()` must print exactly its `.out` file
# - a file with tests must pass them
# - nothing may leak
cd "$(dirname "$0")/run" || exit 1
PLUMB_BIN=${PLUMB_BIN:-../../target/release/plumb}
# temporary files of this run (two suites can run at once)
T=$(mktemp -d /tmp/plumb-suite.XXXXXX)
trap 'rm -rf "$T"' EXIT
# the runtime must compile on its own (split.rs): no quiet fallback here
export PLUMB_SPLIT_STRICT=1
case $PLUMB_BIN in /*) ;; *) PLUMB_BIN=$(pwd)/$PLUMB_BIN ;; esac
fail=0
for f in *.plumb; do
  name=${f%.plumb}
  if [ -f "$name.out" ]; then
    out=$($PLUMB_BIN run --debug "$f" 2>$T/test-err </dev/null)
    if [ "$out" != "$(cat "$name.out")" ]; then
      echo "FAIL $f: output differs"; fail=1
    fi
  fi
  if grep -q '^test "' "$f"; then
    if ! $PLUMB_BIN test --debug "$f" >$T/test-out 2>$T/test-err </dev/null; then
      if [ "$name" != "failing" ]; then echo "FAIL $f: tests failed"; cat $T/test-out; fail=1; fi
    fi
  fi
  if grep -q "not freed" $T/test-err && ! grep -q " 0 not freed" $T/test-err; then
    echo "FAIL $f: leak: $(grep 'not freed' $T/test-err)"; fail=1
  fi
  if grep -q "AddressSanitizer\|runtime error" $T/test-err; then
    echo "FAIL $f: memory error"; head -20 $T/test-err; fail=1
  fi
done
# error messages: each program in errors/ with a .err file must give
# exactly those messages
cd ../errors || exit 1
for f in *.plumb; do
  [ -f "${f%.plumb}.err" ] || continue
  out=$($PLUMB_BIN check "$f" 2>&1)
  if [ "$out" != "$(cat "${f%.plumb}.err")" ]; then
    echo "FAIL errors/$f: the messages differ"; echo "$out" | head -8; fail=1
  fi
done
cd ../run || exit 1
# over real connections: each script's output must match its .out
cd ../wire || exit 1
for f in *.sh; do
  out=$(PLUMB_BIN=$PLUMB_BIN ./"$f" 2>/dev/null)
  if [ "$out" != "$(cat "${f%.sh}.out")" ]; then
    echo "FAIL wire/$f: output differs"; echo "$out"; fail=1
  fi
done
cd ../run || exit 1
# the ports (ports/) still compile, and their tests pass
for d in ../../../ports/*/; do
  for f in "$d"main.plumb "$d"hey.plumb; do
    [ -f "$f" ] || continue
    if ! (cd "$d" && $PLUMB_BIN check "$(basename "$f")" >$T/port-err 2>&1); then
      echo "FAIL port $f: doesn't compile"; head -5 $T/port-err; fail=1
    fi
  done
  for f in "$d"*.plumb; do
    grep -q '^test "' "$f" || continue
    if ! (cd "$d" && $PLUMB_BIN test "$(basename "$f")" >$T/port-err 2>&1); then
      echo "FAIL port $f: tests"; grep -A3 FAIL $T/port-err | head -8; fail=1
    fi
  done
done
# the packages (packages/) and their examples compile
for d in ../../../packages/*/; do
  for e in "$d"examples/*.plumb; do
    [ -f "$e" ] || continue
    rm -rf $T/pkg && mkdir -p $T/pkg && cp "$d"*.plumb "$e" $T/pkg/
    if ! (cd $T/pkg && $PLUMB_BIN check "$(basename "$e")" >$T/port-err 2>&1); then
      echo "FAIL package example $e: doesn't compile"; head -5 $T/port-err; fail=1
    fi
  done
done
# the benchmarks compile
for f in ../../../benchmarks/plumb/*.plumb ../../../benchmarks/backend/plumb/*.plumb; do
  if ! (cd "$(dirname "$f")" && $PLUMB_BIN check "$(basename "$f")" >$T/port-err 2>&1); then
    echo "FAIL benchmark $f: doesn't compile"; head -5 $T/port-err; fail=1
  fi
done
# the language's own code is laid out the standard way
if ! $PLUMB_BIN fmt --check . ../../src/std ../../src/prelude.plumb >/dev/null 2>$T/fmt-err; then
  echo "FAIL plumb fmt --check:"; cat $T/fmt-err; fail=1
fi
[ $fail = 0 ] && echo "all passed"
exit $fail
