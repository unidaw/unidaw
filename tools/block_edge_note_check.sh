#!/usr/bin/env bash
# A NOTE ON A BLOCK-BOUNDARY TICK MUST SOUND WHERE IT IS WRITTEN, not one loop later.
#
# This is a deterministic reproducer for the symptom task #16 describes as "nothing sounded on the
# note side at the head of the render", and it is not a flake — it is arithmetic, and it fires every
# time at these settings.
#
# THE MECHANISM. A nanotick is far smaller than a sample: at 120 bpm, 44.1 kHz and 960000 nanoticks
# to the quarter, samplesPerTick is 0.0229688, so a 512-frame block spans 22291 ticks. placeInBlock
# decided whether an event belonged to this block using the ROUNDED sample — and ticks 22270..22291
# are INSIDE the window while rounding to 512, the first sample of the NEXT block. Those were
# rejected. The caller skipped them, and the strike came back through the pending-strike path a
# whole loop later.
#
# MEASURED, and this is what the check pins:
#
#     note written at nanotick 22275
#     placeInBlock deciding by ROUND   first onset at sample 88714   (2.0002 s late)
#     placeInBlock deciding by FLOOR   first onset at sample   513
#
# 88201 samples at 44.1 kHz is 2.0002 s, which at 120 bpm is exactly one bar — one loop pass. So
# the two readings task #16 could not separate ("the head note was dropped" versus "the note stream
# is one loop late") are the SAME event seen from either end, and this is it.
#
# THE RATE IS EXACTLY 0.5/blockSize, and it is worth stating because it is not small where it
# matters. The drop band is half a sample wide in ticks (0.5/samplesPerTick) and a block spans
# blockSize/samplesPerTick ticks, so samplesPerTick cancels: the probability that any given event
# lands in it depends on the BUFFER SIZE ALONE — not on tempo and not on sample rate. Measured
# across 90/120/174 bpm and 44.1/48 kHz, every combination agrees with the formula:
#
#     block 1024 -> 0.049%      block  256 -> 0.195%
#     block  512 -> 0.098%      block   64 -> 0.781%
#
# At 64 frames that is roughly one note in 128 displaced by a whole loop, which is not a curiosity
# for someone tracking at low latency. THAT is why this check renders at two buffer sizes: the
# small one is where a user would actually meet it, and it is eight times likelier to catch a
# regression than the large one.
#
# WHY IT SURVIVED. It needs a note at an UNQUANTISED position to show at all, and no fixture had
# one — everything in the suite puts notes on beats, where tickDelta is 0 and the conversion is
# exact. That is why it needed a fixture of its own rather than more runs of an existing one, and
# it is the answer to "why did repeated runs under load never reproduce it": no run of those
# fixtures ever could.
#
# Needs the engine built. Renders offline: no device, no wall clock, byte-deterministic.
#   tools/block_edge_note_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

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

# THE TICK IS COMPUTED, NOT COPIED. If the block size or the tempo in this file ever change, a
# hard-coded 22275 would quietly stop being a boundary tick and the check would pass by testing an
# ordinary position — green, and measuring nothing.
SR=44100
BPM=120
for BLOCK in 512 64; do
EDGE="$(python3 - "$Q" "$BPM" "$SR" "$BLOCK" <<'PY'
import sys
Q, bpm, sr, block = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
spt = (60.0 / bpm) * sr / Q
edges = [d for d in range(block * 200) if int(d * spt) < block <= round(d * spt)]
if not edges:
    raise SystemExit("no boundary tick exists at these settings")
print(edges[len(edges) // 2])   # the middle of the band, not its edge
PY
)"
[ -n "$EDGE" ] || fail "could not compute a boundary tick for ${BPM}bpm/${SR}Hz/${BLOCK}"

python3 - "$TMP" "$Q" "$EDGE" <<'PY'
import json, sys, wave, struct, math, os
tmp, Q, edge = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
sr = 48000
w = wave.open(os.path.join(tmp, "t.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(14000 * math.sin(2 * math.pi * 440.0 * i / sr)
                          * max(0.0, 1 - i / float(sr // 2))))
    for i in range(sr // 2)))
w.close()

def route(k="none", t=0):
    return {"kind": k, "track_id": t, "input_id": 0}

slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0, "start_frame": 0,
        "end_frame": 0, "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0, "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0, "vel_low": 0, "vel_high": 127,
        "layer_group": 0, "select_mode": 0, "gate": 0, "reverse": 0, "gain_millibels": 0,
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
# ONE note, at the boundary tick. One note so the onset is unambiguous: with two, a check could
# not tell a deferred first note from a second note arriving on time.
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic",
        "notes": [{"nanotick": edge, "duration": Q, "pitch": 60, "velocity": 110,
                   "column": 0, "note_id": 1}]}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                  "audio_out": route("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "edge"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(os.path.join(tmp, "edge.uniproj.json"), "w"))
PY

( cd "$BUILD" && env DAW_UI_SHM_NAME="/bedge_$$" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project edge --render out --run-seconds 3 \
    --block-size "$BLOCK" --sample-rate "$SR" >"$TMP/eng.log" 2>&1 ) \
  || fail "the render exited non-zero — see $TMP/eng.log"
[ -s "$TMP/out.wav" ] || fail "the render wrote no output"

read -r ONSET PEAK <<EOF
$(python3 - "$TMP/out.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
n, ch = w.getnframes(), w.getnchannels()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
mono = [abs(sum(s[i*ch:(i+1)*ch]) / ch) for i in range(n)]
peak = max(mono) if mono else 0
if peak == 0:
    print("-1 0"); raise SystemExit
thr = peak * 0.1
print("%d %d" % (next(i for i, v in enumerate(mono) if v > thr), int(peak)))
PY
)
EOF

echo "  note at nanotick $EDGE (block $BLOCK, ${SR}Hz, ${BPM}bpm) -> onset sample $ONSET, peak $PEAK"

# THE NOTE MUST SOUND AT ALL. Without this the timing assertion below would be vacuous on a render
# that produced silence — an onset of -1 is not an early onset.
[ "${PEAK:-0}" -gt 500 ] || fail "the boundary note did not sound at all (peak $PEAK)"

# AND IT MUST SOUND WHERE IT WAS WRITTEN. tick $EDGE is inside the FIRST block, so its onset
# belongs within a block or so of the start — not a bar later.
#
# The pre-fix engine put it at sample 88714, which is 2.0002 s at 44.1 kHz and exactly one bar at
# 120 bpm: the strike was rejected by placeInBlock, skipped by the caller, and came back through
# the pending-strike path one whole loop later. That is the "note stream one loop late" reading in
# task #16, reproduced on demand rather than waited for.
LIMIT=$((BLOCK * 4))
if [ "${ONSET:-999999}" -gt "$LIMIT" ]; then
  echo
  fail "the note written at nanotick $EDGE first sounds at sample $ONSET, and it should be within
        $LIMIT of the start — it is in the FIRST block.

        This is the block-boundary rounding defect: placeInBlock decides whether an event belongs
        to this block, and deciding that on the ROUNDED sample rejects ticks that are inside the
        window but round up to blockSize. The caller skips them and the strike returns a whole loop
        later, which is what 'the note stream is one loop late' means in task #16.

        Membership must be decided by FLOOR and the position rounded and then clamped. A genuinely
        later event must still be rejected — clamping those bunches every future event onto the
        boundary — which is why the two are separate questions."
fi

  echo "  block $BLOCK: onset $ONSET, within $LIMIT — not a loop late"
done

echo "block_edge_note_check: PASS — a boundary note sounds where it is written at 512 and at 64," \
     "where the defect is eight times likelier (the rate is 0.5/blockSize)"
