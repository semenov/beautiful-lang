#!/bin/sh
# A stack overflow is a panic with a message, on the main thread and in a
# task; a task's stack is as deep as the main thread's.
for p in overflow overflow_task; do
  $LANG_BIN build $p.lang -o /tmp/lang-$p || exit 1
  /tmp/lang-$p 100000; echo "exit $?"
  /tmp/lang-$p 100000000 2>&1; echo "exit $?"
  rm -f /tmp/lang-$p
done
