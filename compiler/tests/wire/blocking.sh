#!/bin/sh
# Blocking C calls (SQLite) don't stall the other tasks.
$LANG_BIN run blocking.lang
