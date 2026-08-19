#!/usr/bin/env bash
# A SLOT'S STEM BECOMES A REAL TRACK.
#
# This is ruling R1 in docs/SAMPLER_DESIGN.md, and the reason per-slot device chains are refused
# permanently: "this kick needs its own compressor" is not `slot.chain.push(compressor)`, it is
# `--stem 1` and the kick becomes a CHILD TRACK — with its own automation, its own devices, its
# own place in the mixer, PDC already correct and sidechain already wired. Live's pad chain has
# none of that; it is a second-class container that looks like a track and is not.
#
# S6 IN THE DESIGN WAS WRONG ABOUT HOW, and this check exists because of what it took to fix.
# It said "outputStem onto the existing aux output plane. No bump; the plane ships." The plane
# ships FOR HOSTED PLUGINS: a child reads a bus slice of the parent's aux OUTPUT plane, which the
# parent's HOST writes, and reconcileChildTracks derives children by asking that host what buses
# it has. An in-engine instrument has no host to ask and no way to write that plane.
#
# So the input plane gained an aux INPUT region (kControlVersion 14): the sampler writes stems
# there, the host copies aux-in to aux-out before its plugins run, and the child machinery reads
# the output plane exactly as it always did. The sampler's stems travel the same route as a
# plugin's rather than needing a private path around it.
#
# WHY THE PARENT IS MUTED HALFWAY THROUGH. A stem that works and a stem that was never routed
# sound IDENTICAL at the master: a child track sits at unity gain, so slot 1 arriving via the
# child sums to exactly what slot 1 arriving on the main output would have. The first version of
# this check compared those two renders, got byte-identical energies, and called it a PASS — it
# would have passed just as happily with the entire stem path deleted.
#
# Muting the PARENT is what separates them. A child carries its own mute/gain and reads the
# parent's aux plane, which is written before the parent's mixer (daw_engine_main.cpp:15780), so:
#   parent muted + stem  -> slot 1 SURVIVES (it left on the stem)
#   parent muted, no stem -> slot 1 is GONE  (it was on the main output all along)
# The second render is the negative control, and it is in the check rather than beside it.
#
# FOUR PROPERTIES:
#   SPLITS    a stemmed slot survives its parent's mute — it genuinely leaves on the stem
#   CONTROL   the same slot WITHOUT a stem does not survive, so SPLITS can only pass via the stem
#   NOT BOTH  it is not ALSO in the main mix — sending it to both doubles it the moment the child
#             is unmuted, which you only ever hear as "the kick is loud"
#   MAIN OK   a slot WITHOUT a stem still goes to the main output, unaffected
#
# Rendered OFFLINE. No audio device needed.
#   tools/sampler_stem_check.sh
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

# Two clearly different tones, so "which slot am I hearing" is answerable from the audio.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
for name, hz in (("a.wav", 300.0), ("b.wav", 900.0)):
    w = wave.open(os.path.join(sys.argv[1], name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(struct.pack('<h', int(19000 * math.sin(2 * math.pi * hz * i / sr)))
                           for i in range(sr // 2)))
    w.close()
PY

# project <name> <stemForSlot1> <muteParent>
# Slot 1 plays a.wav (300 Hz) at 0.5s, slot 2 plays b.wav (900 Hz) at 1.5s. Separated in TIME as
# well as pitch, so each is measurable without the other.
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP" "$2" "$3" "$Q" <<'PY'
import json, sys, os
out, dirname, stem, mute, Q = (sys.argv[1], sys.argv[2], int(sys.argv[3]),
                               int(sys.argv[4]), int(sys.argv[5]))
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def slot(i, src, key, st):
    return {"id": i, "name": "s%d" % i, "source_local_id": src, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": key, "key_high": key, "root_key": key,
            "pitch_track_milli": 0, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": st, "quality": 1}
sampler = {
    "next_slot_id": 3, "next_source_id": 3, "next_mod_set_id": 2,
    # stemCount > 0 is what makes reconcileChildTracks synthesise buses for an in-engine
    # instrument, which is the whole mechanism this check exercises.
    "stem_count": 1 if stem else 0,
    "voice_cap": 32, "default_view": 0,
    "sources": [{"local_id": 1, "path": os.path.join(dirname, "a.wav"), "content_key": 0},
                {"local_id": 2, "path": os.path.join(dirname, "b.wav"), "content_key": 0}],
    "slice_sets": [],
    "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                  "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
    "slots": [slot(1, 1, 60, stem), slot(2, 2, 62, 0)],
}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
notes = [{"nanotick": Q, "duration": Q // 2, "pitch": 60, "velocity": 120,
          "column": 0, "note_id": 1},
         {"nanotick": Q * 3, "duration": Q // 2, "pitch": 62, "velocity": 120,
          "column": 0, "note_id": 2}]
clip = {"id": 1, "name": "p", "length": BAR * 4, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": bool(mute), "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "st"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
}

render() {
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/stemchk_$$_$1" \
      ./daw_engine --project "$1" --render "$1" --sample-rate 44100 --run-seconds 8 --block-size 256 \
      >"$TMP/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}

energyAt() {  # energyAt <wav> <startSec> <endSec> <hz>
  python3 - "$1" "$2" "$3" "$4" <<'PYE'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(float(sys.argv[2]) * sr), min(n, int(float(sys.argv[3]) * sr))
hz = float(sys.argv[4])
k = 2.0 * math.cos(2.0 * math.pi * hz / sr)
s1 = s2 = 0.0
for i in range(a, b):
    wn = 0.5 - 0.5 * math.cos(2.0 * math.pi * (i - a) / max(1, b - a - 1))
    s0 = s[i * ch] * wn + k * s1 - s2
    s2, s1 = s1, s0
print(int(math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2))))
PYE
}

ratioOk() {  # ratioOk <num> <den> <lo> <hi>
  python3 -c "
r = $1 / max(1, $2)
raise SystemExit(0 if $3 <= r <= $4 else 1)"
}

# The child must actually be derived. Without it there is no stem track at all, and every audio
# assertion below would be measuring the wrong thing.
childCount() { grep -c '"event":"multiout.child_created"' "$TMP/$1.log" 2>/dev/null || true; }

# ---- BASELINE: no stems, parent audible. Both slots go to the main output.
project plain 0 0
render plain
A0="$(energyAt "$TMP/plain.wav" 0.52 0.70 300)"
B0="$(energyAt "$TMP/plain.wav" 1.52 1.70 900)"
echo "  plain:        slot1 (300 Hz) = $A0, slot2 (900 Hz) = $B0"
[ "$A0" -gt 10000 ] || fail "slot 1 is not audible even without a stem ($A0) — the fixture is wrong"
[ "$B0" -gt 10000 ] || fail "slot 2 is not audible ($B0) — the fixture is wrong"
[ "$(childCount plain)" = "0" ] || fail "a child track was derived for a sampler with NO stems.
        stemCount 0 must synthesise no buses at all"

# ---- NOT BOTH + MAIN OK. Slot 1 now goes to stem 1, so it becomes a CHILD track. Its audio must
# still reach the master (the child is unmuted by default) but it must arrive ONCE.
project stem 1 0
render stem
A1="$(energyAt "$TMP/stem.wav" 0.52 0.70 300)"
B1="$(energyAt "$TMP/stem.wav" 1.52 1.70 900)"
echo "  stem:         slot1 (300 Hz) = $A1, slot2 (900 Hz) = $B1"
[ "$(childCount stem)" = "1" ] || fail "stemCount 1 derived $(childCount stem) child tracks, expected 1.
        A stem with no child track is audio written to a plane nothing reads"
[ "$A1" -gt 5000 ] || \
  fail "a slot routed to a STEM went silent ($A0 -> $A1). Its audio must reach the master through
        its child track — a stem that disappears is worse than no stems, because the sound is
        gone and nothing says why"
RATIO="$(python3 -c "print(round($A1 / max(1, $A0), 2))")"
ratioOk "$A1" "$A0" 0.5 1.6 || \
  fail "a stemmed slot arrives at ${RATIO}x its un-stemmed level. Near 2x means it is going to
        BOTH the main output and the stem — which you only ever hear as 'the kick is loud', and
        only once the child track is unmuted. Near 0 means the stem goes nowhere"
ratioOk "$B1" "$B0" 0.85 1.15 || \
  fail "the UNSTEMMED slot changed level ($B0 -> $B1) when a DIFFERENT slot was routed to a
        stem. Routing one slot must not touch another"
echo "  arrives once (${RATIO}x), and the unstemmed slot is untouched"

# ---- SPLITS. Mute the PARENT. The stemmed slot left on the stem, so the child still carries it;
# the unstemmed slot was on the parent's main output and dies with it.
project stemmuted 1 1
render stemmuted
A2="$(energyAt "$TMP/stemmuted.wav" 0.52 0.70 300)"
B2="$(energyAt "$TMP/stemmuted.wav" 1.52 1.70 900)"
echo "  stem, muted:  slot1 (300 Hz) = $A2, slot2 (900 Hz) = $B2"
ratioOk "$A2" "$A0" 0.5 1.6 || \
  fail "muting the parent silenced the STEMMED slot too ($A0 -> $A2). A child track has its own
        mute and reads the parent's aux plane, which is written BEFORE the parent's mixer — so
        the stem must survive. If it does not, the slot never left on the stem at all"
ratioOk "$B2" "$B0" 0.0 0.05 || \
  fail "the parent is muted but its main output is still audible ($B0 -> $B2)"

# ---- CONTROL. The same render with NO stem. Now the parent's mute takes slot 1 with it, which is
# what makes the assertion above mean something: it can only pass because of the stem.
project plainmuted 0 1
render plainmuted
A3="$(energyAt "$TMP/plainmuted.wav" 0.52 0.70 300)"
echo "  plain, muted: slot1 (300 Hz) = $A3   <- the control"
ratioOk "$A3" "$A0" 0.0 0.05 || \
  fail "with NO stem, muting the parent left slot 1 audible ($A0 -> $A3). Then the SPLITS
        assertion above proves nothing — slot 1 survives a mute whether or not it is stemmed,
        so this check cannot see the stem path and would pass with it deleted"

echo "sampler_stem_check: PASS — a stemmed slot leaves on its own track, once, and survives the"
echo "                    parent's mute; the same slot without a stem does not"
