#!/bin/bash
# Compares go-httpbin (port 18080) with the plumb port (18090), request by request.
# Usage: compare.sh  (reads cases below: "name|curl args")
GO=18080; PLUMB_PORT=18090
norm() {
  # headers: lower-case names, drop date/connection, sort; body kept as is
  python3 -c '
import sys,re
data=sys.stdin.buffer.read()
sep=data.find(b"\r\n\r\n")
head=data[:sep].decode("latin1").split("\r\n"); body=data[sep+4:]
status=head[0]
if any(h.lower().startswith("transfer-encoding: chunked") for h in head):
  out=b""; rest=body
  while rest:
    line,_,rest=rest.partition(b"\r\n")
    try: n=int(line.split(b";")[0],16)
    except Exception: break
    if n==0: break
    out+=rest[:n]; rest=rest[n+2:]
  body=out
import zlib
if any(h.lower().startswith("content-encoding: gzip") for h in head): body=zlib.decompress(body,31)
if any(h.lower().startswith("content-encoding: deflate") for h in head): body=zlib.decompress(body)
hs=[]
for h in head[1:]:
  k,_,v=h.partition(":")
  k=k.lower().strip(); v=v.strip()
  if k in ("date","connection","content-length","transfer-encoding","keep-alive"): continue
  v=re.sub(r"1808[0-9]|1809[0-9]","PORT",v)
  hs.append(k+": "+v)
print(status.replace("HTTP/1.1 ","").strip())
for h in sorted(hs): print(h)
print()
try:
  t=body.decode("utf8")
  t=re.sub(r"1808[0-9]|1809[0-9]","PORT",t)
  t=re.sub(r"(\"origin\": ?)\"::1\"",lambda m: m.group(1)+chr(34)*2,t)
  print(t)
except Exception:
  import hashlib; print("<binary %d bytes sha1 %s>"%(len(body),hashlib.sha1(body).hexdigest()))
'
}
pass=0; fail=0
while IFS='|' read -r name args; do
  [ -z "$name" ] && continue
  [[ "$name" == \#* ]] && continue
  a=$(eval curl -s -i --raw --max-time 15 "http://localhost:$GO$args" | norm)
  b=$(eval curl -s -i --raw --max-time 15 "http://localhost:$PLUMB_PORT$args" | norm)
  if [ "$a" == "$b" ]; then echo "SAME  $name"; pass=$((pass+1)); else echo "DIFF  $name"; diff <(echo "$a") <(echo "$b") | head -${DIFFLINES:-12} | sed 's/^/      /'; fail=$((fail+1)); fi
done < "${1:-/tmp/port-httpbin/compare/cases.txt}"
echo "same: $pass, different: $fail"
