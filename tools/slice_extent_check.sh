#!/usr/bin/env bash
# THE SLICES OF A CHOP TILE THE SOURCE — no gap, no overlap, nothing off the end.
#
# The chop PLAYED and could not be SEEN. Every slot reported `lengthFrames` as the SOURCE's
# length, so nothing could draw where a slice begins or how long it is; the extent was computed at
# note-on from the marker list and never published. Dragging a marker, nudging a boundary, seeing
# that slice 3 is twice slice 4 — all of it needs the extent and none of it was derivable from
# what was published. That is the gap between "the chop plays" and "the chop is editable".
#
# THE ASSERTION IS THE WEB-UI AGENT'S, and it is better than anything I had: an eight-way chop
# whose extents TILE the source exactly. begin[0] is 0, end[7] is the frame count, every end
# equals the next begin. It knows nothing about the implementation — no marker ids, no internal
# helper — so it holds whatever the slicer does, and it fails loudly if a re-chop ever leaves a
# hole.
#
# IT ALSO CATCHES A BUG THAT WAS REAL. sliceExtentAt began every slice AT a marker, so frame 0
# fell into a gap and the FIRST slice was unreachable until insertSliceMarker learned to accept
# frame 0. A tiling assertion notices that without being told to look for it, which is what makes
# it worth more than "slice 1 sounds different from slice 8".
#
# FOUR PROPERTIES:
#   COUNT     the chop made the slots it claimed to
#   ORIGIN    the first slice starts at frame 0 — the gap that was real
#   COVER     the last slice ends at the source's last frame, so nothing is off the end
#   TILE      every slice's end is the next one's begin: no gap, no overlap
#
# AND ONE THAT IS NOT ABOUT SLICES: a slot with NO slice publishes the WHOLE SOURCE rather than
# zeroes, because "0,0 means the whole thing" is a sentinel that reads as a bug at the exact
# moment somebody is looking for one.
#
#   tools/slice_extent_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
Q=960000
FRAMES=96000   # exactly 2 s at 48k, and divisible by 8 so an equal chop has no remainder to hide

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP/brk.wav" "$FRAMES" <<'PY'
import sys, wave, struct, math
sr = 48000
n = int(sys.argv[2])
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
# EIGHT bursts, so the file at least looks like something worth chopping. The extents do not
# depend on the audio — an equal chop divides by count — but a fixture that is audibly a break
# fails in a way somebody can listen to.
w.writeframes(b''.join(
    struct.pack('<h', int(13000 * math.sin(2 * math.pi * 300.0 * i / sr) *
                          math.exp(-18.0 * ((i % (n // 8)) / sr)))) for i in range(n)))
w.close()
PY

# A sampler with the source loaded and ONE plain slot on it. The plain slot is the second
# property: it has no slice, so it must publish the whole source rather than zeroes.
python3 - "$TMP/x.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "whole", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 24, "key_high": 24, "root_key": 24,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "brk.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
tr = {"track_id": 0, "name": "X", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "x"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/slicext_$$"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project x --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
xcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
sleep 1.0

# ---- THE PLAIN SLOT, before any chop exists. A slot with no slice covers its whole source.
xcli get sampler-kit --track 0 --device 1 >"$TMP/before.json" 2>/dev/null
python3 - "$TMP/before.json" "$FRAMES" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1])); frames = int(sys.argv[2])
s = [x for x in d.get("slots", []) if x.get("slice") == 0]
if not s:
    print("  FAIL: the fixture's unsliced slot is not in the kit answer"); raise SystemExit(1)
e = s[0]
print("  unsliced slot: begin %s end %s (source is %d frames)"
      % (e.get("slice_begin"), e.get("slice_end"), frames))
if e.get("slice_begin") != 0 or e.get("slice_end") != frames:
    print("  FAIL: a slot with NO slice must publish the WHOLE source (0..%d) and publishes"
          " %s..%s. Zeroes would be a sentinel meaning 'the whole thing', which reads as a bug"
          " at exactly the moment somebody is looking for one." % (frames, e.get("slice_begin"),
                                                                   e.get("slice_end")))
    raise SystemExit(1)
PYC

# ---- CHOP IT EIGHT WAYS.
# --mode equal --count 8, NOT "--equal 8": unknown flags are ignored, so the wrong spelling
# silently ran a TRANSIENT chop instead and produced seven slices starting at 11904 — the check
# then read as a product bug when it was the invocation.
xcli do sampler-slice --track 0 --device 1 --source 1 --mode equal --count 8 >/dev/null 2>&1
sleep 1.2
xcli do sampler-emit-rows --track 0 --device 1 --source 1 >/dev/null 2>&1
sleep 1.2
xcli get sampler-kit --track 0 --device 1 >"$TMP/after.json" 2>/dev/null

python3 - "$TMP/after.json" "$FRAMES" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1])); frames = int(sys.argv[2])
sliced = [s for s in d.get("slots", []) if s.get("slice", 0) != 0]
sliced.sort(key=lambda s: s.get("slice_begin", 0))
print("  %d sliced slots: %s" % (len(sliced),
      ", ".join("%s..%s" % (s.get("slice_begin"), s.get("slice_end")) for s in sliced)))

# ---- COUNT.
if len(sliced) != 8:
    print("  FAIL: an eight-way chop produced %d sliced slots. Nothing below can tile a source"
          " that was not cut into the pieces it claims." % len(sliced))
    raise SystemExit(1)
# ---- ORIGIN. The gap that was real: sliceExtentAt began every slice AT a marker, so frame 0
# fell outside every slice and the first was unreachable.
if sliced[0].get("slice_begin") != 0:
    print("  FAIL: the first slice starts at frame %s, not 0. Frame 0 is outside every slice, so"
          " the first slice is unreachable — which is exactly the bug insertSliceMarker accepting"
          " frame 0 was fixing." % sliced[0].get("slice_begin"))
    raise SystemExit(1)
# ---- COVER.
if sliced[-1].get("slice_end") != frames:
    print("  FAIL: the last slice ends at %s and the source is %d frames. The tail is off the end"
          " of every slice and cannot be played." % (sliced[-1].get("slice_end"), frames))
    raise SystemExit(1)
# ---- TILE.
for i in range(len(sliced) - 1):
    end, nxt = sliced[i].get("slice_end"), sliced[i + 1].get("slice_begin")
    if end != nxt:
        kind = "a GAP" if nxt > end else "an OVERLAP"
        print("  FAIL: %s between slice %d and slice %d — %s ends at %s and %s begins at %s."
              " The slices must tile the source exactly." % (kind, i, i + 1, i, end, i + 1, nxt))
        raise SystemExit(1)
print("  the eight slices tile 0..%d exactly" % frames)
PYC

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "slice_extent_check: PASS — the chop's slices tile the source, and an unsliced slot covers"
echo "                    all of it"
