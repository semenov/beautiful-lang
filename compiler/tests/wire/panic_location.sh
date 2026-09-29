#!/bin/sh
# A panic in an imported file names that file and line.
cd "$(dirname "$0")/panicfile" || exit 1
$PLUMB_BIN run main.plumb 2>&1 >/dev/null | grep -E "^panic|^  at"
