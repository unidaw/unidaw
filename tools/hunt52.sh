#!/bin/bash
# Hunt the decisive #52 stack dir: one run with (a) a real chain/modlink/routing rejection,
# (b) drained=0 in the sidecar, and (c) the ring= cursors. Each suite run is ~10s, so this gets
# far more attempts per minute than a 50-minute sweep at one-in-two odds.
#
# Each run bounded at 90s so a hang is recorded, not blocking.
cd /Users/jak/src/daw-web
D=/private/tmp/claude-501/-Users-jak-src-daw/072087ea-9515-45b9-8660-c8c34a937332/scratchpad
hits=0
for i in $(seq 1 24); do
  suite=chain-error-reasons; [ $((i % 2)) -eq 0 ] && suite=reject-reasons
  node "ui-web/test/$suite.mjs" > "$D/h52_$i.log" 2>&1 &
  pid=$!
  ( sleep 90; kill -9 $pid 2>/dev/null ) & k=$!
  wait $pid 2>/dev/null; rc=$?
  kill $k 2>/dev/null
  if [ "$rc" != "0" ]; then
    hits=$((hits+1))
    echo "run $i ($suite) FAILED — $(grep -oE '\[#52 probe\][^"]*' "$D/h52_$i.log" | head -1 | cut -c1-110)"
  fi
done
echo "TOTAL runs=24 failures=$hits"
