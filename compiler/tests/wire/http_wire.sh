#!/bin/sh
# Requests over a real connection: chunked bodies, 100-continue, the
# client's address, bodiless answers, a stream with a known length.
PORT=18190
PORT=$PORT $PLUMB_BIN run --debug http_server.plumb 2>/tmp/plumb-wire-err &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null 127.0.0.1:$PORT/ip && break; sleep 0.3; done
u=http://127.0.0.1:$PORT
curl -s $u/echo -H 'Transfer-Encoding: chunked' -d 'abc'; echo
printf 'from stdin' | curl -s -T - -X POST $u/echo; echo
head -c 2000000 /dev/zero | tr '\0' 'z' | curl -s -X POST --data-binary @- $u/echo | cut -c1-20
head -c 4000000 /dev/zero | curl -s -X POST --data-binary @- $u/echo; echo
curl -s $u/ip; echo
curl -si $u/empty | tr -d '\r' | grep -iv "^date"
curl -si $u/sized | tr -d '\r' | grep -i "content-length\|transfer-encoding\|^12345"
$PLUMB_BIN run --debug client_lines.plumb $u/lines 2>/dev/null
$PLUMB_BIN run --debug client_proxy.plumb $u/lines 2>/dev/null
kill -TERM $pid
wait $pid
grep "not freed" /tmp/plumb-wire-err | sed 's/.*allocations, //'
