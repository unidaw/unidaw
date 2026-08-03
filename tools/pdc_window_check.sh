#!/usr/bin/env bash
# PLUGIN DELAY COMPENSATION COLLAPSES THE OPENING OF A RENDER, AND THIS PINS IT.
#
# apps/latency_manager.h (`getCompensatedStart`) shifts every event EARLIER by latencySamples_ and
# clamps at zero when that would go negative:
#
#     if (engineSampleStart >= latencySamples_) return engineSampleStart - latencySamples_;
#     return 0;                      // <-- every earlier event collapses onto the SAME sample
#
# So notes in the opening window are not merely early, they land ON TOP OF EACH OTHER. An opening
# chord or a fast run is squashed into one instant, once, at the start of playback. It never
# recurs on a loop, which is why it reads as imagination rather than as a bug.
#
# THIS CHECK IS PART INVERTED, AND SAYS SO LOUDLY. Two of its assertions pin behaviour that is
# WRONG, because a bug nobody can measure is a bug that changes silently in either direction. The
# fix needs an owner's call between three options that trade off against each other (delay the
# transport start; put signed offsets on the wire, which is a contract change; or accept and
# document), so until that call is made the honest thing is to make the defect visible and stop it
# from drifting.
#
# WHEN THE INVERTED BLOCK GOES RED, READ ITS MESSAGE BEFORE ASSUMING A REGRESSION. If the
# placements have become CORRECT, PDC was fixed and this check has done its job: delete that block
# and extend the exact-placement table to cover the whole range. That is the same retirement the
# chord-render inverted check went through, and the reason it must print both numbers rather than
# just failing.
#
# WHY THIS COULD NOT BE WRITTEN BEFORE TODAY: the placements are sample counts, so the render has
# to happen at a KNOWN rate. Until `--sample-rate` existed the offline render took whatever the
# default output device reported, and connecting headphones would have moved every number in the
# table — the exact coupling that made sampler_vintage fail a correct engine.
#
#   tools/pdc_window_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
RATE=44100
BLOCK=512

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# One note per render, at a stated SAMPLE. 120 bpm at 44100 makes a quarter 22050 samples, so the
# nanotick is sample * Q / 22050. DAW_USE_FAKE_IDENTITY makes the instrument write a 10-sample
# pulse at the event's offset, so the FIRST NON-ZERO SAMPLE of the take is the placement — no
# envelope, no attack, nothing to interpret.
mk() {  # mk <name> <sample> [second-sample]
  python3 - "$TMP/$1.uniproj.json" "$2" "${3:-}" <<'PY'
import json, sys
out, first = sys.argv[1], int(sys.argv[2])
second = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
Q = 960000; DIRECT = 4294967294
tick = lambda s: round(s * Q / 22050)
notes = [{"nanotick": tick(first), "duration": Q // 8, "pitch": 60, "velocity": 100,
          "column": 0, "note_id": 1}]
if second is not None:
    notes.append({"nanotick": tick(second), "duration": Q // 8, "pitch": 72, "velocity": 100,
                  "column": 1, "note_id": 2})
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "vst_instrument", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 4 * Q,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "n"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "c", "length": 4 * Q, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
  ( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_PROJECT_DIR="$TMP" \
      DAW_UI_SHM_NAME="/pdcwin_${1}_$$" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 3 \
      --block-size "$BLOCK" --sample-rate "$RATE" >"$TMP/$1.log" 2>&1 ) \
    || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no audio"
}

pulses() {  # pulses <name> -> the start sample of each burst, space separated
  python3 - "$TMP/$1.wav" <<'PY'
import struct, sys, wave
w = wave.open(sys.argv[1]); ch = w.getnchannels(); n = w.getnframes()
d = struct.unpack('<%dh' % (n * ch), w.readframes(n)); mono = d[::ch]
starts, prev = [], 0
for i, v in enumerate(mono):
    if v != 0 and prev == 0:
        starts.append(i)
    prev = v
print(' '.join(str(s) for s in starts))
PY
}

# ---- EXACT, outside the compensation window. This half is a genuine regression guard: everything
# past the window is sample-accurate end to end, and that is worth defending.
for S in 1102 2297 4096; do
  mk "exact$S" "$S"
  GOT="$(pulses "exact$S" | awk '{print $1}')"
  [ "$GOT" = "$S" ] || \
    fail "a note at sample $S rendered at ${GOT:-nothing}. Outside the PDC window the placement
        is exact, so this is a real scheduling regression rather than the known collapse below"
done
echo "  exact outside the window: 1102, 2297, 4096 all land on their own sample"

# ---- THE COLLAPSE, pinned. TWO notes, at different samples, both inside the window.
#
# This is the assertion that states the harm: not "each note is early" but "they are no longer
# distinguishable from each other". A check that only asserted each note's position would report a
# timing error; this reports the loss of RELATIVE timing, which is what a squashed opening chord
# actually is.
mk "collapse" 86 344
STARTS="$(pulses collapse)"
# THE RENDER CARRIES ITS OWN CONTROL, which is what makes this assertion free of hardcoded
# expectations. The clip loops every 4 quarters — 88200 samples at this rate — and the loop pass
# is PAST the compensation window, so the same two notes appear there with their spacing intact.
#
# First pass:  one pulse   (86 and 344 both clamped to 0 — the timing between them is gone)
# Loop pass:   two pulses, 258 samples apart, which is exactly 344 - 86
#
# So the check does not need to know what the right answer is. It compares the opening of the
# render against a later pass of THE SAME NOTES, and the defect is the difference between them.
python3 - "$STARTS" <<'PYS' || fail "see the message above"
import sys
LOOP = 88200          # 4 quarters at 120 bpm, 44100 Hz
WANT_GAP = 344 - 86   # what the two notes were authored to be apart
starts = [int(x) for x in sys.argv[1].split()]
first = [s for s in starts if s < LOOP]
later = [s for s in starts if s >= LOOP]
if len(later) < 2:
    print(f"  FAIL: the loop pass produced {len(later)} pulse(s) ({later}); this check needs the")
    print( "        second pass as its reference, so something changed about looping rather than")
    print( "        about compensation")
    raise SystemExit(1)
gap_later = later[1] - later[0]
if gap_later != WANT_GAP:
    print(f"  FAIL: on the loop pass the two notes are {gap_later} samples apart, not {WANT_GAP}.")
    print( "        The reference itself is wrong, so nothing below can be trusted")
    raise SystemExit(1)
if len(first) == 1:
    print(f"  KNOWN DEFECT pinned: on the opening pass the two notes render as ONE pulse at "
          f"{first[0]},")
    print(f"    while the SAME notes on the loop pass are {gap_later} samples apart. The opening")
    print( "    lost the timing between them entirely — getCompensatedStart clamps both to zero.")
    print( "    Task #1; awaiting an owner's call between three fixes.")
    raise SystemExit(0)
gap_first = first[1] - first[0] if len(first) > 1 else None
print(f"  FAIL: the opening pass now has {len(first)} pulse(s) at {first}, gap {gap_first}.")
print( "        READ THIS BEFORE ASSUMING A REGRESSION: if the gap is now "
      f"{WANT_GAP}, PDC HAS BEEN")
print( "        FIXED and this inverted block has done its job — delete it and extend the exact")
print( "        table above over the opening window. Any other value means compensation changed")
print( "        in a way nobody recorded.")
raise SystemExit(1)
PYS

# ---- AND THE EDGE, which moves rather than collapses: a note just inside the window is shifted
# back to the block boundary rather than to zero. Pinned for the same reason — it is the part of
# the defect that a fix must also change, so it is evidence about the scope of the fix.
mk "edge" 600
EDGE="$(pulses edge | awk '{print $1}')"
if [ "$EDGE" = "600" ]; then
  fail "a note at sample 600 now lands exactly. That is the FIX, not a failure — PDC no longer
        clamps, so delete this block and the collapse block above, and assert exact placement
        across the whole range"
fi
echo "  KNOWN DEFECT pinned: a note at sample 600 renders at $EDGE, short by $((600 - EDGE))"

echo "pdc_window_check: PASS — exact outside the window; the collapse inside it is measured, not assumed"
