#!/usr/bin/env bash
# ONE SNARE, FIVE PITCHES, ONE COLUMN.
#
# This is the gesture the whole sampler design turns on, and the reason `sound` is a per-NOTE
# field rather than a device setting (docs/SAMPLER_DESIGN.md R2). Without it, playing one sample
# at five pitches means five devices — which is what it costs in Live, and why re-chopping a break
# there means re-writing the part.
#
# THE RESOLUTION RULE, and there is exactly one:
#
#   sound != 0   play THAT slot, and `pitch` means varispeed relative to its rootKey
#   sound == 0   find the slot through the keymap, and `pitch` means exactly the same thing
#
# Pitch has one meaning either way. That is what makes the two modes one mechanism rather than
# two, and it is what this check measures — not "did a sound come out" but "did the RIGHT slot
# sound, at the RIGHT rate, from a key that maps to something else".
#
# FIVE PROPERTIES:
#   ADDRESSES   `sound: N` plays slot N from a key the keymap would resolve elsewhere
#   OVERRIDES   the SAME key plays a DIFFERENT slot depending only on the note's `sound`
#   PITCHES     with `sound` set, pitch still varispeeds — five pitches, five rates, one slot
#   SEEKS       `sound_offset` starts playback part-way in, as a fraction of the slot's extent
#   PERSISTS    both survive a save and a reload, and are absent from the file when unset
#
# Rendered OFFLINE. No audio device needed.
#   tools/sampler_sound_address_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
# KEEP THE EVIDENCE WHEN IT FAILS. This used to be `trap 'rm -rf "$TMP"' EXIT`, while the failure
# messages above tell you to read a log inside $TMP — so the one run whose log you need is the one
# run that deletes it. That is not hypothetical: audio_loop failed once under a full-suite run,
# passed 9 times in isolation, and the reason is gone. Same convention as elektron_ops_check.
KEEPDIR="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}"
keep_evidence() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    local dest="$KEEPDIR/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  rm -rf "$TMP"
  exit $rc
}
trap keep_evidence EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# TWO slots with clearly different tones, and the KEYMAP DELIBERATELY POINTS THE WRONG WAY: key 60
# maps to slot 1 (220 Hz) and key 62 to slot 2 (660 Hz). Every assertion below plays a key whose
# keymap answer is NOT the slot being addressed, so "the sound address was ignored" is visible as
# the wrong frequency rather than as nothing at all.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
for name, hz, n in (("a.wav", 220.0, sr // 2), ("b.wav", 660.0, sr // 2)):
    w = wave.open(os.path.join(sys.argv[1], name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(struct.pack('<h', int(19000 * math.sin(2 * math.pi * hz * i / sr)))
                           for i in range(n)))
    w.close()
PY

# project <name> <notes-json>
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP" "$2" "$Q" <<'PY'
import json, sys, os
out, dirname, notes, Q = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def slot(i, src, root):
    return {"id": i, "name": "s%d" % i, "source_local_id": src, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            # FIXED PITCH ZONES, one key each: key 60 -> slot 1, key 62 -> slot 2. So a note on
            # key 62 that ASKS for slot 1 is asking for something the keymap would not give it.
            "key_low": root, "key_high": root, "root_key": root,
            "pitch_track_milli": 1000, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 1}
sampler = {
    "next_slot_id": 3, "next_source_id": 3, "next_mod_set_id": 2,
    "stem_count": 0, "voice_cap": 32, "default_view": 0,
    "sources": [{"local_id": 1, "path": os.path.join(dirname, "a.wav"), "content_key": 0},
                {"local_id": 2, "path": os.path.join(dirname, "b.wav"), "content_key": 0}],
    "slice_sets": [],
    "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                  "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
    "slots": [slot(1, 1, 60), slot(2, 2, 62)],
}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
clip = {"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "sa"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
}

render() {
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/sachk_$$_$1" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 8 --block-size 256 \
      >"$TMP/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}

# tone <wav> <start> <end> — which of the two source tones dominates, scaled by the ratio it
# would be played at. Returns "220" or "660" style base identification plus the dominant Hz.
peakHz() {
  python3 - "$1" "$2" "$3" <<'PYH'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
best, bestF = 0.0, 0.0
f = 100.0
while f < 4000.0:
    k = 2.0 * math.cos(2.0 * math.pi * f / sr)
    s1 = s2 = 0.0
    for i in range(a, b):
        wnd = 0.5 - 0.5 * math.cos(2.0 * math.pi * (i - a) / max(1, b - a - 1))
        s0 = s[i * ch] * wnd + k * s1 - s2
        s2, s1 = s1, s0
    m = math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2))
    if m > best:
        best, bestF = m, f
    f *= 1.01
print(int(round(bestF)))
PYH
}

# ---- ADDRESSES and OVERRIDES. Two notes on the SAME key (62), one plain and one asking for
# slot 1. The keymap says key 62 is slot 2 (660 Hz); the addressed one must be slot 1 (220 Hz,
# played at 62-60 = +2 semitones = 247 Hz).
project addr "[{\"nanotick\":$((Q)),\"duration\":$((Q/2)),\"pitch\":62,\"velocity\":110,\"column\":0,\"note_id\":1},{\"nanotick\":$((Q*3)),\"duration\":$((Q/2)),\"pitch\":62,\"velocity\":110,\"column\":0,\"note_id\":2,\"sound\":1}]"
render addr
PLAIN="$(peakHz "$TMP/addr.wav" 0.55 0.72)"
ADDRESSED="$(peakHz "$TMP/addr.wav" 1.55 1.72)"
echo "  key 62 plain -> ${PLAIN} Hz | key 62 with sound=1 -> ${ADDRESSED} Hz"
# Key 62 maps to slot 2 (660 Hz at root 62, so unity).
[ "$PLAIN" -gt 600 ] && [ "$PLAIN" -lt 720 ] || \
  fail "an UNADDRESSED note on key 62 should play slot 2 through the keymap (~660 Hz), got $PLAIN"
# sound=1 forces slot 1 (220 Hz at root 60), played at key 62 = +2 semitones = 220*2^(2/12) = 247.
[ "$ADDRESSED" -gt 225 ] && [ "$ADDRESSED" -lt 275 ] || \
  fail "an ADDRESSED note (sound=1) on key 62 should play slot 1 at +2 semitones (~247 Hz), got
        $ADDRESSED. If it reads ~660 the sound address was ignored and the keymap answered; if it
        reads ~220 the address worked but PITCH stopped meaning varispeed, which would make the
        two addressing modes two different mechanisms"
echo "  addresses: the same key plays a different slot, and pitch still varispeeds"

# ---- PITCHES. ONE SLOT, FIVE PITCHES, ONE COLUMN — the amen gesture, and the reason this field
# exists at all. All five ask for slot 1 from keys the keymap maps elsewhere or not at all.
NOTES='['
IDX=0
for K in 60 63 65 67 72; do
  T=$(( (IDX + 1) * Q ))
  NOTES="$NOTES{\"nanotick\":$T,\"duration\":$((Q/2)),\"pitch\":$K,\"velocity\":110,\"column\":0,\"note_id\":$((IDX+1)),\"sound\":1},"
  IDX=$((IDX + 1))
done
NOTES="${NOTES%,}]"
project five "$NOTES"
render five
PREV=0
IDX=0
for K in 60 63 65 67 72; do
  START=$(python3 -c "print(($IDX+1)*0.5+0.05)")
  END=$(python3 -c "print(($IDX+1)*0.5+0.22)")
  HZ="$(peakHz "$TMP/five.wav" "$START" "$END")"
  WANT=$(python3 -c "print(int(round(220.0 * 2**(($K-60)/12.0))))")
  LO=$(python3 -c "print(int($WANT*0.90))")
  HI=$(python3 -c "print(int($WANT*1.10))")
  [ "$HZ" -gt "$LO" ] && [ "$HZ" -lt "$HI" ] || \
    fail "key $K addressing slot 1 should sound near ${WANT} Hz, got ${HZ}. One slot at five
          pitches down one column is the gesture this whole field exists for"
  [ "$HZ" -gt "$PREV" ] || fail "the five pitches are not ascending ($PREV then $HZ)"
  PREV="$HZ"
  IDX=$((IDX + 1))
done
echo "  pitches: one slot at five ascending pitches, all from one column"

# ---- SEEKS. `sound_offset` starts part-way into the slot. Measured against a slot whose two
# halves differ: the fixture's tone is uniform, so instead compare TOTAL DURATION — starting half
# way in leaves half as much sample to play.
project seek "[{\"nanotick\":$((Q)),\"duration\":$((Q/2)),\"pitch\":60,\"velocity\":110,\"column\":0,\"note_id\":1,\"sound\":1},{\"nanotick\":$((Q*3)),\"duration\":$((Q/2)),\"pitch\":60,\"velocity\":110,\"column\":0,\"note_id\":2,\"sound\":1,\"sound_offset\":32768}]"
render seek
dur() {  # dur <wav> <startSec> — how long audio continues past `start`, in 10 ms windows
  python3 - "$1" "$2" <<'PYD'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a = int(float(sys.argv[2]) * sr)
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
PYD
}
FULL="$(dur "$TMP/seek.wav" 0.50)"
HALF="$(dur "$TMP/seek.wav" 1.50)"
echo "  from the start: ${FULL} windows | from half way: ${HALF} windows"
[ "$FULL" -gt 30 ] || fail "the unseeked note is only $FULL windows long — the fixture is 0.5 s"
[ "$HALF" -lt "$((FULL * 3 / 4))" ] || \
  fail "a note with sound_offset=32768 (half way) lasted $HALF windows against $FULL — starting
        half way into the slot must leave half as much sample to play. Equal lengths mean the
        offset never reached the voice"
echo "  seeks: a half-way offset leaves half the sample"

# ---- PERSISTS, and is ABSENT when unset. A `"sound": 0` on every note would make every project
# noisier to diff for a field that is almost always the default.
python3 - "$TMP/addr.uniproj.json" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
notes = d["clips"][0]["notes"]
assert "sound" not in notes[0], "an UNSET sound was written to the file: %r" % notes[0]
assert notes[1].get("sound") == 1, "a set sound did not survive: %r" % notes[1]
print("  persists: written when set, absent when not")
PYS

echo "sampler_sound_address_check: PASS — one snare, five pitches, one column"
