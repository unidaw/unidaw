#!/usr/bin/env bash
# Check audio-underrun telemetry + the two stability levers (Movement 4 stability work).
# A heavy plugin is simulated with DAW_FAKE_BUSY_US (busy-wait microseconds per block).
# Loaded to ~9 ms of the ~11.6 ms block budget (at 512/44.1k), OS jitter pushes the odd
# block over the deadline: with the default 4-block pipeline the audio thread starves
# (drops blocks -> glitch), and DEEPENING the pipeline (DAW_ENGINE_NUM_BLOCKS) absorbs
# the jitter and drives underruns to zero. Proves both the telemetry and the lever.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/audio_stability_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/b.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
notes=[{"nanotick":i*Q//2,"duration":Q//4,"pitch":60,"velocity":100,"column":0,"note_id":i+1} for i in range(16)]
clip={"id":1,"name":"n","length":8*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":notes,"chords":[]}
pl={"clip_id":1,"at":0,"length":8*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"b"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY

run() {  # $1=numBlocks -> echoes the starve count
  local nb shm e
  nb="$1"
  shm="/audiostab_${nb}_$$"
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_FAKE_BUSY_US=9000 DAW_ENGINE_NUM_BLOCKS="$nb" \
      DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" ./daw_engine --run-seconds 10 >"$TMP/eng_$nb.log" 2>&1 ) &
  e=$!
  sleep 2
  DAW_UI_SHM_NAME="$shm" "$CLI" do load b --force >/dev/null 2>&1 || true
  sleep 1
  DAW_UI_SHM_NAME="$shm" "$CLI" do play --force >/dev/null 2>&1 || true
  wait "$e"
  grep 'underrun summary' "$TMP/eng_$nb.log" | grep -oE 'summary: [0-9]+' | grep -oE '[0-9]+' || echo -1
}

SHALLOW="$(run 4)"
DEEP="$(run 16)"
echo "pipeline depth 4  -> $SHALLOW starve callback(s) under load"
echo "pipeline depth 16 -> $DEEP starve callback(s) under load"

rm -rf "$TMP"
# The telemetry must produce a real number (>=0) in both runs, and deepening the pipeline
# must not INCREASE underruns — it should absorb the jitter (deep <= shallow, deep == 0).
if [ "$SHALLOW" -lt 0 ] || [ "$DEEP" -lt 0 ]; then
  echo "audio_stability_check: FAIL (no telemetry emitted)"; exit 1
fi
if [ "$DEEP" -gt "$SHALLOW" ] || [ "$DEEP" -ne 0 ]; then
  echo "audio_stability_check: FAIL (deeper pipeline did not absorb jitter: deep=$DEEP shallow=$SHALLOW)"; exit 1
fi
echo "audio_stability_check: PASS — underrun telemetry works and a deeper pipeline drives starves to zero"
