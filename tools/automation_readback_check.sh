#!/usr/bin/env bash
# AUTOMATION YOU CANNOT READ IS A LANE YOU DRAW INTO AND NEVER SEE.
#
# Automation playback shipped in Movement 3 phase 1; WriteAutomationPoint came later; and nothing
# in the observation header ever mentioned automation at all. So a UI could send points and had no
# way to ask what was there — blank lanes over a song that plays the sweep you authored, and no way
# to tell a write that was accepted from one that was dropped. The frontend agent raised it before
# building the lane, which is the right time: the command was the easy half.
#
# v28 publishes two shapes for two questions. The LANE LIST is standing and version-gated, so
# "which params are automated" needs no request. The POINTS are answered per request into a seqlock
# slot the CALLER addresses, because a song can hold far more automation than a fixed region could
# carry and a UI only draws the lanes it has open.
#
# WHAT IS DELIBERATELY NOT PUBLISHED: the resolved value at the playhead. Interpolation belongs to
# whoever draws the curve. A published resolved value would be a SECOND implementation of it that
# can disagree with what plays, and "two answers to what is the cutoff at bar 9" is the class of
# bug this whole read-back exists to make visible.
#
# SIX PROPERTIES:
#   LISTS      the lane list names the automated params, per track, with their point counts
#   POINTS     a requested lane comes back with the ticks and values that were written
#   ANSWERS    a param nothing automates returns found:false — an ANSWER, not silence
#   RIPPLES    after SetSectionLength, the read-back shows the points at their MOVED ticks.
#              This is the one the frontend asked for by name, and the reason it matters is
#              specific: the read-back is published from the RT SNAPSHOT, the same copy the
#              scheduler reads. The bug that was found here once had the ripple move the points
#              in the model and in the saved file while the snapshot stayed put — right on disk,
#              wrong in your ears. A read-back taken from the model would have agreed with the
#              file and certified the bug. Taken from the snapshot, it catches it.
#
#              BOTH DIRECTIONS MEASURED, because this is the trap that keeps recurring here.
#              Comment out the ripple's snapshot republish and this check FAILS, reporting the
#              points at their old ticks. Leave that bug in place and switch the read-back to
#              publish rt->track instead, and the whole suite goes GREEN over a broken engine —
#              seven properties, all passing, sweep in the wrong place. The choice of source is
#              the entire difference between a check and a rubber stamp.
#   CACHES     the version moves on an automation write and does NOT move on a note edit, so
#              caching on it is safe and is not invalidated by ordinary typing
#   REFUSES    a removal of bars that hold automation is refused, by lane name. The refusal
#              guarded placements only; automation is material by the same argument, and worse,
#              a point at the boundary collapses onto the one at the new end because addPoint
#              replaces. Measured without the guard: two points became one
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/automation_readback_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((Q * 4))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# TWO markers and TWO automation lanes on one track. Two markers show the ripple carries them;
# two lanes is the minimum that can show the list is a list rather than a hardcoded first answer.
# A point before the boundary and points after it, so the ripple has to move some and not others.
python3 - "$TMP/ar.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1}]}
# `cutoff` straddles the edit point (bar 5 = tick 4*BAR): one point before it,
# two after it. `res` is entirely after the boundary, so it moves whole.
auto = [
    {"param_id": "cutoff", "target_plugin_index": 4294967295, "discrete": False,
     "points": [{"nanotick": 2 * BAR, "value": 0.1},
                {"nanotick": 5 * BAR, "value": 0.5},
                {"nanotick": 7 * BAR, "value": 0.9}]},
    {"param_id": "res", "target_plugin_index": 4294967295, "discrete": True,
     "points": [{"nanotick": 6 * BAR, "value": 0.25}]},
]
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "automation": auto,
      # Placements are what the ripple REFUSES over: a shrink into occupied bars is rejected, so
      # keep the material before the edit point and insert rather than remove.
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
markers = [{"id": 1, "nanotick": 0, "name": "intro", "color_rgb": 0},
           {"id": 2, "nanotick": 4 * BAR, "name": "verse", "color_rgb": 0}]
json.dump({"schema_version": 4, "meta": {"name": "ar"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "markers": markers, "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/archk_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load ar --force >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  if grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
sleep 1.5

# Read helpers. Each shells out to python3 over the CLI's JSON rather than grepping it: a grep that
# matches the wrong line is how a check passes with the bug present.
lanes_json() { cli get automation 2>/dev/null; }
# JSON on stdin, so the script goes in -c: a heredoc IS stdin, and `python3 - <<EOF` with a pipe
# reads the script where it expected the data. That cost two lanes reported as MISSING against a
# working engine, which is exactly the direction a test must never fail in.
lane_field() {  # lane_field <track> <param> <field>, JSON on stdin
  python3 -c '
import json, sys
doc = json.load(sys.stdin)
track, param, field = int(sys.argv[1]), sys.argv[2], sys.argv[3]
for lane in doc.get("lanes", []):
    if lane.get("track_id") == track and lane.get("param") == param:
        print(lane.get(field))
        break
else:
    print("MISSING")
' "$1" "$2" "$3"
}
points_of() {  # points_of <track> <param> -> "tick:value tick:value" (or NOTFOUND)
  cli get automation-points --track "$1" --param "$2" 2>/dev/null | python3 -c '
import json, sys
try:
    a = json.load(sys.stdin)
except Exception:
    print("NOANSWER"); raise SystemExit
if not a.get("found"):
    print("NOTFOUND"); raise SystemExit
if a.get("points_truncated"):
    print("TRUNCATED"); raise SystemExit
print(" ".join("%d:%.3f" % (p["nanotick"], p["value"]) for p in a["points"]))
'
}

# ---- LISTS. Both lanes, on the right track, with their real point counts, and the discrete flag
# read back per lane (one clip is discrete and one is not, so a hardcoded answer fails either way).
LJ="$(lanes_json)"
echo "$LJ" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null || \
  fail "get automation did not return JSON — the lane region is missing or unreadable:
        $(echo "$LJ" | head -3)"
TRUNC="$(echo "$LJ" | python3 -c 'import json,sys; print(json.load(sys.stdin)["lanes_truncated"])')"
[ "$TRUNC" = "0" ] || fail "the lane list reports $TRUNC lanes dropped for a 2-lane project"
CC="$(echo "$LJ" | lane_field 0 cutoff points)"
RC="$(echo "$LJ" | lane_field 0 res points)"
[ "$CC" = "3" ] || \
  fail "lane 'cutoff' on track 0 reports $CC points, expected 3. MISSING means the lane list does
        not include it at all, which is the state that made an automation UI impossible to build"
[ "$RC" = "1" ] || fail "lane 'res' on track 0 reports $RC points, expected 1"
CD="$(echo "$LJ" | lane_field 0 cutoff discrete)"
RD="$(echo "$LJ" | lane_field 0 res discrete)"
[ "$CD" = "False" ] || fail "'cutoff' is a continuous clip but publishes discrete=$CD"
[ "$RD" = "True" ] || \
  fail "'res' is a discrete clip but publishes discrete=$RD — discreteOnly is fixed at creation
        and decides whether the curve steps or ramps, so a UI that cannot read it draws the wrong
        shape for half the lanes"
echo "  lists: both lanes, with point counts and the per-lane discrete flag"

# ---- POINTS. Ticks and values as written, in tick order.
WANT="$((2 * BAR)):0.100 $((5 * BAR)):0.500 $((7 * BAR)):0.900"
GOT="$(points_of 0 cutoff)"
[ "$GOT" = "$WANT" ] || fail "points for 'cutoff' came back as [$GOT], expected [$WANT]"
echo "  points: the requested lane's ticks and values match what was authored"

# ---- ANSWERS. A param nothing automates is answered, not ignored.
NOPE="$(points_of 0 nosuchparam)"
[ "$NOPE" = "NOTFOUND" ] || \
  fail "asking for a param nothing automates returned [$NOPE], expected NOTFOUND. 'No such lane'
        has to be an ANSWER: a request that is silently dropped is indistinguishable from one the
        engine never received, and a caller cannot tell 'nothing there' from 'still waiting'"
echo "  answers: an unautomated param returns found:false rather than nothing at all"

# ---- CACHES, part 1: a NOTE edit must not move the automation version. The whole value of the
# version is that a UI can cache lanes on it; if typing moved it, every keystroke would invalidate
# every lane and the number would be worthless.
V0="$(echo "$LJ" | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')"
cli do note --track 0 --nanotick "$Q" --pitch 64 --velocity 90 >/dev/null 2>&1 || true
sleep 1.2
V1="$(lanes_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')"
[ "$V0" = "$V1" ] || \
  fail "a note edit moved the automation version ($V0 -> $V1). It is the clip version's job to
        move on a note; an automation version that also moves is one a UI cannot cache on"
echo "  caches: a note edit leaves the automation version alone ($V0)"

# ---- CACHES, part 2: an automation WRITE must move it.
cli do automation --track 0 --param cutoff --nanotick "$((3 * BAR))" --value 0.7 >/dev/null 2>&1 || true
sleep 1.2
V2="$(lanes_json | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')"
[ "$V2" != "$V1" ] || \
  fail "writing an automation point did NOT move the version ($V1), so a cached lane list stays
        stale forever and the point you just drew never appears"
GOT2="$(points_of 0 cutoff)"
case "$GOT2" in
  *"$((3 * BAR)):0.700"*) : ;;
  *) fail "the point just written is not in the read-back: [$GOT2]" ;;
esac
echo "  caches: an automation write moves the version ($V1 -> $V2) and the point is readable"

# ---- RIPPLES. Insert 2 bars at bar 5, and every point at or after that point
# must move by 2 bars, while the points before it stay. Asserted against the
# READ-BACK, which is published from the RT snapshot — so this fails if the ripple moves the
# model and the file but not what plays.
cli do time insert --nanotick 15360000 --bars 2 >/dev/null 2>&1 || true
sleep 1.5
grep -q '"event":"time.edited"' "$TMP/eng.log" || \
  fail "the time edit was not applied: $(grep -o '"event":"time[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"

# Expected after a +2 bar insert at the bar-4 boundary:
#   2*BAR (inside section 1)  stays
#   3*BAR (inside section 1)  stays        <- the point written above
#   5*BAR -> 7*BAR
#   7*BAR -> 9*BAR
WANT_R="$((2 * BAR)):0.100 $((3 * BAR)):0.700 $((7 * BAR)):0.500 $((9 * BAR)):0.900"
GOT_R="$(points_of 0 cutoff)"
[ "$GOT_R" = "$WANT_R" ] || \
  fail "after growing section 1 by 2 bars the read-back shows [$GOT_R], expected [$WANT_R].
        Points before the boundary must stay and points at or after it must move by the delta —
        the same rule the placements follow, or the notes and the sweep drift apart by exactly
        the size of the edit and nothing reports it"
WANT_RES="$((8 * BAR)):0.250"
GOT_RES="$(points_of 0 res)"
[ "$GOT_RES" = "$WANT_RES" ] || \
  fail "the 'res' lane sat entirely after the boundary and should have moved whole to
        [$WANT_RES], but reads [$GOT_RES]"
echo "  ripples: the read-back shows the moved ticks — the RT snapshot rippled, not just the model"

# ---- A REMOVAL OF BARS HOLDING AUTOMATION IS REFUSED. The refusal guarded PLACEMENTS only, and automation
# is material by the same argument: rippleTick moves what is at or after the boundary, so a sweep
# inside the removed bars stays where it is while the later section boundaries slide over it —
# re-sectioned, with no point changed and nothing to see. And worse for automation specifically:
# a point AT the old boundary lands on the new end, and addPoint REPLACES at an existing tick, so
# the shrink silently destroys one of the two.
#
# MEASURED WITHOUT THE GUARD: two points at bars 7 and 9 became ONE point, keeping the later
# value. There is a point at 3*BAR, so removing 3 bars ending at 6*BAR vacates [3*BAR, 6*BAR]
# and must be refused.
BEFORE_SHRINK="$(points_of 0 cutoff)"
# Removing 3 bars ending at bar 7 vacates [3*BAR, 6*BAR], which holds a point.
cli do time remove --nanotick 23040000 --bars 3 >/dev/null 2>&1 || true
sleep 1.4
grep '"event":"time_edit.rejected"' "$TMP/eng.log" | grep -q '"reason":"automation_in_removed_bars"' ||   fail "removing bars that hold automation was NOT refused. What happens instead
        is silent: the sweep stays put, the markers slide over it, and a point at the boundary
        collapses onto the one already at the new end — one of the two is gone with no undo entry
        that would put it back: $(grep -o '"event":"time[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
grep '"event":"time_edit.rejected"' "$TMP/eng.log" | tail -1 | grep -q '"param":"cutoff"' ||   fail "the refusal did not name the lane in the way. 'Something is in the way' is not actionable
        when the something is one lane out of sixty"
AFTER_SHRINK="$(points_of 0 cutoff)"
[ "$AFTER_SHRINK" = "$BEFORE_SHRINK" ] ||   fail "the refused removal still changed the automation: [$BEFORE_SHRINK] became [$AFTER_SHRINK].
        A refusal has to be whole — a half-applied ripple is a corrupted arrangement"
echo "  refuses: a removal of bars holding automation is refused by lane name, and changes nothing"

# ---- AND THE FILE AGREES. The read-back and the save are two views of the same edit; if they
# disagree, one of them is the bug, and the point of checking both is that neither can hide it.
cli do save arout --force >/dev/null 2>&1 || true
sleep 1.6
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
FILE_R="$(python3 - "$TMP/arout.uniproj.json" <<'PYF'
import json, sys
doc = json.load(open(sys.argv[1]))
for t in doc.get("tracks", []):
    for a in t.get("automation", []):
        if a.get("param_id") == "cutoff":
            print(" ".join("%d:%.3f" % (p["nanotick"], p["value"]) for p in a["points"]))
            raise SystemExit
print("MISSING")
PYF
)"
[ "$FILE_R" = "$WANT_R" ] || \
  fail "the saved file has [$FILE_R] but the read-back had [$WANT_R]. The two views of one edit
        disagree, which means the model and the RT snapshot are out of step in one direction or
        the other — and whichever way round it is, the sweep is in the wrong place somewhere"
echo "  agrees: the saved file matches the read-back tick for tick"

echo "automation_readback_check: PASS — lanes and points are readable, and the ripple moves what plays"
