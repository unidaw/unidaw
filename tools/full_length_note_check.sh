#!/usr/bin/env bash
# A NOTE THAT FILLS ITS CLIP STILL SOUNDS.
#
# A gated note whose duration exactly equalled its clip's length rendered SILENT. wrapTick maps
# loopEnd to loopStart — right for a POSITION, wrong for an END — so a note with
# onTick == loopStart and noteEndTick == loopEnd had its note-off wrapped to loopStart, the same
# tick as its own note-on, and the voice was cut the instant it started.
#
# "A pad note filling the bar" is an ordinary thing to write. Every structural fact about it was
# correct: the note is in the clip, it emits, the slot resolves, the sampler reports a healthy
# render. Only the audio was missing — the same shape as the rest of this suite's findings.
#
# IT ONLY BIT A GATED SLOT, which is why it survived: a one-shot ignores note-off entirely, and
# every sampler fixture in this repo until now used gate 0.
#
# A SECOND DEFECT WAS FOUND IN THE SAME BLOCK and is covered by the same fixture at other note
# lengths: the sampler's note-off tee read eventSample — the note-ON's time — instead of
# offSample, so any note whose on and off fell in ONE block released at the instant it started.
# The MIDI entry three lines above already used offSample; the two disagreed, and only the
# in-engine tee was wrong, so the same note through a hosted plugin sounded correct.
#
# THREE LENGTHS, because the interesting cases are at the ends:
#   FULL     duration == clip length: the wrap case
#   SHORT    duration well under one block: the on/off-in-one-block case
#   HALF     an ordinary note, as the control that says the fixture can make sound at all
#
#   tools/full_length_note_check.sh
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

# A LONG STEADY TONE, so a note that sustains has something to sustain WITH and the measurement
# is about the note's length rather than the sample running out.
python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr * 3
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(13000 * math.sin(2 * math.pi * 400.0 * i / sr))) for i in range(n)))
w.close()
PY

# project <name> <durationTicks>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" <<'PY'
import json, sys
out, Q, dur = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        # GATE 1 IS THE WHOLE POINT. A one-shot ignores note-off, so it cannot show either of the
        # defects here — and every sampler fixture in this repo until now used gate 0, which is
        # exactly why both survived.
        "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 8, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
notes = [{"nanotick": 0, "duration": dur, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": 1}]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "f"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/fulln_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --sample-rate 44100 --run-seconds 3 --block-size 1024 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

peak() {  # peak <name> <fromSec> <toSec>
  python3 - "$TMP/$1.wav" "$2" "$3" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(sr * float(sys.argv[2])), int(sr * float(sys.argv[3]))
seg = [abs(s[i * ch]) for i in range(max(a, 0), min(b, n))]
print(max(seg) if seg else 0)
PY
}

# ---- HALF. An ordinary note, and the control: if this is silent the fixture is broken and
# nothing below means anything.
project half $((Q * 2))
render half half
HALF="$(peak half 0.1 0.9)"
echo "  half-bar note:  peak $HALF"
[ "${HALF:-0}" -gt 2000 ] || \
  fail "an ordinary half-bar gated note is silent (peak ${HALF:-0}). The fixture cannot make
        sound at all, so the two cases below would prove nothing"

# ---- FULL. duration == clip length. This is the one that rendered silence.
project full $((Q * 4))
render full full
FULL="$(peak full 0.1 0.9)"
echo "  full-bar note:  peak $FULL"
[ "${FULL:-0}" -gt 2000 ] || \
  fail "a gated note whose duration EQUALS its clip length is silent (peak ${FULL:-0}) where a
        half-length one gives $HALF. wrapTick maps loopEnd to loopStart, so the note-off lands on
        the same tick as the note-on and cuts the voice as it starts"

# ---- SHORT. Well under one 1024-frame block, so the note-on and note-off fall in ONE block.
# At 120bpm a 1024-frame block is about 11145 nanoticks; Q/128 is 7500.
project short $((Q / 128))
render short short
SHORT="$(peak short 0.0 0.2)"
echo "  sub-block note: peak $SHORT"
[ "${SHORT:-0}" -gt 2000 ] || \
  fail "a note shorter than one block is silent (peak ${SHORT:-0}). Its note-on and note-off fall
        in the same block, and the sampler's note-off tee read the note-ON's sample time — so the
        voice was released at the instant it started"

echo "full_length_note_check: PASS — a note that fills its clip sounds, and so does one shorter"
echo "                        than a block"
