#!/usr/bin/env bash
# Checks the MASTER track (patcher-is-a-device item 4a): it is published as an
# addressable entity (stable id kMasterTrackId, master flag) right after the regular
# tracks, and its device chain accepts edits addressed by that id — a patcher device
# added to the master shows up on it. (Audio FX on the master sum is 4b.)
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/master_track_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
SHM="/master_check_$$"
MASTER_ID=4294901760  # 0xFFFF0000 == kMasterTrackId

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" ./daw_engine --run-seconds 6 >/dev/null 2>&1 ) &
ENG=$!
sleep 2

tracks() { DAW_UI_SHM_NAME="$SHM" "$CLI" get tracks 2>/dev/null; }

BEFORE="$(tracks)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do add-device --track master --kind patcher_event --force >/dev/null 2>&1 || true
sleep 0.6
AFTER="$(tracks)"
wait "$ENG" 2>/dev/null || true

master_present() {
  python3 -c "
import json,sys
d=json.loads(sys.stdin.read())
m=[t for t in d['tracks'] if t['track_id']==$MASTER_ID]
print('yes' if (m and m[0]['name']=='Master' and m[0].get('master')) else 'no', end='')
print(' dev='+repr(m[0]['device']) if m else '')
"
}
BEFORE_M="$(printf '%s' "$BEFORE" | master_present)"
AFTER_M="$(printf '%s' "$AFTER" | master_present)"

echo "before: $BEFORE_M"
echo "after : $AFTER_M  (expect: yes dev='patcher_event')"

ok=1
case "$BEFORE_M" in yes*) ;; *) echo "FAIL: master not published as an addressable entity"; ok=0;; esac
case "$AFTER_M" in *"dev='patcher_event'"*) ;; *) echo "FAIL: device added to master by its id did not land"; ok=0;; esac
[ "$ok" = "1" ] && echo "master_track_check: PASS — master is addressable and its chain takes edits" \
                || { echo "master_track_check: FAIL"; exit 1; }
