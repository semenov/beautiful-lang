#!/bin/sh
# Blocking C calls (SQLite) don't stall the other tasks.
$PLUMB_BIN run blocking.plumb
