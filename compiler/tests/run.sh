#!/bin/sh
# Runs every program in tests/run under the memory checker:
# - a program with `fn main()` must print exactly its `.out` file
# - a file with tests must pass them
# - nothing may leak
cd "$(dirname "$0")/run" || exit 1
LANG_BIN=${LANG_BIN:-../../target/release/lang}
case $LANG_BIN in /*) ;; *) LANG_BIN=$(pwd)/$LANG_BIN ;; esac
fail=0
for f in *.lang; do
  name=${f%.lang}
  if [ -f "$name.out" ]; then
    out=$($LANG_BIN run --debug "$f" 2>/tmp/lang-test-err </dev/null)
    if [ "$out" != "$(cat "$name.out")" ]; then
      echo "FAIL $f: output differs"; fail=1
    fi
  fi
  if grep -q '^test "' "$f"; then
    if ! $LANG_BIN test --debug "$f" >/tmp/lang-test-out 2>/tmp/lang-test-err </dev/null; then
      if [ "$name" != "failing" ]; then echo "FAIL $f: tests failed"; cat /tmp/lang-test-out; fail=1; fi
    fi
  fi
  if grep -q "not freed" /tmp/lang-test-err && ! grep -q " 0 not freed" /tmp/lang-test-err; then
    echo "FAIL $f: leak: $(grep 'not freed' /tmp/lang-test-err)"; fail=1
  fi
  if grep -q "AddressSanitizer\|runtime error" /tmp/lang-test-err; then
    echo "FAIL $f: memory error"; head -20 /tmp/lang-test-err; fail=1
  fi
done
# over real connections: each script's output must match its .out
cd ../wire || exit 1
for f in *.sh; do
  out=$(LANG_BIN=$LANG_BIN ./"$f" 2>/dev/null)
  if [ "$out" != "$(cat "${f%.sh}.out")" ]; then
    echo "FAIL wire/$f: output differs"; echo "$out"; fail=1
  fi
done
cd ../run || exit 1
# the ports (ports/) still compile, and their tests pass
for d in ../../../ports/*/; do
  for f in "$d"main.lang "$d"hey.lang; do
    [ -f "$f" ] || continue
    if ! (cd "$d" && $LANG_BIN check "$(basename "$f")" >/tmp/lang-port-err 2>&1); then
      echo "FAIL port $f: doesn't compile"; head -5 /tmp/lang-port-err; fail=1
    fi
  done
  for f in "$d"*.lang; do
    grep -q '^test "' "$f" || continue
    if ! (cd "$d" && $LANG_BIN test "$(basename "$f")" >/tmp/lang-port-err 2>&1); then
      echo "FAIL port $f: tests"; grep -A3 FAIL /tmp/lang-port-err | head -8; fail=1
    fi
  done
done
# the packages (packages/) and their examples compile
for d in ../../../packages/*/; do
  for e in "$d"examples/*.lang; do
    [ -f "$e" ] || continue
    rm -rf /tmp/lang-pkg && mkdir -p /tmp/lang-pkg && cp "$d"*.lang "$e" /tmp/lang-pkg/
    if ! (cd /tmp/lang-pkg && $LANG_BIN check "$(basename "$e")" >/tmp/lang-port-err 2>&1); then
      echo "FAIL package example $e: doesn't compile"; head -5 /tmp/lang-port-err; fail=1
    fi
  done
done
# the benchmarks compile
for f in ../../../benchmarks/lang/*.lang ../../../benchmarks/backend/lang/*.lang; do
  if ! (cd "$(dirname "$f")" && $LANG_BIN check "$(basename "$f")" >/tmp/lang-port-err 2>&1); then
    echo "FAIL benchmark $f: doesn't compile"; head -5 /tmp/lang-port-err; fail=1
  fi
done
# the language's own code is laid out the standard way
if ! $LANG_BIN fmt --check . ../../src/std ../../src/prelude.lang >/dev/null 2>/tmp/lang-fmt-err; then
  echo "FAIL lang fmt --check:"; cat /tmp/lang-fmt-err; fail=1
fi
[ $fail = 0 ] && echo "all passed"
exit $fail
