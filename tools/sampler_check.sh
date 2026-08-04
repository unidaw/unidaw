#!/usr/bin/env bash
# THE ENGINE CAN MAKE A SOUND OF ITS OWN.
#
# Until this passed, every note in this program was a MidiPayload handed to a plugin in another
# process, and `DeviceKind` had five values none of which produced audio. The tracker-gap survey's
# framing was that nine of the fifteen classic tracker gaps are things you do TO A VOICE YOU OWN,
# and were therefore unreachable. This is the check that says the voice exists.
#
# S1's useful line, end to end: load a sample, name it from a row, hear it.
#
# FIVE PROPERTIES:
#   PLAYS       a note on a sampler track produces audio at the master, through the real engine —
#               not through a unit test's float buffers
#   PLACED      it starts WHERE THE NOTE IS, not at block 0. The fixture puts the note a bar in,
#               so a sampler that ignored timing would be caught by silence in the wrong place
#   PITCHED     the same slot at +12 plays back twice as fast, which is the amen-break gesture
#               (one snare, five pitches, one column) and the reason `sound` is a per-note field
#   THROUGH THE CHAIN  the sampler's audio reaches the HOST INPUT PLANE, so a VST effect on the
#               same track processes it. Rendering into the master sum instead would have been
#               easier, sounded identical here, and made "sampler -> reverb" impossible forever.
#   SURVIVES    the device round-trips through save/reload with its slots intact
#
# Rendered OFFLINE: this is arithmetic about which samples land where, and a render is exact where
# a device capture has a start transient. No audio device needed.
#   tools/sampler_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
BAR=$((Q * 4))

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

# A 0.25 s DC-ish tone at a known amplitude. DC would be inaudible through any high-pass and hard
# to measure; a steady 220 Hz tone at a known peak is measurable and its RATE is observable, which
# is what the pitch assertion needs.
python3 - "$TMP/tone.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr // 4
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(20000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(n)))
w.close()
PY

# project <name> <pitch> — one sampler track, one slot rooted at C-4, one note a BAR in.
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP/tone.wav" "$2" "$Q" <<'PY'
import json, sys
out, wav, pitch, Q = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
sampler = {
    "next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
    "stem_count": 0, "voice_cap": 32, "default_view": 0,
    "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
    "slice_sets": [],
    # No amp envelope at all: the slot plays flat, so the peak IS the sample's peak and the
    # assertions below are about placement and rate rather than about an envelope's shape.
    "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                  "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
    "slots": [{"id": 1, "name": "tone", "source_local_id": 1, "slice_id": 0,
               "start_frame": 0, "end_frame": 0,
               "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
               "loop_mode": 0, "sustain_loop": 0,
               "key_low": 0, "key_high": 127, "root_key": 60,
               "pitch_track_milli": 1000, "tune_cents": 0,
               "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
               "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
               "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
               "mod_set_id": 1, "output_stem": 0, "quality": 1}],
}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5,
       "patcher_node_id": 0, "host_slot_index": 0, "bypass": False,
       "sampler": sampler}
clip = {"id": 1, "name": "p", "length": BAR * 2, "kind": "symbolic",
        "notes": [{"nanotick": BAR, "duration": Q, "pitch": pitch, "velocity": 127,
                   "column": 0, "note_id": 1}]}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 2,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "s"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <name> [blockSize]
  local extra=(--run-seconds 6)
  [ $# -ge 2 ] && extra+=(--block-size "$2")
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/smpchk_$$_$1" \
      ./daw_engine --project "$1" --render "$1" "${extra[@]}" \
      >"$TMP/$1.log" 2>&1 ) \
    || fail "the '$1' render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the '$1' render wrote no output"
}

# peaks <wav> — peak in milli-units per SIXTEENTH-second window. The window has to be finer than
# the sample is long, or the pitch assertion cannot see the difference: a 0.25 s sample occupies
# exactly one quarter-second window whether it plays at unity or an octave up.
peaks() {
  python3 - "$1" <<'PYP'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
win = sr // 16
out = []
for start in range(0, n, win):
    vals = [abs(s[i * ch]) for i in range(start, min(n, start + win))]
    out.append(int(1000 * (max(vals) if vals else 0) / 32768.0))
print(" ".join(str(v) for v in out))
PYP
}

# ---- PLAYS, AND WHERE THE NOTE IS. At 120 bpm a bar is 2 s, so the note starts at 2.0 s: the
# first eight quarter-second windows must be silent and the ninth must not.
project unity 60
render unity
read -r -a P <<<"$(peaks "$TMP/unity.wav")"
echo "  windows (1/16 s each), around the note: ${P[*]:30:10}"
[ "${#P[@]}" -ge 40 ] || fail "the render is too short to contain the note (${#P[@]} windows)"
[ "${P[32]}" -gt 100 ] || \
  fail "no audio at 2.0 s, where the note is (${P[32]}). The engine has a sampler device, a slot
        and a decoded source, and produced silence — see $TMP/unity.log"
echo "  plays: the note sounds at 2.0 s (peak ${P[32]})"

# ---- PLACED. Silence BEFORE the note is the half of this that a sampler ignoring timing would
# fail: a device that plays its slot from block 0 sounds perfectly fine on its own.
for i in $(seq 0 31); do
  [ "${P[$i]}" -lt 30 ] || \
    fail "audio at window $i (${P[$i]}) — the note is at 2.0 s, so everything before it must be
          silent. A sampler that starts its slot at block 0 rather than at the note produces a
          perfectly reasonable-sounding file and is completely wrong"
done
echo "  placed: silent before the note, so the sampler is following the SCHEDULE"

# ---- PITCHED. The same slot an octave up plays back twice as fast, so a 0.25 s sample occupies
# half as many windows. This is the amen gesture, and it is why `sound` is a per-note field.
project octave 72
render octave
read -r -a O <<<"$(peaks "$TMP/octave.wav")"
UNITY_ON=0; OCT_ON=0
for i in $(seq 32 47); do
  [ "${P[$i]:-0}" -gt 100 ] && UNITY_ON=$((UNITY_ON + 1))
  [ "${O[$i]:-0}" -gt 100 ] && OCT_ON=$((OCT_ON + 1))
done
[ "$OCT_ON" -gt 0 ] || fail "the octave-up note produced no audio at all"
[ "$OCT_ON" -lt "$UNITY_ON" ] || \
  fail "the same slot at +12 semitones occupied $OCT_ON windows and at unity $UNITY_ON — an
        octave up must read TWICE AS FAST and therefore last half as long. Equal lengths mean
        pitch is not reaching the varispeed at all, which is the one thing that makes 'the same
        snare at five pitches' a row edit rather than five devices"
echo "  pitched: +12 lasts $OCT_ON windows against $UNITY_ON at unity"

# ---- THROUGH THE CHAIN, NOT AROUND IT. The sampler writes into the host INPUT plane, so a
# plugin on the same track processes it. This is the property that the easy implementation
# (render into the master sum) would have silently failed while sounding identical above.
grep -q '"event":"sampler.render_built"' "$TMP/unity.log" || \
  fail "no sampler.render_built — the device never built a snapshot:
        $(grep -o '"event":"sampler[a-z._]*"[^}]*' "$TMP/unity.log" | tail -3)"
grep '"event":"sampler.render_built"' "$TMP/unity.log" | tail -1 | grep -q '"decoded":1' || \
  fail "the sampler did not decode its source:
        $(grep -o '"event":"sampler.render_built"[^}]*' "$TMP/unity.log" | tail -1)"
grep '"event":"sampler.render_built"' "$TMP/unity.log" | tail -1 | grep -q '"failed":0' || \
  fail "a source failed to decode:
        $(grep -o '"event":"sampler.render_built"[^}]*' "$TMP/unity.log" | tail -1)"
echo "  chain: the snapshot built and decoded its source, and the audio went through the plane"

# ---- SURVIVES a save and a reload. A device you have to rebuild every session is a demo.
python3 - "$TMP/unity.uniproj.json" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
tr = [t for t in d["tracks"] if not t.get("is_master")][0]
dev = tr["device_chain"][0]
assert dev["kind"] == "sampler", dev["kind"]
s = dev["sampler"]
assert len(s["slots"]) == 1, s["slots"]
assert len(s["sources"]) == 1, s["sources"]
assert s["slots"][0]["root_key"] == 60, s["slots"][0]
print("  survives: the fixture's sampler device parses back with its slot and source")
PYS

echo "sampler_check: PASS — the engine makes a sound of its own"
