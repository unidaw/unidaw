#!/usr/bin/env bash
set -uo pipefail
unset BASH_ENV ENV

# Hunt the decisive #52 stack dir: one run with (a) a real chain/modlink/routing rejection,
# (b) drained=0 in the sidecar, and (c) the ring= cursors. Each suite run is ~10s, so this gets
# far more attempts per minute than a 50-minute sweep at one-in-two odds.
#
# Each run bounded at 90s so a hang is recorded, not blocking.
if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'hunt52: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || {
  printf '%s\n' 'hunt52: ERROR: cannot locate the containing tools directory' >&2
  exit 2
}
. "$SCRIPT_DIR/lib/repository_root.sh" || exit 2
ROOT="$(daw_repository_root)" || exit 2
cd "$ROOT" || exit 2
D="$(daw_make_temp_directory daw-hunt52)" || exit 2
# Failure logs are the purpose of this hunt, so retain this unique run-owned directory for review.
printf 'hunt52 logs retained at: %s\n' "$D"
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
