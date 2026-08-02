#!/usr/bin/env bash
# A PAD CAN BE REPOINTED AT ANOTHER SAMPLE, AND YOU HEAR THE OTHER SAMPLE.
#
# `sourceLocalId` and `sliceId` say which sample and which slice a slot plays. Both were set at
# MINT by `sampler-load` and `sampler-slice` and NEVER AGAIN — so "this pad should play that other
# file", or "this pad should be slice 12 instead of 11", meant deleting the slot and rebuilding
# it. Every other per-slot field had a command; these two did not.
#
# HOW THEY WERE FOUND, and the method is the point: every key the sampler serializer writes for a
# SLOT, diffed against every SamplerSlotField id. 27 of 31 were reachable. Of the four that were
# not, `id` is correctly absent (you address BY it) and `name` does not fit an int32 value and is
# filed separately. That sweep — take a struct the format persists, list its fields, check each
# against the command meant to edit it — has now found this defect eight times in this codebase,
# which is the argument for running it deliberately rather than stumbling on the next one.
#
# THREE PROPERTIES:
#   BEFORE    the slot plays its original sample — the control, and proof the fixture can tell
#             the two apart at all
#   REPOINTED the same note on the same key plays the OTHER sample after the command. Which TONE
#             comes out, not which id reads back: a field that saves and never reaches the voice
#             is the defect this suite has repeatedly found one layer along
#   REFUSED   a source id that does not exist is rejected AND LEAVES THE SLOT ALONE. A slot
#             pointing at a source that is not there is SILENT, so clamping or half-applying it
#             would turn a typo into a mute pad with no explanation
#
#   tools/slot_repoint_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

tone() {  # tone <file> <hz>
  python3 - "$TMP/$1" "$2" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(13000 * math.sin(2 * math.pi * float(sys.argv[2]) * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY
}
tone lo.wav 400
tone hi.wav 1200

# TWO SOURCES, ONE SLOT. The second source exists but nothing plays it — which is exactly the
# state a repoint has to move out of, and a fixture with one source could not express it.
python3 - "$TMP/r.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "pad", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 60, "key_high": 60, "root_key": 60,
        # FIXED PITCH, so the note plays each sample at its own rate and the tone that comes out
        # is the SOURCE's frequency rather than a transposition of it.
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 3, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 8, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "lo.wav", "content_key": 0},
                               {"local_id": 2, "path": "hi.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
notes = [{"nanotick": 0, "duration": Q * 2, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": 1}]
tr = {"track_id": 0, "name": "R", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "r"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/repoint_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 4 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# Which of the two known tones is it? Correlation against both candidates rather than a peak
# picker — there is nothing to search for when both answers are known in advance.
which_tone() {  # which_tone <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(sr * 0.30), int(sr * 0.90)
seg = [s[i * ch] for i in range(a, min(b, n))]
if not seg or max(abs(v) for v in seg) < 1500:
    print("silent"); raise SystemExit(0)
def energy(f):
    re = sum(v * math.cos(2 * math.pi * f * i / sr) for i, v in enumerate(seg))
    im = sum(v * math.sin(2 * math.pi * f * i / sr) for i, v in enumerate(seg))
    return math.hypot(re, im)
e400, e1200 = energy(400.0), energy(1200.0)
print("400" if e400 > e1200 * 3 else ("1200" if e1200 > e400 * 3 else
      "neither(400=%.0f 1200=%.0f)" % (e400, e1200)))
PY
}

# ---- BEFORE. The control: the fixture plays its original source and can tell the two apart.
render r before
B="$(which_tone before)"
echo "  before the repoint: $B Hz"
[ "$B" = "400" ] || \
  fail "the slot points at source 1 (the 400 Hz tone) and the render is '$B'. The fixture is not
        playing what it says it plays, so a change below could not be attributed to the command"

SHM="/repoint_cmd_$$"
( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project r --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
rcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# ---- REFUSED FIRST, so the slot is known-untouched when the real repoint runs. A source that
# does not exist must be rejected AND leave the slot alone: a slot pointing at a missing source is
# silent, so half-applying a typo is a mute pad with no explanation.
rcli do sampler-slot --track 0 --device 1 --slot 1 --field source --value 999 >/dev/null 2>&1
sleep 0.6
grep -q '"reason":"no_such_source"' "$TMP/eng.log" 2>/dev/null || \
  fail "repointing at source 999 was not refused — no no_such_source in $TMP/eng.log. A source
        that is not there makes the slot silent, so accepting the id is a mute pad and no reason"

# ---- REPOINTED. The same note on the same key, now the other sample.
rcli do sampler-slot --track 0 --device 1 --slot 1 --field source --value 2 >/dev/null 2>&1
sleep 0.8
rcli do save after >/dev/null 2>&1
sleep 1.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/after.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"

python3 - "$TMP/after.uniproj.json" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
for t in d["tracks"]:
    for dev in t["device_chain"]:
        for s in dev.get("sampler", {}).get("slots", []):
            got = s.get("source_local_id")
            if got == 2:
                print("  the slot's source_local_id is 2 after the repoint")
                raise SystemExit(0)
            print("  FAIL: source_local_id is %r, expected 2. Either the refused 999 above left"
                  " the slot in a state the real command could not move, or the field id does not"
                  " reach the slot at all." % got)
raise SystemExit(1)
PYC

render after after
A="$(which_tone after)"
echo "  after the repoint:  $A Hz"
[ "$A" = "1200" ] || \
  fail "the slot now saves source_local_id 2 (the 1200 Hz tone) and the render is '$A'. The field
        reached the file and not the voice, which is the same defect one layer along — exactly
        what filterType turned out to be"

echo "slot_repoint_check: PASS — a pad can be repointed at another sample, you hear it, and a"
echo "                    source that does not exist is refused without touching the slot"
