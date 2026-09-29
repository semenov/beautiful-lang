#!/bin/sh
# Server timeouts: slow and idle clients are closed, a slow handler isn't.
PORT=18191
$PLUMB_BIN build timeouts_server.plumb -o /tmp/plumb-timeouts-server || exit 1
PORT=$PORT /tmp/plumb-timeouts-server 2>/dev/null &
pid=$!
sleep 1
$PLUMB_BIN run timeouts_client.plumb $PORT
kill -TERM $pid
wait $pid 2>/dev/null
rm -f /tmp/plumb-timeouts-server
