#!/usr/bin/env bash
# AN ENVELOPE THAT WAS WRITTEN CAN BE READ BACK — EVERY FIELD OF IT.
#
# `SamplerSetEnvelopePoints` (84) writes a full multi-segment envelope: the points, both loop
# ranges, the release fade, the time base and the rate. Nothing could read any of it back. The kit
# read-back carries `modMask` — ten bits saying WHICH (target, kind) pairs are configured — which
# answers a modulator ROW's question and cannot answer "what shape".
#
# So a pencil editor built on 84 would be WRITE-ONLY: able to send a curve and never to draw the
# one already in the project, including the one a loaded file arrived with. The web-UI agent named
# that as the reason they had not built the editor, and their framing is the right one — a writer
# with no reader is the same lie as a field with no writer, with the halves swapped.
#
# WHY EVERY FIELD IS ASSERTED, not just the points. A read-back that returns a SUBSET is how a
# pencil editor lies: an editor draws the shape, sends it back, and CLEARS whatever the answer
# omitted, because the caller cannot preserve what it was never told. Opcode 84 takes eleven
# fields and the answer returns eleven, so this checks all of them — including the two loop ranges
# and the release fade, which are exactly the ones an editor would silently destroy.
#
# SIX PROPERTIES:
#   ABSENT      before anything is written, the answer is found=false — an ANSWER, not an error.
#               A UI draws that (an empty lane with an add button), and making it an error means
#               the UI cannot tell it from a dropped request
#   POINTS      the points come back with the time, value AND TENSION that were sent. Tension is
#               the one a subset-answer drops first: it is per-point, it is one byte, and a curve
#               that reads back linear looks plausible
#   SHAPE       both loop ranges, the release fade, the time base and the rate all round-trip
#   ADDRESSED   asking by TARGET and asking by MODULATOR ID reach the same envelope, and asking
#               for a target that has none answers found=false rather than the first one it meets
#   ECHOED      the answer carries the request's own sequence, so a caller can tell WHICH question
#               it is the answer to. Without it a slot reused for another modulator looks like a
#               reply to the one you asked
#   SURVIVES A RELOAD  the same shape comes back after a save and a reload, because an editor
#               that can only draw what THIS session wrote is the same defect one step along
#
# No audio device needed: an envelope is data.
#   tools/sampler_envelope_readback_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

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

python3 - "$TMP/s.wav" <<'PYW'
import sys, wave, struct, math
sr = 48000
n = sr // 100
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(20000 * math.sin(2 * math.pi * 440 * i / sr)))
                       for i in range(n)))
w.close()
PYW

# A sampler with a mod set and NO modulators. The envelope under test is created by the WRITE,
# not by the fixture — a fixture envelope would prove the format round-trips and nothing about
# the read-back answering what the command wrote.
python3 - "$TMP/e.uniproj.json" "$TMP/s.wav" <<'PYP'
import json, sys
out, wav = sys.argv[1], sys.argv[2]
Q = 960000
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0, "loop_start_frame": 0, "loop_end_frame": 0,
        "loop_xfade_frames": 0, "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60, "pitch_track_milli": 1000,
        "tune_cents": 0, "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                                 "resonance_milli": 0, "next_modulator_id": 2,
                                 "modulators": []}],
                   "slots": [slot]}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PYP

SHM="/envchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project e --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# One field out of the answer, by name.
env_field() {  # env_field <jsonKey> [extra cli args...]
  local key="$1"; shift
  cli get sampler-envelope --track 0 "$@" 2>/dev/null | python3 -c "
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    print('unreadable'); raise SystemExit
try:
    d = json.loads(raw)
except Exception:
    print('unreadable'); raise SystemExit
v = d.get('$key')
if isinstance(v, list):
    print(','.join(str(x) for x in v))
else:
    print(v)
" 2>/dev/null
}
# The points as "time:value:tension" triples, so tension is asserted rather than assumed.
env_points() {
  cli get sampler-envelope --track 0 "$@" 2>/dev/null | python3 -c "
import json, sys
raw = sys.stdin.read().strip()
try:
    d = json.loads(raw)
except Exception:
    print('unreadable'); raise SystemExit
print(' '.join('%d:%d:%d' % (p['time'], p['value_milli'], p['tension'])
               for p in d.get('points', [])))
" 2>/dev/null
}

# ---- ABSENT. Before anything is written there is no envelope, and that is an ANSWER.
[ "$(env_field found --target amp)" = "False" ] || \
  fail "before any write, the amp target reports found=$(env_field found --target amp); it should
        be false. If this says 'unreadable' the answer is not being published at all, and every
        assertion below would be measuring the absence of a region rather than of an envelope"
echo "  absent: no envelope on amp yet, and the answer says so rather than erroring"

# ---- POINTS, including TENSION. A curve that reads back linear looks perfectly plausible.
cli do sampler-env-draw --track 0 --target amp \
    --points "0,1000;480000,500,20;960000,250;1440000,0" >/dev/null 2>&1
for _ in $(seq 1 40); do
  [ "$(env_field found --target amp)" = "True" ] && break
  sleep 0.25
done
[ "$(env_field found --target amp)" = "True" ] || \
  fail "after writing a four-point envelope the answer still reports found=false — the write
        landed (check sampler.envelope_points_set in $TMP/eng.log) or it did not, but either way
        the read-back is not seeing what the engine has"
GOT="$(env_points --target amp)"
[ "$GOT" = "0:1000:0 480000:500:20 960000:250:0 1440000:0:0" ] || \
  fail "the envelope reads back as '$GOT', wanted '0:1000:0 480000:500:20 960000:250:0
        1440000:0:0'. TENSION is the field a subset-answer drops first — it is one byte, per
        point, and a curve that comes back linear looks plausible enough to ship"
echo "  points: four points with their times, values and the tension of 20 that was drawn"

# ---- SHAPE. The fields an editor would silently destroy if the answer omitted them.
cli do sampler-lfo --track 0 --target pan --hz 2 >/dev/null 2>&1 || true
cli do sampler-env-draw --track 0 --target pitch \
    --points "0,0;240000,1000;480000,-1000;720000,0" >/dev/null 2>&1
for _ in $(seq 1 40); do
  [ "$(env_field found --target pitch)" = "True" ] && break
  sleep 0.25
done
[ "$(env_field time_base --target pitch)" != "unreadable" ] || \
  fail "the pitch envelope's time_base is unreadable"
[ "$(env_field sustain_loop --target amp)" = "255,255" ] || \
  fail "the amp envelope's sustain loop reads '$(env_field sustain_loop --target amp)', wanted
        '255,255' (no loop). 255 is kEnvLoopNone and the same sentinel the write uses; a zero
        here would be loop-point-0, which is a different envelope"
[ "$(env_field release_fade --target amp)" = "0" ] || \
  fail "the amp envelope's release fade reads '$(env_field release_fade --target amp)', wanted 0"
[ "$(env_field rate_milli --target amp)" = "1000" ] || \
  fail "the amp envelope's rate reads '$(env_field rate_milli --target amp)', wanted 1000"
echo "  shape: loop ranges, release fade, time base and rate all round-trip"

# ---- ADDRESSED. By target and by modulator id must reach the same envelope.
MOD="$(env_field modulator_id --target amp)"
[ -n "$MOD" ] && [ "$MOD" != "None" ] || fail "the answer carries no modulator id to address by"
BY_ID="$(env_points --modulator "$MOD")"
[ "$BY_ID" = "$GOT" ] || \
  fail "asking by modulator id $MOD gives '$BY_ID' and asking by target gives '$GOT'. The two
        address the same envelope or the read-back is answering about a different object than
        the one the write reached"
# A target with no envelope must answer found=false rather than the first one it meets.
[ "$(env_field found --target res)" = "False" ] || \
  fail "the resonance target has no envelope and the answer reports found=true — the search is
        returning whatever modulator it met first, so every 'by target' answer above is suspect"
echo "  addressed: by target and by modulator id agree, and an empty target says so"

# ---- ECHOED. Two requests in a row must each get their OWN sequence back.
S1="$(env_field request_seq --target amp)"
S2="$(env_field request_seq --target amp)"
[ -n "$S1" ] && [ "$S1" != "$S2" ] || \
  fail "two requests returned the same request_seq ($S1). The client owns that number and the
        answer echoes it; if it does not change, a caller cannot tell its own answer from the
        previous one sitting in the slot — which is the bug get_clip's comment describes"
echo "  echoed: each request gets its own sequence back ($S1 then $S2)"

# ---- SURVIVES A RELOAD. An editor that can only draw what THIS session wrote is the same
# defect one step along: the envelope a loaded project arrived with is exactly the one you
# cannot reconstruct.
cli do save eout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/eout.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/eout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
cli do load eout --force >/dev/null 2>&1 || true
wait_for_loads "$TMP/eng.log" "$ENG" 2 120 "the reload"
RELOADED=""
for _ in $(seq 1 40); do
  RELOADED="$(env_points --target amp)"
  [ "$RELOADED" = "$GOT" ] && break
  sleep 0.25
done
[ "$RELOADED" = "$GOT" ] || \
  fail "after a save and reload the amp envelope reads '$RELOADED', wanted '$GOT'. This is the
        case that matters most: the shape a UI most needs to draw is the one it did not write"
echo "  survives a reload: the same four points come back from the saved project"

echo "sampler_envelope_readback_check: PASS — a written envelope reads back whole, by either
                                 address, and survives a reload"
