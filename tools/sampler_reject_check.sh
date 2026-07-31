#!/usr/bin/env bash
# A REFUSED SAMPLER COMMAND TELLS THE CALLER, NOT JUST THE LOG.
#
# Twenty rejection sites across seven sampler commands reported refusal with DAW_EVENT and nothing
# else. daw-cli can read the engine's stderr; a browser cannot. So from a UI every one of them was
# a SILENT NO-OP THAT REPORTED SUCCESS — the web-UI agent sent SamplerSetSlot with slot 0, got
# `no_such_slot` in a log they never see, and watched the command succeed while the sound ran the
# full eight seconds. That is how it was found, and it is the general shape rather than one bug.
#
# UiDiffType::SamplerRejected (17) carries it: which command, why, and which id could not be
# found. The rule is the one PresetSaved (16) was built on — every exit reports, including the
# early refusals, because a caller that gets nothing back cannot tell "refused" from "still
# working" from "done", and the one thing it must not do is tell the user it worked.
#
# THREE PROPERTIES, and the third is the one that makes the other two mean anything:
#
#   REPORTED   an unknown slot id produces a SamplerRejected naming SamplerSetSlot (74),
#              reason NoSuchSlot (3), and the id that was not found
#   SPECIFIC   a DIFFERENT refusal on a DIFFERENT command moves BOTH fields — loading onto a
#              track that is not there gives NoSuchTrack (1) against SamplerLoad (73). One code
#              for everything would pass a single-case check while telling a caller nothing it
#              can act on, and the codes exist precisely because the fix differs: "no such slot"
#              means re-read the kit, "no such track" means the caller is addressing something
#              that is not there and retrying will never help
#   QUIET      a VALID command reports NOTHING. Without this a channel that fired on every
#              command would pass both assertions above and be useless — the same trap as a
#              read-back bit that is always set
#
#   tools/sampler_reject_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 440.0 * i / sr)))
                       for i in range(sr)))
w.close()
PY

# A REAL SAMPLER WITH A REAL SLOT, so a refusal below is about the id the caller named and not
# about the device being absent. A fixture with no slots would make "no such slot" true for the
# uninteresting reason.
python3 - "$TMP/r.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 1000, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
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
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "r"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/samprej_$$"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project r --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null || \
  fail "the engine never loaded — see $TMP/eng.log"
rcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# Reports the LAST sampler_rejected diff as "reason command target", or "none".
last_reject() {
  rcli get diffs 2>/dev/null > "$TMP/diffs.json"
  python3 - "$TMP/diffs.json" <<'PYR'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("unreadable"); raise SystemExit(0)
hits = [x for x in d.get("diffs", []) if x.get("type") == "sampler_rejected"]
print("%d %d %d" % (hits[-1]["reason"], hits[-1]["command"], hits[-1]["target"])
      if hits else "none")
PYR
}

# ---- QUIET FIRST. A valid command must report nothing, and asserting it BEFORE anything else
# also proves the ring starts clean — otherwise a stale diff from the load could be mistaken for
# the answer to a command sent later.
rcli do sampler-slot --track 0 --device 1 --slot 1 --field gate --value 1 >/dev/null 2>&1
sleep 0.6
QUIET="$(last_reject)"
[ "$QUIET" = "none" ] || \
  fail "a VALID sampler-slot command produced a rejection ($QUIET). A channel that fires on
        success cannot distinguish a refusal from anything else, and every assertion below would
        pass without meaning it"
echo "  valid command: nothing reported"

# ---- REPORTED. An unknown slot: SamplerSetSlot (74), NoSuchSlot (3), target 999.
rcli do sampler-slot --track 0 --device 1 --slot 999 --field gate --value 1 >/dev/null 2>&1
sleep 0.6
GOT="$(last_reject)"
echo "  unknown slot 999 -> reason/command/target: $GOT"
[ "$GOT" = "3 74 999" ] || \
  fail "an unknown slot id should report reason NoSuchSlot(3) for SamplerSetSlot(74) naming
        target 999, and the last rejection is '$GOT'. If it is 'none' the refusal never reached
        the outbound ring and the command still looks like a success from outside the engine —
        which is the whole defect"

# ---- SPECIFIC. A different refusal on a DIFFERENT command, so both fields have to move:
# NoSuchTrack (1) on SamplerLoad (73).
#
# Not the out-of-range filter type, which was the first thing tried: daw-cli validates --type
# against off|lp12|lp24|hp|bp and refuses client-side, so the engine never sees it. That is the
# CLI behaving correctly and the probe being wrong — a check cannot exercise an engine path
# through a caller that will not send it.
rcli do sampler-load --track 99 --file s.wav --root 60 >/dev/null 2>&1
sleep 0.6
GOT2="$(last_reject)"
echo "  load on track 99 -> reason/command/target: $GOT2"
[ "$GOT2" = "1 73 0" ] || \
  fail "loading onto a track that does not exist should report reason NoSuchTrack(1) for
        SamplerLoad(73), and the last rejection is '$GOT2'. If it is still '3 74 999' the second
        refusal never reached the ring; if the reason is 3 the codes are not distinct and a caller
        cannot tell 're-read the kit' from 'that track is not there' — which is the only thing the
        codes are for"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

echo "sampler_reject_check: PASS — refusals reach the caller, carry distinct reasons, and a"
echo "                      valid command stays silent"
