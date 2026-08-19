#!/usr/bin/env bash
# AUDIO ARRIVES ONE BLOCK LATE, AND THE SAME ONE BLOCK WHICHEVER TRACK IS PROCESSED FIRST.
#
# AE-P1.2 G2-B item 18, R-ROUTING-AUTHORITY:
#
#   "Every MIDI, audio, and sidechain Track edge delivers the source's fully rendered block N-1 to
#    destination block N, so graph cycles are legal explicit one-block-per-edge feedback and
#    runtime/worker order cannot change same- versus next-block delivery."
#
# WHY THIS CHECK HAD TO BE WRITTEN, and it is the whole reason it exists. The producer used ONE
# inbound buffer per track, consumed at the start of a destination's block and written at the end of
# a source's — both inside the same block. The engine's own comment said what follows: "Whether the
# destination sees this block's audio or next block's therefore depends on which of the two runs
# first." A serial group PINNED the order instead of removing the dependence on it.
#
# The nine checks that touch routing — midi_route, sidechain, sampler_routed_input, master_mixer,
# master_fx, master_track, multiout, multiout_persist, aux_child_fidelity — ALL PASS with the defect
# restored. Measured, not assumed: the two-buffer fix was reverted to a single buffer and every one
# of the nine stayed green. For AUDIO the destination hears the source either way; only WHEN differs,
# and nothing was measuring when.
#
# THE DISCRIMINATOR IS THE DIFFERENCE BETWEEN TWO RENDERS, not an absolute position. The same project
# is rendered twice with the two track ids SWAPPED, which swaps their processing order. Under the
# defect one render delivers in the same block and the other a block later, so the first audible
# sample moves by exactly one block. Under the rule both are one block late and the two positions are
# equal. Comparing the two renders means this does not need to know the pipeline's absolute latency —
# only that reordering cannot change it, which is exactly what the record forbids.
#
# NOT IN ctest, AND WHY. This check is FLAKY as written. Eight consecutive runs disagreed three
# times: one direction's audio ended 3 to 4 blocks earlier than in the other seven runs, in EITHER
# direction, independently of which way the routing went. The property it looks for is a ONE-block
# difference between the orderings, so the noise is larger than the signal and the check cannot
# answer its own question.
#
# Three renders of ONE project back to back are byte-identical, so this is not the renderer being
# nondeterministic in the small. Something varies across separated runs and it is not yet known
# what. Run it by hand; do not trust a single result in either direction.
#
#   tools/routing_block_determinism_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
. "$ROOT/tools/lib/identity_plugin.sh"
BUILD="$ROOT/build"
[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
IDENTITY="$(resolve_identity_vst3 "$BUILD")" || { echo "  SKIP: no Identity.vst3 built"; exit 0; }

TMP="$(mktemp -d)"
# KEEP_TMP preserves the renders. Kept because diagnosing this check means looking at the WAVs, and
# the first version of it was blind for a reason only the samples could show.
cleanup() { [ -n "${KEEP_TMP:-}" ] && { echo "  kept: $TMP"; return; }; rm -rf "$TMP"; }
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

Q=960000
BLOCK=256

for PAIR in "0 1" "1 0"; do
  set -- $PAIR
  SRC=$1; DST=$2
  python3 - "$TMP" "$Q" "$IDENTITY" "$SRC" "$DST" <<'PY'
import json, sys, os
tmp, Q, ident, src, dst = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
BAR = Q * 4
def route(kind="none", track=0):
    return {"kind": kind, "track_id": track, "input_id": 0}

# ONE note, at tick 0, so the first audible sample is unambiguous.
notes = [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 120, "column": 0, "note_id": 1}]
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic", "notes": notes}

# THE SOURCE makes the sound and sends its AUDIO to the destination. audio_out = Track means it does
# NOT also reach master, so every sample in the render arrived over the routing edge.
source = {"track_id": src, "name": "SRC", "harmony_quantize": False, "lines_per_beat": 4,
          "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
          "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                      "audio_out": route("track", dst), "pre_fader_send": True},
          "device_chain": [{"device_id": 1, "kind": "vst_instrument", "capability_mask": 5,
                            "patcher_node_id": 4294967295, "host_slot_index": 4294967294,
                            "bypass": False,
                            "vst_ref": {"vendor": "daw", "name": "Identity", "path": ident,
                                        "uid16": ""}}],
          "mod_links": [],
          "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                          "notes": [], "chords": [], "mutes": []}]}
# THE DESTINATION makes nothing of its own and is the only path to master.
dest = {"track_id": dst, "name": "DST", "harmony_quantize": False, "lines_per_beat": 4,
        "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
        "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                    "audio_out": route("master"), "pre_fader_send": True},
        "device_chain": [], "mod_links": [], "placements": []}
tracks = sorted([source, dest], key=lambda t: t["track_id"])
json.dump({"schema_version": 4, "meta": {"name": "det%d%d" % (src, dst)},
           "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": tracks},
          open(os.path.join(tmp, "det%d%d.uniproj.json" % (src, dst)), "w"))
PY
  [ $? -eq 0 ] || fail "could not write the fixture for ${SRC}->${DST}"

  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/rdet_${SRC}${DST}_$$" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "det${SRC}${DST}" --render "out${SRC}${DST}" --sample-rate 44100 --run-seconds 3 \
      --block-size "$BLOCK" > "$TMP/eng${SRC}${DST}.log" 2>&1 ) \
    || fail "the render exited non-zero for ${SRC}->${DST} — see $TMP/eng${SRC}${DST}.log"
  [ -s "$TMP/out${SRC}${DST}.wav" ] || fail "the render wrote no output for ${SRC}->${DST}"
done

# THE LAST AUDIBLE SAMPLE IN EACH RENDER, and the difference between them.
#
# NOT THE FIRST, and the first version of this check measured the first and was blind. The Identity
# fixture emits a constant level from sample 0 — it is not note-gated — so "where does the audio
# start" is 0 in every render whatever the routing does. Measured, not assumed: both renders reported
# first-audible 0 under the fix AND under a restored single buffer, so the check passed with the
# defect present.
#
# The END moves, because the source's last block still has to arrive somewhere. Under the defect one
# ordering delivers same-block and the other next-block, so the two renders end ONE block
# apart; under the rule both land on the same sample.
#
# NO NUMBER IS QUOTED HERE ON PURPOSE. An earlier version of this comment offered specific sample
# indices as evidence the fix works, thirty lines below a sentence saying a single result from this
# script cannot be trusted in either direction. Both cannot be true. The values it quoted came from
# runs whose spread is larger than the difference being measured. An intermediate version of this
# comment also said two of those runs overlapped a ctest run; independent review reconstructed every
# ctest window from the logs and NONE of the kept runs falls inside one. That claim is withdrawn.
# What the timestamps DO show is that the kept runs overlapped EACH OTHER -- three finished within
# eight seconds, while a single run performs two three-second offline renders. Concurrent instances
# of this script are a contamination it does not defend against.
python3 - "$TMP" "$BLOCK" <<'PYEND'
import struct, sys, os
tmp, block = sys.argv[1], int(sys.argv[2])

def samples(path):
    data = open(path, "rb").read()
    i, off, size, bits = 12, None, 0, 16
    while i + 8 <= len(data):
        cid = data[i:i+4]; csz = struct.unpack("<I", data[i+4:i+8])[0]
        if cid == b"fmt ":
            bits = struct.unpack("<H", data[i+8+14:i+8+16])[0]
        elif cid == b"data":
            off, size = i + 8, csz
            break
        i += 8 + csz + (csz & 1)
    if off is None:
        return None
    if bits == 16:
        n = size // 2
        return struct.unpack("<%dh" % n, data[off:off + n * 2])
    n = size // 4
    return [int(v * 32768) for v in struct.unpack("<%df" % n, data[off:off + n * 4])]

def last_audible(v, floor=32):
    for i in range(len(v) - 1, -1, -1):
        if abs(v[i]) > floor:
            return i
    return None

a = samples(os.path.join(tmp, "out01.wav"))
b = samples(os.path.join(tmp, "out10.wav"))
if a is None or b is None:
    sys.exit("  FAIL: a render produced no data chunk")
la, lb = last_audible(a), last_audible(b)
if la is None or lb is None:
    print("  FAIL: a render is silent (0->1: %r, 1->0: %r). The source routes its audio to the" % (la, lb))
    print("        destination and NOT to master, so silence means the edge delivered nothing.")
    sys.exit(1)
print("  last audible sample: 0->1 at %d, 1->0 at %d" % (la, lb))
if la != lb:
    blocks = abs(la - lb) / float(block * 2)
    print("  FAIL: swapping the two track ids moved the end of the audio by %d samples (%.1f blocks)."
          % (abs(la - lb), blocks))
    print("        The ids decide processing order and nothing else in this project changed, so")
    print("        delivery is still same-block one way and next-block the other —")
    print("        'runtime/worker order cannot change same- versus next-block delivery'.")
    sys.exit(1)
print("  and they are EQUAL, so processing order does not change when the audio arrives")
PYEND
[ $? -eq 0 ] || exit 1

# A GREEN RUN FROM THIS SCRIPT IS NOT A RESULT. Its run-to-run variation is larger than the
# difference it measures, so one agreement is what noise looks like as often as it is what the rule
# looks like. The word PASS is kept only so the exit status reads conventionally; what it means is
# "the two renders agreed THIS time". The gate for this rule is the inbound_audio test.
echo "routing_block_determinism_check: the two renders AGREED this time — not a verdict." \
     "This check is declared unregistered (tools/check_registry_check.sh); its noise exceeds its" \
     "signal. The gate for one-block delivery is the inbound_audio test"
exit 0
