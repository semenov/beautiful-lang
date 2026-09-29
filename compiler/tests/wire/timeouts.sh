#!/bin/sh
# Server timeouts: slow and idle clients are closed, a slow handler isn't.
PORT=18191
$LANG_BIN build timeouts_server.lang -o /tmp/lang-timeouts-server || exit 1
PORT=$PORT /tmp/lang-timeouts-server 2>/dev/null &
pid=$!
sleep 1
$LANG_BIN run timeouts_client.lang $PORT
kill -TERM $pid
wait $pid 2>/dev/null
rm -f /tmp/lang-timeouts-server
