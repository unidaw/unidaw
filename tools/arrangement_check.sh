#!/usr/bin/env bash
# THE ARRANGEMENT: named positions, the song's meter, and inserting or removing time.
#
# Replaces section_ops_check + arrange_summary_check. The Section spine is gone (v29) and what
# replaced it is three things that each do one job, where every spine op did two:
#
#   MARKERS          name a tick. Total — they move no material and refuse nothing but a bad id.
#   THE METER MAP    the song's time signature, now AUTHORITATIVE and authorable by command. A
#                    Section's meter was reachable from NO command at all and honoured by nothing
#                    downstream, so mid-song meter was a stub only a hand-edited file could reach.
#   InsertRemoveTime the ripple, as its own command over a tick range.
#
# WHAT THIS PINS, and every one of them was a real defect or a real gap:
#
#   RESOLVED     a marker's BAR is published, prefix-summed through the meter. Not tick/barLength:
#                after a 7/8 change the two disagree, and a client deriving it would draw the
#                marker at the wrong bar with nothing reporting an error.
#   AUTHORABLE   `do time-sig` sets a mid-song meter, and it REACHES the ruler and the plugins.
#   REFUSED      an invalid signature (4/5 is a typo, not a meter) is refused rather than clamped.
#   CARRIED      a time edit moves markers AND meter points with everything else.
#   ON THE GRID  the inserted span is whole bars of the meter PRECEDING the edit point — the one
#                at the point moves away. Getting this wrong put a 7/8 change at 7.5 bars, which
#                TimeSignatureMap then snapped forward, silently parting it from its marker.
#   IDS         a marker id is never reused, and a duplicate in a file is repaired and REPORTED.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/arrangement_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((Q * 4))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
# THE ENGINE MUST DIE WHEN THIS CHECK DOES, including when ctest KILLS the check on a timeout.
# This trap used to remove $TMP and leave the engine running: it was only stopped on the normal
# path and inside fail(). A timed-out check therefore orphaned a possibly-hung engine, and ctest
# then blocked on it — measured at about 1000s per timeout across 18 runs, perfectly correlated
# with the timeout count. override showed it plainly: 909.87s against a TIMEOUT of 600, passing
# standalone in 23.2s.
#
# stop_engine escalates to SIGKILL after 10s and SAYS SO, so a hang stops being something to
# infer from a sample stack and becomes a line in the run.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# DUPLICATE AND ZERO MARKER IDS ON PURPOSE. A lookup returns the FIRST match, so a second marker
# sharing an id is unaddressable — renaming it renames the other one. The load must repair them
# and SAY it did, rather than quietly changing someone's document.
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
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      # Material well clear of the edit point, so the time edits below are not refused for
      # straddling — that refusal has its own coverage in markers_tests and ripple_undo_check.
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 1, "id": 2, "at": 12 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "ar"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "markers": [{"id": 1, "nanotick": 0, "name": "intro", "color_rgb": 0},
                       {"id": 1, "nanotick": 4 * BAR, "name": "verse", "color_rgb": 0},
                       {"id": 0, "nanotick": 8 * BAR, "name": "chorus", "color_rgb": 0}],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/archk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load ar --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
# POLL FOR THE ARRANGEMENT REGION. Every assertion below reads it, so a fixed sleep here is a bet
# on how busy the machine is.
# THE PREDICATE MUST REQUIRE CONTENT, NOT THE KEY. My first version grepped for '"markers"',
# which `get arrangement` prints even when the list is EMPTY — so it was satisfied instantly and
# the read still raced. It turned a check that mostly passed on a fixed sleep into one that failed
# in the gating run with "markers ... became []". A poll predicate needs the same scrutiny as an
# assertion: waiting for a key that is always present is not waiting at all.
arr_ready() {
  cli get arrangement 2>/dev/null | python3 -c 'import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(1)
raise SystemExit(0 if d.get("markers") else 1)'
}
wait_until 20 arr_ready || true

# Read the published arrangement through python rather than grepping it: a grep that matches the
# wrong line is how a check passes with the bug present.
arr() { cli get arrangement 2>/dev/null; }
field() {  # field <jq-ish path expr>, JSON on stdin
  python3 -c "
import json, sys
d = json.load(sys.stdin)
print($1)
"
}

# ---- IDS REPAIRED, AND REPORTED. Two of the three were unaddressable as written.
grep -q '"event":"markers.ids_repaired"' "$TMP/eng.log" || \
  fail "a file with a duplicate and a zero marker id loaded without repairing them. A lookup
        returns the FIRST match, so the second marker sharing an id could never be renamed,
        moved or removed — the command would silently hit the other one"
IDS="$(arr | field '",".join(str(m["id"]) for m in d["markers"])')"
[ "$(echo "$IDS" | tr ',' '\n' | sort -u | wc -l | tr -d ' ')" = "3" ] || \
  fail "marker ids are still not unique after the repair: $IDS"
echo "  ids: a duplicate and a zero id were repaired and reported ($IDS)"

# ---- RESOLVED BARS, in one meter first. Markers at ticks 0, 4 and 8 bars.
BARS="$(arr | field '",".join(str(m["bar"]) for m in d["markers"])')"
[ "$BARS" = "1,5,9" ] || \
  fail "markers at 0, 4 and 8 bars published bars [$BARS], expected 1,5,9"
echo "  resolved: bars are published, one-based, prefix-summed ($BARS)"

# ---- AUTHORABLE MID-SONG METER, which is the capability the spine never had. 7/8 from bar 5.
cli do time-sig --sig 7/8 --nanotick $((4 * BAR)) >/dev/null 2>&1 || true
sleep 1.2
grep -q '"event":"time_sig.set"' "$TMP/eng.log" || \
  fail "SetTimeSignature did not apply. A Section's meter was reachable from no command at all,
        so this is the whole point of the replacement: $(grep -o '"event":"time_sig[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
METER="$(arr | field '",".join("%d:%s/%s" % (p["nanotick"], *p["sig"].split("/")) for p in d["time_sig"])')"
[ "$METER" = "0:4/4,$((4 * BAR)):7/8" ] || \
  fail "the meter map reads [$METER], expected 0:4/4,$((4 * BAR)):7/8"
echo "  authorable: a mid-song 7/8 was set by command and published"

# ---- AND THE BAR NUMBERS FOLLOW IT. This is the assertion that separates a real meter map from
# a decorative one: after a 7/8 change, a marker's bar is NOT tick / barLength(4/4).
#
# Marker 3 is at 8*BAR. Bars 1-4 are 4/4; from 4*BAR the meter is 7/8 (3.5 quarters = 3360000).
# (8*BAR - 4*BAR) / 3360000 = 15360000 / 3360000 = 4.571..., so 4 whole 7/8 bars fit before it:
# bar 5 + 4 = bar 9... but the map SNAPS a change forward to a bar line, and 4*BAR is already one,
# so the arithmetic stands. What a naive tick/barLength(4/4) would say is 9 as well — so this
# fixture would not discriminate. Ask for a tick that DOES: 4*BAR + 2 * 3360000 is bar 7 under
# the real map and bar 6 under the naive one.
cli do marker add --nanotick $((4 * BAR + 2 * 3360000)) --name probe >/dev/null 2>&1 || true
sleep 1.2
PROBE="$(arr | field 'next(str(m["bar"]) for m in d["markers"] if m["name"] == "probe")')"
[ "$PROBE" = "7" ] || \
  fail "a marker two 7/8 bars past the meter change published bar $PROBE, expected 7. Bar 6 means
        the bar number is tick / barLength at ONE signature — the prefix sum across the change is
        exactly what the published bar exists to do, and getting it wrong draws every marker after
        a meter change in the wrong place"
echo "  follows the meter: a marker two 7/8 bars past the change is bar 7, not 6"

# ---- AN INVALID SIGNATURE IS REFUSED, not clamped. Silently turning 4/5 into 4/4 puts the ruler
# somewhere the caller never asked for.
cli do time-sig --sig 4/5 --nanotick $((8 * BAR)) >/dev/null 2>&1 || true
sleep 1.1
grep '"event":"time_sig.rejected"' "$TMP/eng.log" | grep -q '"reason":"invalid_signature"' || \
  fail "4/5 was accepted. A denominator must be a power of two; clamping a typo to 4/4 would put
        the ruler somewhere nobody asked for"
AFTER_BAD="$(arr | field 'len(d["time_sig"])')"
[ "$AFTER_BAD" = "2" ] || fail "the refused signature still changed the map ($AFTER_BAD points)"
echo "  refuses: 4/5 is refused by name and changes nothing"

# ---- A TIME EDIT CARRIES MARKERS AND METER POINTS. Insert 2 bars at bar 3 — before the 7/8
# change, so the change and every later marker must move, and 'intro' at tick 0 must not.
BEFORE_M="$(arr | field '",".join("%s@%d" % (m["name"], m["nanotick"]) for m in d["markers"])')"
cli do time insert --nanotick $((2 * BAR)) --bars 2 >/dev/null 2>&1 || true
sleep 1.4
grep -q '"event":"time.edited"' "$TMP/eng.log" || \
  fail "the time edit was not applied: $(grep -o '"event":"time[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
grep '"event":"time.edited"' "$TMP/eng.log" | tail -1 | grep -q '"meter_points_moved":1' || \
  fail "the time edit did not report carrying the meter point. A meter change left behind while
        the material moves is the bug that got the tick-keyed map deleted in the first place"
AFTER_M="$(arr | field '",".join("%s@%d" % (m["name"], m["nanotick"]) for m in d["markers"])')"
[ "$AFTER_M" != "$BEFORE_M" ] || fail "the time edit moved no markers at all"
INTRO="$(arr | field 'next(str(m["nanotick"]) for m in d["markers"] if m["name"] == "intro")')"
[ "$INTRO" = "0" ] || fail "'intro' is before the edit point and must not have moved (at $INTRO)"
VERSE="$(arr | field 'next(str(m["nanotick"]) for m in d["markers"] if m["name"] == "verse")')"
[ "$VERSE" = "$((6 * BAR))" ] || \
  fail "'verse' was at 4 bars; inserting 2 bars before it should put it at 6 bars
        ($((6 * BAR))), got $VERSE"
echo "  carries: markers and the meter point moved with the material, earlier ones did not"

# ---- AND THE METER CHANGE IS STILL ON A BAR LINE. The inserted span must be whole bars of the
# meter PRECEDING the point. Taking the meter AT the point instead lands the change off the grid,
# and TimeSignatureMap then snaps it forward — silently parting it from the marker beside it.
SIGTICK="$(arr | field 'str([p["nanotick"] for p in d["time_sig"] if p["nanotick"] > 0][0])')"
[ $((SIGTICK % BAR)) = "0" ] || \
  fail "after the insert the 7/8 change sits at $SIGTICK, which is not a bar line"
[ "$SIGTICK" = "$VERSE" ] || \
  fail "the 7/8 change ($SIGTICK) and the 'verse' marker ($VERSE) parted company — they were at
        the same tick and a time edit must carry both by the same amount"
echo "  on the grid: the meter change is still bar-aligned, and still with its marker"

# ---- IT SURVIVES A SAVE AND A RELOAD IN A FRESH ENGINE. The meter map is authoritative now, so
# the save writes it as it is — it used to be derived from the spine, which destroyed a real
# multi-point map on any project that had one.
cli do save arout --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

SHM2="/archk2_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/eng2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load arout --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng2.log" "$ENG" 80
# Same again for the RELOADED engine — this one compares against what the first run published, so
# an early read here reports a save/reload mismatch that is pure timing.
wait_until 20 arr_ready || true
RELOAD_M="$(arr | field '",".join("%s@%d" % (m["name"], m["nanotick"]) for m in d["markers"])')"
RELOAD_S="$(arr | field '",".join("%d:%s" % (p["nanotick"], p["sig"]) for p in d["time_sig"])')"
[ "$RELOAD_M" = "$AFTER_M" ] || \
  fail "markers changed across save/reload: [$AFTER_M] became [$RELOAD_M]"
case "$RELOAD_S" in
  *"7/8"*) : ;;
  *) fail "the mid-song 7/8 did not survive the round trip: [$RELOAD_S]. The save used to DERIVE
        this map from the spine, which flattened a real multi-point map to one point" ;;
esac
echo "  survives: markers and the mid-song meter come back in a fresh engine"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "arrangement_check: PASS — markers name ticks, the meter is authorable, and a time edit carries both"
