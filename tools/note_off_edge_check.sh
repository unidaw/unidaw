#!/usr/bin/env bash
# A NOTE-OFF ON A BLOCK-BOUNDARY TICK MUST STOP THE NOTE. If it is dropped, the note never ends.
#
# THE DEFECT THIS PINS, and why the sibling check could not. tools/block_edge_note_check.sh
# established that placeInBlock must decide block membership by FLOOR: a nanotick is far smaller
# than a sample, so a tick INSIDE this block's window can ROUND UP to exactly blockSize, and
# deciding membership on the rounded value drops it. That was fixed for the note-ON.
#
# THREE NOTE-OFF SITES KEPT THE OLD ARITHMETIC — hand-rolled, identical, and each with a different
# consequence:
#
#   engine_emit_notes.cpp   the clip note-off   the `else` that registers the note for a LATER
#                                               block sits outside the rejected branch, so
#                                               registerActiveNote never ran: PERMANENTLY STUCK
#   engine_emit_notes.cpp   the activeNotes     the note was not added to notesToRemove and its
#                           drain               end tick is now BEHIND rangeStart, so it is never
#                                               matched again until the arrangement loops: the
#                                               note sounds a whole extra pass
#   engine_resolve_events.cpp  the patcher       same as the first: a generated note never released
#
# WHY block_edge_note_check COULD NOT SEE ANY OF IT. Its slot sets "gate": 0 — a ONE-SHOT, which
# plays the sample to its end and ignores note-off by definition. The fixture that proved the
# note-ON rule is structurally incapable of testing the note-OFF rule, which is the same shape as
# every other coverage gap in this repository: the fixture decides which bugs are findable.
#
# SO THIS ONE GATES. "gate": 1 holds the sample while the note is held, so a dropped note-off is
# audible as sound that keeps going — and the sample here is deliberately LONG and FLAT, so the
# difference between "stopped when told" and "ran to the end of the sample" is the whole file
# rather than a subtlety in an envelope tail.
#
# Needs the engine built. Renders offline: no device, no wall clock, byte-deterministic.
#   tools/note_off_edge_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
SR=44100

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

for BLOCK in 512 64; do
# THE DURATION IS THE BOUNDARY, not the onset. The note starts at tick 0 — exactly on a sample —
# and its DURATION is chosen so the note-OFF lands on a tick that rounds up to blockSize. Computed
# from the settings for the same reason the sibling check computes its onset: a hard-coded number
# silently stops being a boundary the moment the tempo or the buffer changes.
EDGE="$(python3 - "$Q" "$SR" "$BLOCK" <<'PY'
import sys
Q, sr, block = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
spt = (60.0 / 120.0) * sr / Q
edges = [d for d in range(block * 200) if int(d * spt) < block <= round(d * spt)]
if not edges:
    raise SystemExit("no boundary tick exists at these settings")
print(edges[len(edges) // 2])
PY
)"
[ -n "$EDGE" ] || fail "could not compute a boundary tick for block $BLOCK"

python3 - "$TMP" "$Q" "$EDGE" <<'PY'
import json, sys, wave, struct, math, os
tmp, Q, edge = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
sr = 48000
# LONG AND FLAT ON PURPOSE. Two seconds of constant-amplitude tone: if the note-off is dropped the
# sample simply keeps playing, and "did it stop" is then a question about whole seconds of audio
# rather than about the shape of a release tail.
w = wave.open(os.path.join(tmp, "t.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(14000 * math.sin(2 * math.pi * 440.0 * i / sr)))
    for i in range(sr * 2)))
w.close()

def route(k="none", t=0):
    return {"kind": k, "track_id": t, "input_id": 0}

slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0, "start_frame": 0,
        "end_frame": 0, "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0, "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0, "vel_low": 0, "vel_high": 127,
        "layer_group": 0, "select_mode": 0,
        # GATED. This one character is the difference between a fixture that can see a dropped
        # note-off and the one next door that cannot.
        "gate": 1,
        "reverse": 0, "gain_millibels": 0,
        "pan_thousandths": 0, "voice_group": 0, "nna": 0, "polyphony": 0,
        "choke_fade_us": 3000, "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "t.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                                 "resonance_milli": 0, "next_modulator_id": 1,
                                 "modulators": []}],
                   "slots": [slot]}}
# ONE note, starting at tick 0, ending on the boundary tick.
# FOUR BARS, NOT ONE, and the render is shorter than that on purpose. With a one-bar clip the
# arrangement loops at 2 s, the note retriggers, and "the last loud sample" is the SECOND pass —
# which reads exactly like a note that never stopped. The first draft of this check measured that
# and blamed the engine; the probe that caught it was rendering with a mid-block note-off, where
# the audio still ran to the end of the file.
clip = {"id": 1, "name": "c", "length": BAR * 4, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": edge, "pitch": 60, "velocity": 110,
                   "column": 0, "note_id": 1}]}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                  "audio_out": route("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 4,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "offedge"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(os.path.join(tmp, "offedge.uniproj.json"), "w"))
PY

( cd "$BUILD" && env DAW_UI_SHM_NAME="/offedge_${BLOCK}_$$" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project offedge --render "out$BLOCK" --run-seconds 2 \
    --block-size "$BLOCK" --sample-rate "$SR" >"$TMP/eng$BLOCK.log" 2>&1 ) \
  || fail "the render exited non-zero at block $BLOCK — see $TMP/eng$BLOCK.log"
[ -s "$TMP/out$BLOCK.wav" ] || fail "the render wrote no output at block $BLOCK"

read -r ONSET LASTLOUD PEAK <<EOF
$(python3 - "$TMP/out$BLOCK.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
n, ch = w.getnframes(), w.getnchannels()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
mono = [abs(sum(s[i*ch:(i+1)*ch]) / ch) for i in range(n)]
peak = max(mono) if mono else 0
if peak == 0:
    print("-1 -1 0"); raise SystemExit
thr = peak * 0.1
loud = [i for i, v in enumerate(mono) if v > thr]
print("%d %d %d" % (loud[0], loud[-1], int(peak)))
PY
)
EOF

echo "  block $BLOCK: note ends at tick $EDGE, audio runs samples $ONSET..$LASTLOUD (peak $PEAK)"

# IT MUST SOUND AT ALL, or the timing assertion below is vacuous on a silent render.
[ "${PEAK:-0}" -gt 500 ] || fail "the gated note did not sound at all (peak $PEAK) — this check
        cannot say anything about when a note STOPPED if it never started"

# AND IT MUST STOP. The note-off is at tick $EDGE, which is inside the first block, so the sound
# belongs to roughly the first block plus the sampler's release. A generous ceiling of four blocks
# still separates "stopped when told" from "played the whole two-second sample", which is what a
# dropped note-off produces.
LIMIT=$((BLOCK * 4))
if [ "${LASTLOUD:-999999}" -gt "$LIMIT" ]; then
  echo
  fail "the note is still sounding at sample $LASTLOUD, and its note-off was at tick $EDGE —
        within the first block. The sample is 2 s long, so this is a note that was never
        released rather than one that rang on a little.

        THE NOTE-OFF WAS DROPPED. Membership in this block must be decided by placeInBlock
        (FLOOR), not by comparing a ROUNDED sample against blockSize: a tick inside the window
        can round up to exactly blockSize and be rejected, and the branch that would have
        registered the note for a later block sits OUTSIDE the rejected one — so nothing ever
        ends the note. See apps/engine_emit_notes.cpp and apps/engine_resolve_events.cpp; the
        note-ON half of the same rule is pinned by tools/block_edge_note_check.sh."
fi
echo "  block $BLOCK: stopped by sample $LASTLOUD, within $LIMIT — the note-off was delivered"
done

echo "note_off_edge_check: PASS — a note-off on a block-boundary tick releases the note at 512" \
     "and at 64, where a dropped one would leave it sounding for the whole sample"
