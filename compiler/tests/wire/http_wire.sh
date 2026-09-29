#!/bin/sh
# Requests over a real connection: chunked bodies, 100-continue, the
# client's address, bodiless answers, a stream with a known length.
PORT=18190
PORT=$PORT $LANG_BIN run --debug http_server.lang 2>/tmp/lang-wire-err &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null localhost:$PORT/ip && break; sleep 0.3; done
u=http://127.0.0.1:$PORT
curl -s $u/echo -H 'Transfer-Encoding: chunked' -d 'abc'; echo
printf 'from stdin' | curl -s -T - -X POST $u/echo; echo
head -c 2000000 /dev/zero | tr '\0' 'z' | curl -s -X POST --data-binary @- $u/echo | cut -c1-20
curl -s $u/ip; echo
curl -si $u/empty | tr -d '\r' | grep -iv "^date"
curl -si $u/sized | tr -d '\r' | grep -i "content-length\|transfer-encoding\|^12345"
kill -TERM $pid
wait $pid
grep "not freed" /tmp/lang-wire-err | sed 's/.*allocations, //'
