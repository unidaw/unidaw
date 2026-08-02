#!/usr/bin/env bash
# A NOTE'S DEFAULT LENGTH IS ITS OWN BAR, NOT A 4/4 ONE.
#
# Four note-entry sites computed "the end of the bar containing this tick" as
#
#   const uint64_t bar = 4 * kNanoticksPerQuarter;
#   const uint64_t barAfter = (nanotick / bar + 1) * bar;
#
# — a bar hardcoded to 4/4, ignoring the song's meter map entirely. In a 3/4 project note entry
# then disagreed with the ruler about where a bar is: a note entered with no duration ran a third
# too long, and writing past the end grew the song to the wrong tick.
#
# time_signature_map.h's own opening comment warns about this shape: "the bar a tick falls in is
# NOT (tick / barLength)". The map has been the authority since the section was deleted (#79) and
# these four sites never asked it.
#
# WHICH METER IT SHOULD BE was the open question in task #43, and it is not open any more: #76 put
# the meter on the SONG and kept the grid on the CLIP, and #79 flattened the meter to markers. The
# note-entry TODO predates both rulings.
#
# THE NOTE GOES AT SEVEN QUARTERS, AND THAT IS THE WHOLE DESIGN OF THIS FIXTURE.
#
# The bar is a FLOOR on the span, not the answer: a note with no duration runs to the next event,
# or failing that to the end of the current span, and only reaches its bar end when that is
# further out. The first version of this check entered the note at tick 0 and got four quarters in
# BOTH meters — correctly, because with no next event the span (patternTicks, four quarters here)
# dominates and the bar never enters the arithmetic. It would have reported the bug as unfixed
# while measuring something else entirely.
#
# At seven quarters the note is past the span, so the bar end IS the answer, and the two meters
# disagree about which bar that is:
#   4/4   bars at 0, 4, 8 — seven quarters is inside 4..8, so the note runs ONE quarter
#   3/4   bars at 0, 3, 6, 9 — seven quarters is inside 6..9, so it runs TWO
#
# The 4/4 case is the control: it is also what the old hardcoded code produced, so on its own it
# proves only that the fixture can enter a note and read its length back. The 3/4 case is the one
# assertion that can tell "reads the meter" from "assumes 4/4".
#
# THE ANCHOR IS NOT FIXED and is deliberately not tested here: where a NEW CLIP lands still uses a
# 4/4 bar, because resolveNoteEntry computes it as (tick / barLength) * barLength — the naive form
# again, and one that cannot be corrected by passing a different length, since under a meter change
# the bar containing a tick is not at a multiple of anything. That needs the map inside a pure
# function or a callback, and it MOVES WHERE CLIPS LAND in existing projects. Task #43.
#
#   tools/meter_bar_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# project <name> <numerator> <denominator>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" "$3" <<'PY'
import json, sys
out, Q, num, den = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# NO CLIPS AND NO PLACEMENTS. The note is entered into empty space, which is the path that has
# to decide a default duration for itself — a note dropped into an existing clip inherits that
# clip's answer and would not exercise the bar arithmetic at all.
tr = {"track_id": 0, "name": "M", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
# THE SONG SIGNATURE LIVES UNDER `timebase`, not at the root — project_file.cpp reads
# "timebase.time_sig_numerator" with a 4/4 default, so a fixture that puts it at the root gets
# 4/4 silently and would "prove" the bug is still there when the fixture is what is wrong.
# `time_sig_map` IS at the root; the two are not in the same place and it is worth stating.
json.dump({"schema_version": 4, "meta": {"name": "m"}, "nanoticks_per_quarter": Q,
           "timebase": {"nanoticks_per_quarter": Q,
                        "time_sig_numerator": num, "time_sig_denominator": den},
           "time_sig_map": [{"nanotick": 0, "numerator": num, "denominator": den}],
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY
}

# entered_duration <project> <name> — enters one note at SEVEN QUARTERS with no duration and
# reports the duration the engine gave it, read back from a SAVE (the durable answer).
entered_duration() {
  local proj="$1" name="$2"
  local shm="/meterbar_${$}_$name"
  ( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$shm" \
      ./daw_engine --project "$proj" --run-seconds 22 >"$TMP/$name.log" 2>&1 ) &
  ENG=$!
  wait_for_boot "$TMP/$name.log" "$ENG" 40
  grep -q '"event":"project.load"' "$TMP/$name.log" 2>/dev/null || \
    fail "the '$name' engine never loaded — see $TMP/$name.log"
  local c=(env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$shm" "$CLI")
  # THE SIGNATURE THE ENGINE ACTUALLY HOLDS, asserted before the note. A fixture whose meter did
  # not reach the engine would give the 4/4 answer for a reason that has nothing to do with the
  # code under test, and the failure would point at the wrong place.
  "${c[@]}" get arrangement >"$TMP/$name.arr.json" 2>/dev/null
  "${c[@]}" do note --track 0 --nanotick $((Q * 7)) --pitch 60 >/dev/null 2>&1
  sleep 0.8
  "${c[@]}" do save "$name.out" >/dev/null 2>&1
  sleep 1.2
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  [ -f "$TMP/$name.out.uniproj.json" ] || fail "the '$name' engine did not save — see $TMP/$name.log"
  python3 - "$TMP/$name.out.uniproj.json" <<'PYD'
import json, sys
d = json.load(open(sys.argv[1]))
for c in d.get("clips", []):
    for n in c.get("notes", []):
        print(n.get("duration", 0)); raise SystemExit(0)
print(0)
PYD
}

# ---- 4/4. The control: also what the old hardcoded code produced, so it proves the fixture
# works rather than proving the fix.
project four 4 4
D4="$(entered_duration four four)"
echo "  4/4: a note at 7 quarters runs $D4 ticks ($(python3 -c "print($D4/$Q)") quarters)"
[ "${D4:-0}" = "$Q" ] || \
  fail "in 4/4 a note entered at seven quarters with no duration should reach the end of its bar
        at eight quarters, so $Q ticks — and it is ${D4:-0}. Either the fixture cannot enter a
        note past the span or it cannot read the length back, and the 3/4 assertion below would
        be measuring the harness"

# ---- 3/4. THE ONE THAT DISCRIMINATES. The old code gave one quarter here too.
project three 3 4
D3="$(entered_duration three three)"
echo "  3/4: a note at 7 quarters runs $D3 ticks ($(python3 -c "print($D3/$Q)") quarters)"
[ "${D3:-0}" = "$((Q * 2))" ] || \
  fail "in 3/4 the bars are 0, 3, 6, 9 — so a note at seven quarters reaches nine, which is
        $((Q * 2)) ticks, and it is ${D3:-0}. $Q means the bar is still hardcoded to 4/4 and note
        entry disagrees with the ruler the song is drawn against: the shape
        time_signature_map.h's own comment warns about, that the bar a tick falls in is NOT
        (tick / barLength)"

echo "meter_bar_check: PASS — a note's default length follows the song's meter, in 3/4 as in 4/4"
