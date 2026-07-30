#!/usr/bin/env bash
# RE-CUT THE CHOP WHILE IT PLAYS, AND EVERY EXISTING ROW STILL SOUNDS BIT-IDENTICAL.
#
# This is docs/SAMPLER_DESIGN.md §5.1 — the headline claim of the whole design — turned into an
# assertion. Renoise re-chops live but addresses slices by INDEX, so inserting a marker silently
# reassigns every note downstream: the part you wrote plays different audio and nothing reports
# it. Here slices have stable IDS, an insert mints a NEW one and shortens its predecessor by
# DERIVATION, and nothing is renumbered.
#
# THE ASSERTION IS BIT-IDENTITY, NOT SIMILARITY. "Sounds about the same" is what a design with
# index addressing would also produce most of the time — the failure is that ONE note moves, and
# an approximate comparison is exactly what hides it. So: render, insert a marker upstream,
# render again, and the samples of the untouched slices are the same numbers.
#
# FOUR PROPERTIES:
#   STABLE       inserting a marker leaves every OTHER slice's audio byte-for-byte identical
#   DERIVED      the predecessor shortens, so the new cut is audible where it was made
#   ADDRESSED    a note plays its slice by ID, from a key the keymap does not map there
#   GONE IS GONE removing a slice makes the notes addressing it SILENT, not "the whole sample"
#
# Rendered OFFLINE. No audio device needed.
#   tools/sampler_slice_stability_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A "break": four 0.25 s segments at four DIFFERENT frequencies, so which slice sounded is
# identifiable from the audio rather than assumed. A uniform tone would let a slice that played
# the wrong region pass perfectly.
python3 - "$TMP/break.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
seg = sr // 4
freqs = [220.0, 330.0, 495.0, 740.0]
frames = []
for f in freqs:
    for i in range(seg):
        frames.append(struct.pack('<h', int(18000 * math.sin(2 * math.pi * f * i / sr))))
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(frames)); w.close()
PY

# project <name> <markers-json> <notes-json>
# Slots 1..N each name a SLICE by id; the keymap puts them on keys 60..63.
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP/break.wav" "$2" "$3" "$Q" <<'PY'
import json, sys
out, wav, markers, notes, Q = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), json.loads(sys.argv[4]), int(sys.argv[5])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
# THE SLOTS ARE FIXED: slice ids 1, 2 and 3 on keys 60, 61 and 62, in BOTH renders. That is the
# whole point — the rows and the kit do not change, only the marker list does.
#
# The first version of this generator made one slot per MARKER, so adding a marker shifted every
# key's meaning: key 61 played slice 2 before and slice 4 after. The check failed, correctly, and
# the bug was in the FIXTURE — which is worth noting because it is exactly the failure the design
# exists to prevent, reproduced by accident in the thing meant to detect it.
slots = []
for i, sid in enumerate([1, 2, 3]):
    slots.append({"id": i + 1, "name": "sl%d" % (i + 1), "source_local_id": 1,
                  "slice_id": sid,
                  "start_frame": 0, "end_frame": 0,
                  "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
                  "loop_mode": 0, "sustain_loop": 0,
                  "key_low": 60 + i, "key_high": 60 + i, "root_key": 60 + i,
                  "pitch_track_milli": 0, "tune_cents": 0,
                  "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
                  "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
                  "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
                  "mod_set_id": 1, "output_stem": 0, "quality": 1})
sampler = {
    "next_slot_id": len(slots) + 1, "next_source_id": 2,
    "next_mod_set_id": 2, "stem_count": 0, "voice_cap": 32, "default_view": 1,
    "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
    "slice_sets": [{"source_local_id": 1,
                    "next_marker_id": max([m["id"] for m in markers] + [0]) + 1,
                    "markers": [{"id": m["id"], "frame": m["frame"], "tune_cents": 0,
                                 "reverse": 0, "mod_set_id": 0} for m in markers]}],
    "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                  "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
    "slots": slots,
}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
clip = {"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "sl"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
}

render() {
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/slchk_$$_$1" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 8 --block-size 256 \
      >"$TMP/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}

# Three markers at the segment boundaries, so slices 1..3 are the 2nd, 3rd and 4th segments.
# (Segment 1 is before the first marker and is not addressable as a slice here — the first slice
# begins at the first MARKER, and frame 0 is a start, not a boundary.)
M='[{"id":1,"frame":12000},{"id":2,"frame":24000},{"id":3,"frame":36000}]'
# One note per slice, half a second apart, each on the key its slot occupies.
N='[{"nanotick":960000,"duration":240000,"pitch":60,"velocity":110,"column":0,"note_id":1},{"nanotick":1920000,"duration":240000,"pitch":61,"velocity":110,"column":0,"note_id":2},{"nanotick":2880000,"duration":240000,"pitch":62,"velocity":110,"column":0,"note_id":3}]'

project before "$M" "$N"
render before

# ---- ADDRESSED. Each note must sound ITS slice's frequency, not the whole break and not a
# neighbour. Slice 1 = frames 12000..24000 = the 330 Hz segment, and so on.
domHz() {
  python3 - "$1" "$2" "$3" <<'PYD'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
best, bestF = 0.0, 0.0
for f in (220.0, 330.0, 495.0, 740.0):
    k = 2.0 * math.cos(2.0 * math.pi * f / sr)
    s1 = s2 = 0.0
    for i in range(a, b):
        wn = 0.5 - 0.5 * math.cos(2.0 * math.pi * (i - a) / max(1, b - a - 1))
        s0 = s[i * ch] * wn + k * s1 - s2
        s2, s1 = s1, s0
    m = math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2))
    if m > best:
        best, bestF = m, f
print(int(bestF))
PYD
}
H1="$(domHz "$TMP/before.wav" 0.52 0.68)"
H2="$(domHz "$TMP/before.wav" 1.02 1.18)"
H3="$(domHz "$TMP/before.wav" 1.52 1.68)"
echo "  slices sound: ${H1} Hz, ${H2} Hz, ${H3} Hz"
[ "$H1" = "330" ] || fail "slice 1 (frames 12000..24000) should be the 330 Hz segment, got $H1"
[ "$H2" = "495" ] || fail "slice 2 should be the 495 Hz segment, got $H2"
[ "$H3" = "740" ] || fail "slice 3 should be the 740 Hz segment, got $H3"
echo "  addressed: each note plays ITS slice, identified by frequency"

# ---- STABLE. Insert a marker INSIDE slice 1 (upstream of 2 and 3). Slices 2 and 3 must be
# BIT-IDENTICAL — not similar, identical. An index-addressed design would shift them by one and
# still sound plausible, which is exactly why the comparison is exact.
M2='[{"id":1,"frame":12000},{"id":4,"frame":18000},{"id":2,"frame":24000},{"id":3,"frame":36000}]'
project after "$M2" "$N"
render after

python3 - "$TMP/before.wav" "$TMP/after.wav" <<'PYC'
import sys, wave, struct
def frames(p):
    w = wave.open(p, 'rb')
    ch, n = w.getnchannels(), w.getnframes()
    d = w.readframes(n); w.close()
    return d, ch, n
a, ach, an = frames(sys.argv[1])
b, bch, bn = frames(sys.argv[2])
rate = 44100
def span(t0, t1):
    return slice(int(t0 * rate) * ach * 2, int(t1 * rate) * ach * 2)
# Notes 2 and 3 are at 1.0 s and 1.5 s; compare generously around them.
for label, t0, t1 in (("slice 2", 1.00, 1.45), ("slice 3", 1.50, 1.95)):
    sa, sb = a[span(t0, t1)], b[span(t0, t1)]
    if sa != sb:
        diff = sum(1 for x, y in zip(sa, sb) if x != y)
        print("%s DIFFERS in %d of %d bytes after an upstream re-cut" % (label, diff, len(sa)))
        raise SystemExit(1)
    if not any(sa):
        print("%s is SILENT in both renders — comparing two silences proves nothing" % label)
        raise SystemExit(1)
print("  stable: slices 2 and 3 are BIT-IDENTICAL after a marker was inserted upstream")
PYC
[ $? -eq 0 ] || fail "re-cutting the chop moved audio that no row asked to move. Slices address
        by ID precisely so this cannot happen; if it did, an insert is renumbering something"

# ---- DERIVED. The new cut IS audible where it was made: slice 1 now ends at 18000, so its
# note plays a shorter segment. If the predecessor did not shorten, the extent is stored
# somewhere and has drifted from the marker list.
BEFORE_LEN="$(python3 - "$TMP/before.wav" <<'PYL'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a = int(0.5 * sr)
win = sr // 100
c = 0
i = a
while i + win < n:
    if max(abs(s[j * ch]) for j in range(i, i + win)) > 400:
        c += 1
    elif c > 0:
        break
    i += win
print(c)
PYL
)"
AFTER_LEN="$(python3 - "$TMP/after.wav" <<'PYL'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a = int(0.5 * sr)
win = sr // 100
c = 0
i = a
while i + win < n:
    if max(abs(s[j * ch]) for j in range(i, i + win)) > 400:
        c += 1
    elif c > 0:
        break
    i += win
print(c)
PYL
)"
echo "  slice 1 length: ${BEFORE_LEN} -> ${AFTER_LEN} windows after the insert"
[ "$AFTER_LEN" -lt "$BEFORE_LEN" ] || \
  fail "slice 1 did not SHORTEN when a marker was inserted inside it ($BEFORE_LEN -> $AFTER_LEN).
        Its extent is derived from marker order, so an insert must shorten it automatically — if
        it did not, the extent is stored somewhere and has already drifted from the markers"
echo "  derived: the predecessor shortened, so the new cut is audible where it was made"

# ---- GONE IS GONE. Remove slice 2; the note addressing it must be SILENT rather than falling
# back to the whole sample. A chop whose slice was deleted should not suddenly play the entire
# break — that is louder, longer and completely wrong.
M3='[{"id":1,"frame":12000},{"id":3,"frame":36000}]'
project removed "$M3" "$N"
render removed
GONE="$(python3 - "$TMP/removed.wav" <<'PYG'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(1.02 * sr), int(1.40 * sr)
print(max(abs(s[i * ch]) for i in range(a, min(b, n))))
PYG
)"
[ "$GONE" -lt 400 ] || \
  fail "a note addressing a REMOVED slice produced audio (peak $GONE). It must be silent: falling
        back to the whole sample would play the entire break where one chop was expected, which is
        louder, longer and completely wrong — and it is the shape of failure that a 'sensible
        default' introduces"
echo "  gone is gone: a note addressing a removed slice is silent, not the whole sample"

echo "sampler_slice_stability_check: PASS — re-cut while it plays, and the rows do not move"
