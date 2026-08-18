#!/usr/bin/env bash
# ROUTING A TRACK'S MIDI TO ANOTHER TRACK MUST WORK IN BOTH DIRECTIONS.
#
# THE DEFECT. The source stamps its routed events for the NEXT block — `routed.sampleTime =
# nextBlockSampleStart + offset` at the end of its processTrack — and the destination drains them
# at the TOP of its renderTrack, against THIS block's window. The drain SWAPS THE WHOLE QUEUE OUT
# and keeps only what lands inside that window:
#
#     runtime.inboundMidiEvents.swap(inboundEvents);
#     runtime.inboundMidiEvents.clear();          // everything is now in hand, or gone
#     ...
#     if (entry.sampleTime >= blockSampleEnd) continue;   // and this DISCARDS it
#
# So an event stamped for block N+1 is consumed during block N and never seen again. Whether that
# happens depends only on the ORDER the tracks are processed in, which is track-id order:
#
#     route track 1 -> track 0   destination runs FIRST, queue is drained on the block it is
#                                stamped for, the notes play
#     route track 0 -> track 1   destination runs LATER IN THE SAME BLOCK, swallows events
#                                stamped for the next one, EVERY EVENT IS DELETED
#
# Reverse the two track ids and the same project works. That is why this reads as "routing is
# flaky" rather than as a rule with a direction.
#
# WHY NOTHING CAUGHT IT: no fixture in tools/ sets `midi_out` at all. Probed directly — an abort()
# at the top of the destination's inbound-drain loop does not fire in ANY of the 189 registered
# tests, so the whole receive path was unexecuted. This check is the first fixture that routes.
#
# BOTH DIRECTIONS ARE RENDERED, and that is the assertion: a routing rule that works one way round
# is not a routing rule. The instrument is the repo's Identity fixture, which emits a short pulse
# per note-on, so "did the destination hear it" is a peak rather than an interpretation.
#
# Needs the engine and identity_plugin_VST3 built. Renders offline: byte-deterministic.
#   tools/midi_route_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/identity_plugin.sh"
BUILD="$ROOT/build"
Q=960000
SR=44100

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
# WHERE THE BUNDLE IS, asked of tools/lib/identity_plugin.sh rather than typed. The flat
# `identity_plugin_artefacts/VST3/` path this used to hardcode is a layout JUCE stopped
# emitting long ago (tools/webstack.sh records the same finding); it survives only as a
# leftover in build directories that were configured before the change, so on a FRESH
# checkout this check bailed with "build identity_plugin first" and read as a regression.
IDENTITY="$(resolve_identity_vst3 "$BUILD")" || IDENTITY="$BUILD/identity_plugin_artefacts/VST3/Identity.vst3"
[ -d "$IDENTITY" ] || { echo "build identity_plugin_VST3 first"; exit 2; }

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

# SRC is the track that owns the notes and routes them away; DST carries the instrument.
# Rendered once with SRC=0/DST=1 and once with SRC=1/DST=0 — same project otherwise.
for PAIR in "0 1" "1 0"; do
  set -- $PAIR
  SRC=$1; DST=$2
  python3 - "$TMP" "$Q" "$IDENTITY" "$SRC" "$DST" <<'PY'
import json, sys, os
tmp, Q, ident, src, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
BAR = Q * 4
def route(kind="none", track=0):
    return {"kind": kind, "track_id": track, "input_id": 0}

# Four notes, well inside the block grid, so nothing here depends on boundary arithmetic.
notes = [{"nanotick": i * (Q // 2), "duration": Q // 4, "pitch": 60 + i, "velocity": 110,
          "column": 0, "note_id": i + 1} for i in range(4)]
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic", "notes": notes}

# THE SOURCE has the notes and NO instrument: everything it makes must leave via midi_out.
source = {"track_id": src, "name": "SRC", "harmony_quantize": False, "lines_per_beat": 4,
          "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
          "routing": {"midi_in": route(), "midi_out": route("track", dst),
                      "audio_in": route(), "audio_out": route("master"),
                      "pre_fader_send": True},
          "device_chain": [], "mod_links": [],
          "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                          "notes": [], "chords": [], "mutes": []}]}
# THE DESTINATION has the instrument and NO notes of its own, so any sound it makes arrived
# over the routing. That separation is what makes the peak below unambiguous.
dest = {"track_id": dst, "name": "DST", "harmony_quantize": False, "lines_per_beat": 4,
        "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
        "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                    "audio_out": route("master"), "pre_fader_send": True},
        "device_chain": [{"device_id": 1, "kind": "vst_instrument", "capability_mask": 5,
                          "patcher_node_id": 4294967295, "host_slot_index": 4294967294,
                          "bypass": False,
                          "vst_ref": {"vendor": "daw", "name": "Identity", "path": ident,
                                      "uid16": ""}}],
        "mod_links": [], "placements": []}
tracks = sorted([source, dest], key=lambda t: t["track_id"])
json.dump({"schema_version": 4, "meta": {"name": "route%d%d" % (src, dst)},
           "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": tracks},
          open(os.path.join(tmp, "route%d%d.uniproj.json" % (src, dst)), "w"))
PY

  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/mroute_${SRC}${DST}_$$" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "route${SRC}${DST}" --render "out${SRC}${DST}" --run-seconds 3 \
      --sample-rate "$SR" >"$TMP/eng${SRC}${DST}.log" 2>&1 ) \
    || fail "the render exited non-zero for ${SRC}->${DST} — see $TMP/eng${SRC}${DST}.log"
  [ -s "$TMP/out${SRC}${DST}.wav" ] || fail "the render wrote no output for ${SRC}->${DST}"

  PEAK="$(python3 - "$TMP/out${SRC}${DST}.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb'); n, ch = w.getnframes(), w.getnchannels()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PY
)"
  echo "  routing track $SRC -> track $DST: peak $PEAK"
  eval "PEAK_${SRC}${DST}=$PEAK"
done

# BOTH DIRECTIONS MUST SOUND. The destination has no notes of its own, so a silent render means
# its instrument never received the routed events.
for PAIR in "0 1" "1 0"; do
  set -- $PAIR
  SRC=$1; DST=$2
  eval "P=\$PEAK_${SRC}${DST}"
  if [ "${P:-0}" -le 500 ]; then
    echo
    fail "routing track $SRC -> track $DST produced silence (peak $P), while the other direction
        did not. The destination track carries the instrument and no notes of its own, so this is
        every routed event being deleted.

        THE SOURCE STAMPS FOR THE NEXT BLOCK and the destination drains against THIS one, taking
        the WHOLE queue and discarding whatever falls outside its window. When the destination is
        processed after the source in the same block it swallows events stamped for the next one.
        Which of the two happens depends on track-id order, which is why the same project works
        with the ids reversed. An event stamped ahead must be kept for the block it belongs to,
        not consumed and dropped — see the inbound drain at the top of renderTrack."
  fi
done

echo "midi_route_check: PASS — a track's MIDI reaches its destination's instrument in both" \
     "directions (peaks $PEAK_01 and $PEAK_10), not only when the ids happen to be ordered"
