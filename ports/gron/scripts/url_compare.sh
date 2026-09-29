#!/bin/bash
# Compares URL input (needs: python3 -m http.server 8765 in the testdata dir).
for args in "-m http://127.0.0.1:8765/two.json" "-m -s http://127.0.0.1:8765/stream.json" "-u -m http://127.0.0.1:8765/two.gron" "-m http://127.0.0.1:8765/missing.json" "-m http://localhost:1/x.json" "-m -k http://127.0.0.1:8765/two.json" "-m -x http://p:1 http://127.0.0.1:8765/two.json"; do
  a=$(/tmp/port-src/gron/gron $args 2>&1); ca=$?
  b=$(/tmp/port-gron/gron $args 2>&1); cb=$?
  if [ "$a" == "$b" ] && [ $ca == $cb ]; then echo "SAME: $args"; else echo "DIFF: $args ($ca vs $cb)"; diff <(echo "$a") <(echo "$b") | head -6; fi
done
