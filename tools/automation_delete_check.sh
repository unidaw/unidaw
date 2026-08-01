#!/usr/bin/env bash
# AN AUTOMATION POINT CAN BE DELETED, AND THE DELETION IS HEARD, SEEN AND SAVED.
#
# `WriteAutomationPoint` (60) creates a point and re-values one at a tick that already has a
# point. Nothing removed one. So an automation lane was DRAW-ONLY: a point written at the wrong
# tick could only be neutralised by writing another beside it and leaving the mistake in the
# curve, and a UI drawing that lane had no eraser. Reported by the web-UI agent, and confirmed
# against the code rather than the manual — 60 was the only automation opcode.
#
# SIX PROPERTIES:
#   PUBLISHED    the deleted point is gone from the lane the UI reads back
#   ADDRESSED    deleting one point leaves its neighbours alone, asserted from both ends —
#                the handler walks lanes and points, and a fixture with one of either cannot
#                tell "deleted the one I named" from "deleted the first one it met"
#   LANE KEPT    removing one point of several leaves the lane, with the rest intact
#   LANE DROPPED removing the LAST point removes the lane, rather than leaving an empty clip
#                that reappears on reload and cannot be got rid of
#   REFUSED      a lane that does not exist, and a tick with no point on it, are refused by the
#                ENGINE with a reason — deleting a point that is not there is a caller working
#                from a stale view of the curve, not a no-op worth swallowing
#   PERSISTS     the deletion survives a save and a reload, with the value moved away in between
#                so "the load restored it" and "nothing happened" are distinguishable
#
# No audio device needed: an automation point is data.
#   tools/automation_delete_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP/au.uniproj.json" <<'PY'
import json, sys
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "au"}, "nanoticks_per_quarter": 960000,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/auchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project au --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# The ticks of one lane, as the UI reads them back, space separated and in order.
#
# `get automation-points`, not `get automation`: the latter lists lanes with a point COUNT, and a
# count cannot tell "the point I named is gone" from "a different one is". The windowed query is
# what a UI drawing the lane actually uses.
#
# `found: false` is the lane's ANSWER, not an error, so it maps to "nolane" here rather than to a
# failure — which is what makes the LANE DROPPED property assertable at all.
ticks() {  # ticks <paramId>
  cli get automation-points --track 0 --param "$1" 2>/dev/null | python3 -c "
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    print('unreadable'); raise SystemExit
try:
    d = json.loads(raw)
except Exception:
    print('unreadable'); raise SystemExit
if not d.get('found'):
    print('nolane'); raise SystemExit
print(' '.join(str(p['nanotick']) for p in d.get('points', [])))
" 2>/dev/null
}
waitticks() {  # waitticks <paramId> <want>
  for _ in $(seq 1 60); do
    [ "$(ticks "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}
reason() { grep -q "\"event\":\"automation.delete_rejected\".*\"reason\":\"$1\"" "$TMP/eng.log"; }

# TWO LANES, three points each, so both "the right lane" and "the right point" are assertable.
# A one-lane, one-point fixture cannot tell an addressed delete from a delete-whatever-you-meet.
for T in 0 960000 1920000; do
  cli do automation --track 0 --param "index:0" --nanotick "$T" --value 0.25 >/dev/null 2>&1
done
for T in 0 960000 1920000; do
  cli do automation --track 0 --param "index:1" --nanotick "$T" --value 0.75 >/dev/null 2>&1
done
waitticks "index:0" "0 960000 1920000" || \
  fail "the fixture's first lane did not read back as three points, it reads '$(ticks index:0)' —
        nothing below could mean anything. If it says 'unreadable', get automation's shape has
        changed and this check needs updating rather than the engine"
waitticks "index:1" "0 960000 1920000" || \
  fail "the fixture's second lane did not read back as three points: '$(ticks index:1)'"
echo "  two lanes of three points each"

# ---- PUBLISHED, and ADDRESSED: the middle point of the FIRST lane only.
cli do delete-automation --track 0 --param "index:0" --nanotick 960000 >/dev/null 2>&1
waitticks "index:0" "0 1920000" || \
  fail "after deleting the point at 960000 the lane reads '$(ticks index:0)', wanted '0 1920000'.
        The RT scheduler reads automation from the track SNAPSHOT, so a deletion that is not
        republished is a point that still PLAYS after it has been removed"
echo "  published: the middle point is gone from the lane the UI reads"

[ "$(ticks index:1)" = "0 960000 1920000" ] || \
  fail "deleting from lane index:0 changed lane index:1, which now reads '$(ticks index:1)' —
        the command is addressed by paramId and reached the wrong lane, or all of them"
echo "  addressed: the other lane is untouched"

# ---- THE OTHER DIRECTION, so the assertion does not depend on which lane the handler met first.
cli do delete-automation --track 0 --param "index:1" --nanotick 0 >/dev/null 2>&1
waitticks "index:1" "960000 1920000" || \
  fail "deleting from lane index:1 did not reach it: it reads '$(ticks index:1)'"
[ "$(ticks index:0)" = "0 1920000" ] || \
  fail "deleting from lane index:1 changed lane index:0, which now reads '$(ticks index:0)'"
echo "  addressed both ways: index:0 then index:1, each leaving the other alone"

# ---- LANE KEPT. Two points remain, so the lane must still be there.
[ "$(ticks index:0)" != "nolane" ] || \
  fail "the lane vanished while it still had points in it"
echo "  lane kept: a lane with points left is still a lane"

# ---- REFUSED BY THE ENGINE, both cases, asserted on the reason rather than an exit code: the
# CLI cannot know what is in the model, so these are the engine's to judge.
cli do delete-automation --track 0 --param "index:9" --nanotick 0 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do reason no_such_lane && break; sleep 0.25; done
reason no_such_lane || \
  fail "deleting from a lane that does not exist was not refused with reason no_such_lane"

cli do delete-automation --track 0 --param "index:0" --nanotick 12345 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do reason no_point_at_tick && break; sleep 0.25; done
reason no_point_at_tick || \
  fail "deleting a tick with no point on it was not refused with reason no_point_at_tick. A
        caller asking to remove something that is not there has a stale view of the curve, and
        swallowing it makes that disagreement unreportable"
[ "$(ticks index:0)" = "0 1920000" ] || \
  fail "a refused delete still changed the lane: it reads '$(ticks index:0)'"
echo "  refused by the engine: a lane that is not there, and a tick with no point — and neither
        moved the curve"

# ---- LANE DROPPED. Emptying a lane must remove it, not leave an empty clip behind.
cli do delete-automation --track 0 --param "index:0" --nanotick 0 >/dev/null 2>&1
cli do delete-automation --track 0 --param "index:0" --nanotick 1920000 >/dev/null 2>&1
waitticks "index:0" "nolane" || \
  fail "after deleting its last point the lane still exists, reading '$(ticks index:0)'. An empty
        automation clip is still saved and still declares its discreteOnly flag, so it reappears
        on reload — visible, empty, and impossible to get rid of"
echo "  lane dropped: emptying a lane removes it"

# ---- PERSISTS. Saved, moved away, reloaded — so a reload doing NOTHING is distinguishable from
# one that restored correctly.
cli do save auout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/auout.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/auout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/auout.uniproj.json" <<'PYC' || fail "the deletion did not reach the saved project"
import json, sys
d = json.load(open(sys.argv[1]))
lanes = []
for t in d.get("tracks", []):
    lanes.extend(t.get("automation_clips", t.get("automation", [])) or [])
ids = [l.get("param_id", l.get("param")) for l in lanes]
if "index:0" in ids:
    print("  the emptied lane index:0 is still in the saved file: %r" % (ids,)); raise SystemExit(1)
keep = [l for l in lanes if l.get("param_id", l.get("param")) == "index:1"]
if not keep:
    print("  lane index:1 is missing from the save entirely: %r" % (ids,)); raise SystemExit(1)
pts = keep[0].get("points", [])
ticks = sorted(p.get("nanotick", p.get("tick")) for p in pts)
if ticks != [960000, 1920000]:
    print("  lane index:1 saved %r, wanted [960000, 1920000]" % (ticks,)); raise SystemExit(1)
PYC
echo "  persists: the emptied lane is gone from the file and the surviving one kept its points"

echo "automation_delete_check: PASS — a point can be removed, only the one named, and the lane
                         goes with its last point"
