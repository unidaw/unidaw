#!/usr/bin/env bash
# A CLIP'S OWN GRID IS SETTABLE, AND SETTING IT IS VISIBLE.
#
# `ProjectClip` has carried `linesPerBeat` and a time signature since the grid moved off the track
# — "so each clip carries its own meter", says the struct. All three persist, all three publish
# packed into `UiClipExtent`'s flag bits, and the tracker draws the CLIP's grid BEFORE the
# track's. So the authority in that chain was the one thing no command could write: a verse in 4
# against a bridge in 3 is the case the manual describes, and it was reachable only by editing
# the project file by hand. Task #43 phase 2.
#
# THE LOAD-BEARING PROPERTY IS **PUBLISHED**, NOT **SAVED**, and that is not a guess about what
# might go wrong — it is what this command did when first written. The handler edited the clip and
# returned; the save carried 3 and 7/8 correctly and `get extents` still said 4 and 4/4, because
# the whole extent publish is gated on `clipVersion` moving and a grid change touches no note. An
# edit that saves and is never drawn is the same defect as one that is drawn and never saved, and
# a check asserting only the file would have shipped it.
#
# FIVE PROPERTIES:
#   PUBLISHED   the extent the UI reads carries the new grid — asserted FIRST, because this is
#               the half that broke
#   INDEPENDENT setting the meter alone leaves the subdivision alone. Zero cannot mean "leave it"
#               here: 0 is the packer's "no grid on this extent" sentinel, so the payload carries
#               a flag per field
#   PERSISTS    it survives a save and a reload, with the value moved away in between so
#               "the load restored it" and "nothing happened" are distinguishable
#   REFUSED     out of range is refused rather than clamped, at both ends and for the denominator
#               specifically: it is stored as a 3-bit EXPONENT, so a non-power-of-two is a caller
#               with the wrong idea and rounding hands back a meter nobody asked for
#   NO SUCH CLIP  a clip id that does not exist is refused by the ENGINE, with a reason
#
# No audio device needed: a grid is drawn, not heard.
#   tools/clip_grid_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# TWO CLIPS, because a command addressed by clip id cannot be shown to address the RIGHT one on a
# fixture with a single clip. Neither id is 1, so an id equal to the index or the count cannot pass
# by coincidence.
#
# ADDRESSING IS ASSERTED IN BOTH DIRECTIONS, further down, and that is deliberate: a handler that
# ignored the id entirely would edit whichever clip it met FIRST, and the runtime's clip order is
# not the file's — I tried pinning the order and the bug still landed on the right clip by luck.
# Editing 7 and checking 9, then editing 9 and checking 7, catches it whichever order the runtime
# happens to hold them in.
python3 - "$TMP/cg.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def clip(cid):
    return {"id": cid, "name": "c%d" % cid, "length": BAR, "kind": "symbolic",
            "lines_per_beat": 4, "time_sig_numerator": 4, "time_sig_denominator": 4,
            "notes": []}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 7, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 9, "id": 2, "at": BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "cg"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip(9), clip(7)], "tracks": [tr]}, open(out, "w"))
PY

SHM="/cgchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project cg --run-seconds 45 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# The grid the UI would DRAW for one clip, out of the published extents.
grid() {  # grid <clipId>
  cli get extents --track 0 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unreadable'); raise SystemExit
# A FLAT ARRAY of extents, one per placement — not grouped by track.
for e in (d if isinstance(d, list) else []):
    if e.get('clip') == $1:
        g = e.get('grid') or {}
        print('%s %s' % (g.get('lpb'), g.get('time_sig'))); raise SystemExit
print('missing')
" 2>/dev/null
}
waitgrid() {  # waitgrid <clipId> <want>
  for _ in $(seq 1 60); do
    [ "$(grid "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}

[ "$(grid 7)" = "4 4/4" ] || fail "clip 7 did not start at 4 and 4/4, it reads '$(grid 7)' — so
        the change below would prove nothing"
[ "$(grid 9)" = "4 4/4" ] || fail "clip 9 did not start at 4 and 4/4"
echo "  both clips start at 4 rows per beat, 4/4"

# ---- PUBLISHED. The half that broke when this command was first written.
cli do clip-grid --track 0 --clip 7 --lines 3 --num 7 --den 8 >/dev/null 2>&1
waitgrid 7 "3 7/8" || \
  fail "the published extent for clip 7 reads '$(grid 7)', wanted 3 and 7/8. The model may well
        have changed — it did when this failed for real, and the SAVE was correct — but the whole
        extent publish is gated on clipVersion moving, and a grid change touches no note. An edit
        that saves and is never drawn is as broken as one drawn and never saved"
echo "  published: clip 7 draws on 3 rows per beat in 7/8"

# ---- AND IT IS THE RIGHT CLIP. A fixture with one clip cannot make this assertion.
[ "$(grid 9)" = "4 4/4" ] || \
  fail "clip 9 moved to '$(grid 9)' — the command is addressed by clip id and reached the wrong
        clip, or reached all of them"
echo "  addressed: clip 9 is untouched"

# ---- THE OTHER DIRECTION, so the assertion does not depend on which clip the runtime met first.
cli do clip-grid --track 0 --clip 9 --lines 12 >/dev/null 2>&1
waitgrid 9 "12 4/4" || fail "addressing clip 9 did not reach it: it reads '$(grid 9)'"
[ "$(grid 7)" = "3 7/8" ] || \
  fail "editing clip 9 changed clip 7, which now reads '$(grid 7)'. Together with the assertion
        above this pins the addressing whichever order the runtime holds the clips in"
cli do clip-grid --track 0 --clip 9 --lines 4 >/dev/null 2>&1
waitgrid 9 "4 4/4" || fail "could not restore clip 9"
echo "  addressed both ways: editing 9 left 7 alone"

# ---- INDEPENDENT. Setting the meter must not silently reset the subdivision.
cli do clip-grid --track 0 --clip 7 --num 5 --den 4 >/dev/null 2>&1
waitgrid 7 "3 5/4" || \
  fail "after setting only the meter, clip 7 reads '$(grid 7)' and should be 3 and 5/4. The
        payload carries a flag per field precisely so this is expressible — 0 cannot mean 'leave
        it alone' when 0 is the packer's no-grid sentinel"
echo "  independent: the meter changed and the subdivision survived"

# ---- AND THE OTHER DIRECTION. Setting ONLY the subdivision must leave the meter alone.
#
# BOTH DIRECTIONS OR NEITHER: with only the first, a handler that ignored the NUMERATOR flag
# passed cleanly — every command this check sent up to here names the numerator, so writing it
# unconditionally changed nothing. Found by the negative control, not by the run. A per-field flag
# needs one assertion per field that can be left out.
cli do clip-grid --track 0 --clip 7 --lines 6 >/dev/null 2>&1
waitgrid 7 "6 5/4" || \
  fail "after setting only the subdivision, clip 7 reads '$(grid 7)' and should be 6 and 5/4. A
        lines-only call carries 0 in the meter fields, so a handler that writes them regardless of
        their flag silently sets the meter to 0/0"
cli do clip-grid --track 0 --clip 7 --lines 3 >/dev/null 2>&1
waitgrid 7 "3 5/4" || fail "could not restore the subdivision to 3 for the assertions below"
echo "  independent both ways: the subdivision changed and the meter survived"

# ---- REFUSED. Each bound is the packer's, not an opinion.
cli do clip-grid --track 0 --clip 7 --den 6 >/dev/null 2>&1 && \
  fail "a denominator of 6 was accepted. It is stored as a 3-bit EXPONENT, so it must be a power
        of two; rounding would hand back a meter nobody asked for"
cli do clip-grid --track 0 --clip 7 --lines 0 >/dev/null 2>&1 && \
  fail "--lines 0 was accepted, and 0 is the packer's sentinel for 'no grid on this extent'"
cli do clip-grid --track 0 --clip 7 --lines 32 >/dev/null 2>&1 && \
  fail "--lines 32 was accepted; the field is five bits"
[ "$(grid 7)" = "3 5/4" ] || \
  fail "a refused command still changed the grid: clip 7 reads '$(grid 7)'. A refusal that edits
        is not a refusal"
echo "  refused: 6 as a denominator, 0 and 32 as a subdivision — and the value did not move"

# ---- NO SUCH CLIP, refused by the ENGINE rather than by the CLI's own validation.
cli do clip-grid --track 0 --clip 999 --lines 5 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  grep -q '"event":"clip.grid_rejected"' "$TMP/eng.log" && break
  sleep 0.25
done
grep -q '"event":"clip.grid_rejected"' "$TMP/eng.log" || \
  fail "a clip id that does not exist was not refused by the engine — no clip.grid_rejected in
        the log. The CLI cannot cover this one, so without it the engine's guard is untested"
echo "  refused: a clip id that does not exist is rejected by the engine, with a reason"

# ---- PERSISTS, and comes back. Moved away first so a reload doing NOTHING is distinguishable.
cli do save cgout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/cgout.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/cgout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/cgout.uniproj.json" <<'PYC' || fail "the grid did not reach the saved project"
import json, sys
d = json.load(open(sys.argv[1]))
by = {c["id"]: c for c in d["clips"]}
c7, c9 = by.get(7, {}), by.get(9, {})
if (c7.get("lines_per_beat"), c7.get("time_sig_numerator"), c7.get("time_sig_denominator")) != (3, 5, 4):
    print("  clip 7 saved %r" % [c7.get("lines_per_beat"), c7.get("time_sig_numerator"),
                                 c7.get("time_sig_denominator")]); raise SystemExit(1)
if (c9.get("lines_per_beat"), c9.get("time_sig_numerator")) != (4, 4):
    print("  clip 9 saved %r, should be untouched" % [c9.get("lines_per_beat"),
                                                      c9.get("time_sig_numerator")])
    raise SystemExit(1)
raise SystemExit(0)
PYC
cli do clip-grid --track 0 --clip 7 --lines 6 --num 3 --den 4 >/dev/null 2>&1
waitgrid 7 "6 3/4" || fail "could not move the grid away before the reload test"
cli do load cgout --force >/dev/null 2>&1
waitgrid 7 "3 5/4" || \
  fail "after reloading, clip 7 reads '$(grid 7)' and the file says 3 and 5/4. The value was
        moved to 6 and 3/4 first, so this is the LOAD dropping it rather than nothing happening"
echo "  persists: saved 3 and 5/4, moved to 6 and 3/4, reloaded, back to 3 and 5/4"

echo "clip_grid_check: PASS — a clip's own grid is settable, drawn, addressed and saved"
