#!/bin/sh
# Runs every program in tests/run under the memory checker:
# - a program with `fn main()` must print exactly its `.out` file
# - a file with tests must pass them
# - nothing may leak
cd "$(dirname "$0")/run" || exit 1
LANG_BIN=../../target/release/lang
fail=0
for f in *.lang; do
  name=${f%.lang}
  if [ -f "$name.out" ]; then
    out=$($LANG_BIN run --debug "$f" 2>/tmp/lang-test-err)
    if [ "$out" != "$(cat "$name.out")" ]; then
      echo "FAIL $f: output differs"; fail=1
    fi
  fi
  if grep -q '^test "' "$f"; then
    if ! $LANG_BIN test --debug "$f" >/tmp/lang-test-out 2>/tmp/lang-test-err; then
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
[ $fail = 0 ] && echo "all passed"
exit $fail
