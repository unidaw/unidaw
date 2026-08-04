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
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
SHM="/master_check_$$"
MASTER_ID=4294901760  # 0xFFFF0000 == kMasterTrackId

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 12 >/dev/null 2>&1 ) &
ENG=$!
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. Without this the
# engine was orphaned and ctest then blocked on it — ~1000s per timeout, measured across 18 runs.
# stop_engine escalates to SIGKILL after 10s and says so.
# There was NO EXIT TRAP here at all, so a timed-out check was guaranteed to orphan.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT
sleep 2

tracks() { DAW_UI_SHM_NAME="$SHM" "$CLI" get tracks 2>/dev/null; }

BEFORE="$(tracks)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do add-device --track master --kind patcher_event --force >/dev/null 2>&1 || true
sleep 0.6
AFTER="$(tracks)"
# Persistence: save the master's chain, reload, and confirm the device survived.
DAW_UI_SHM_NAME="$SHM" "$CLI" do save mchk --force >/dev/null 2>&1 || true
sleep 0.6
DAW_UI_SHM_NAME="$SHM" "$CLI" do load mchk --force >/dev/null 2>&1 || true
sleep 0.6
PERSISTED="$(tracks)"
wait "$ENG" 2>/dev/null || true
rm -rf "$TMP"

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
PERSISTED_M="$(printf '%s' "$PERSISTED" | master_present)"

echo "before   : $BEFORE_M"
echo "after add: $AFTER_M      (expect: yes dev='patcher_event')"
echo "reloaded : $PERSISTED_M  (expect: yes dev='patcher_event')"

ok=1
case "$BEFORE_M" in yes*) ;; *) echo "FAIL: master not published as an addressable entity"; ok=0;; esac
case "$AFTER_M" in *"dev='patcher_event'"*) ;; *) echo "FAIL: device added to master by its id did not land"; ok=0;; esac
case "$PERSISTED_M" in *"dev='patcher_event'"*) ;; *) echo "FAIL: master chain did not survive save/reload"; ok=0;; esac
[ "$ok" = "1" ] && echo "master_track_check: PASS — master is addressable, takes chain edits, and persists" \
                || { echo "master_track_check: FAIL"; exit 1; }
