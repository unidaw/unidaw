#!/usr/bin/env bash
# A PATCHER COULD NOT PLAY THE BUILT-IN SAMPLER.
#
# Every one of the six places that feed the sampler was on the CLIP path — the note tee inside
# renderTrack. The patcher's generated events take a different route entirely: they are resolved
# from degrees to pitches and written straight into the host ring as MIDI. So a Euclidean or
# RandomDegree node produced notes that a hosted PLUGIN would play and an in-engine instrument on
# the same track never heard. Rendered, the track was silent, with the sampler reporting a
# perfectly healthy render of one decoded slot.
#
# That also blocked docs/SAMPLER_DESIGN.md's own top follow-up: a SliceSelect node choosing which
# slice a step plays is worth nothing if patcher notes cannot reach the sampler at all.
#
# THE GRAPH HERE IS euclidean -> random_degree -> event_out, AND THE MIDDLE NODE IS REQUIRED.
# Euclidean emits GATES — a rhythm with no pitch — and the note-resolution path skips gates by
# design. The first version of this fixture went straight from euclidean to event_out, produced
# 40 gate events, resolved none of them, and rendered silence. That silence looked exactly like
# the bug being tested for, which is the trap: a fixture that cannot produce the input makes the
# check agree with you for the wrong reason.
#
# TWO PROPERTIES:
#   SOUNDS   a patcher-generated note plays the sampler on the same track
#   TIMED    it lands on the patcher's rhythm rather than all at the block boundary — the tee
#            carries the exact frame offset, which is the thing MidiEvent.sampleOffset throws
#            away for hosted plugins (docs/SAMPLER_DESIGN.md §3.5)
#
# Rendered OFFLINE. No audio device needed.
#   tools/patcher_plays_sampler_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# Short and percussive, so each generated note is a separate burst the timing test can find.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
n = sr // 8
w.writeframes(b''.join(
    struct.pack('<h', int(15000 * math.sin(2 * math.pi * 330.0 * i / sr) * (1.0 - i / n)))
    for i in range(n)))
w.close()
PY

python3 - "$TMP/pat.uniproj.json" "$TMP" "$Q" <<'PY'
import json, sys, os
out, dirname, Q = sys.argv[1], sys.argv[2], int(sys.argv[3])
DIRECT = 4294967294
def r(k="none"):
    return {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        # A ZONE across the keyboard, because the pitch a patcher resolves is not known in
        # advance — the keymap has to answer for whatever degree comes out.
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 1000, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
sampler = {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2, "stem_count": 0,
           "voice_cap": 16, "default_view": 0,
           "sources": [{"local_id": 1, "path": os.path.join(dirname, "s.wav"),
                        "content_key": 0}],
           "slice_sets": [],
           "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                         "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
           "slots": [slot]}
# euclidean -> random_degree -> event_out. The middle node is not decoration: euclidean emits
# GATES and the note path skips them, so without it this graph resolves nothing and renders
# silence that looks exactly like the defect.
patcher = {"device_id": 1, "kind": "patcher_instrument", "capability_mask": 5,
           "patcher_node_id": 1, "host_slot_index": DIRECT, "bypass": False,
           "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
           "patcher": {"nodes": [
               {"id": 0, "type": "euclidean",
                "euclidean": {"steps": 16, "hits": 8, "offset": 0,
                              "duration_ticks": Q // 4, "degree": 1, "octave_offset": 0,
                              "velocity": 110, "base_octave": 4}},
               {"id": 2, "type": "random_degree",
                "random_degree": {"degree": 8, "velocity": 110, "duration_ticks": Q // 4}},
               {"id": 1, "type": "event_out"}],
             "edges": [
               {"src_node_id": 0, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0,
                "kind": "event"},
               {"src_node_id": 2, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0,
                "kind": "event"}]}}
dev = {"device_id": 2, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [patcher, dev], "mod_links": [],
      # NO PLACEMENTS AT ALL. Every note in this render came from the patcher, so nothing here
      # can be explained by a clip.
      "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "pat"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           # A harmony is REQUIRED: the resolution path turns a degree into a pitch through the
           # scale in force, and drops the event when there is none.
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/patsam_$$" \
    ./daw_engine --project pat --render pat --run-seconds 5 --block-size 256 \
    >"$TMP/eng.log" 2>&1 ) || fail "the render exited non-zero — see $TMP/eng.log"
[ -s "$TMP/pat.wav" ] || fail "the render wrote no output"

grep -qE '"event":"project.patcher_(loaded|assembled)"' "$TMP/eng.log" || \
  fail "the patcher graph never loaded, so this check is measuring a project without one"

read -r PEAK BURSTS <<EOF
$(python3 - "$TMP/pat.wav" <<'PYA'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
peak = max((abs(v) for v in s), default=0)
# Count ONSETS: a rise from near-silence to loud. The generated notes are short percussive
# bursts, so the count is the rhythm — one number that says both "it sounded" and "more than
# once", which a single peak cannot.
bursts, armed = 0, True
gate_hi, gate_lo = peak * 0.35, peak * 0.05
for i in range(0, n, 32):
    a = abs(s[i * ch])
    if armed and a > gate_hi:
        bursts += 1
        armed = False
    elif not armed and a < gate_lo:
        armed = True
print(peak, bursts)
PYA
)
EOF
echo "  patcher -> sampler: peak $PEAK, $BURSTS separate onsets"

# ---- SOUNDS.
[ "${PEAK:-0}" -gt 2000 ] || \
  fail "a patcher-generated note did not reach the sampler: peak $PEAK. The note tee lived only
        on the clip path, so patcher events resolved to MIDI for a hosted plugin and the
        in-engine instrument on the same track heard nothing"

# ---- TIMED. Eight hits over sixteen steps, repeating: several onsets, not one long note and not
# everything piled on one boundary.
[ "${BURSTS:-0}" -ge 4 ] || \
  fail "only $BURSTS onset(s) in the take. The patcher's rhythm is 8 hits in 16 steps, so a
        handful of separate bursts is what reaching the sampler sounds like — one burst means
        the notes arrived but not as a rhythm"

echo "patcher_plays_sampler_check: PASS — a patcher generator drives the built-in sampler, in time"
