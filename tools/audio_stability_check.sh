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
  ( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_FAKE_BUSY_US=9000 DAW_ENGINE_NUM_BLOCKS="$nb" \
      DAW_UI_SHM_NAME="$shm" DAW_PROJECT_DIR="$TMP" ./daw_engine --run-seconds 10 >"$TMP/eng_$nb.log" 2>&1 ) &
  e=$!
  sleep 2
  # `|| true` HERE IS THE SAME BUG AS THE MISSING POSITIVE CONTROL, one layer down. A refused
  # `do load` or `do play` — a stale daw-cli that the version gate rejects is the documented way
  # this happens — leaves the engine sitting there STOPPED, rendering nothing, reporting zero
  # starves. That is indistinguishable from "the lever worked", and the swallowed exit code is
  # what made it indistinguishable. A refusal is now recorded and reported as a harness failure.
  DAW_UI_SHM_NAME="$shm" "$CLI" do load b --force >"$TMP/cli_$nb.out" 2>&1 \
    || touch "$TMP/refused_$nb"
  sleep 1
  DAW_UI_SHM_NAME="$shm" "$CLI" do play --force >>"$TMP/cli_$nb.out" 2>&1 \
    || touch "$TMP/refused_$nb"
  wait "$e"
  grep 'underrun summary' "$TMP/eng_$nb.log" | grep -oE 'summary: [0-9]+' | grep -oE '[0-9]+' || echo -1
}

SHALLOW="$(run 4)"
DEEP="$(run 16)"
echo "pipeline depth 4  -> $SHALLOW starve callback(s) under load"
echo "pipeline depth 16 -> $DEEP starve callback(s) under load"

if [ -e "$TMP/refused_4" ] || [ -e "$TMP/refused_16" ]; then
  echo "audio_stability_check: FAIL — daw-cli was refused, so an engine never started playing"
  for nb in 4 16; do
    [ -e "$TMP/refused_$nb" ] && echo "  depth $nb: $(head -3 "$TMP/cli_$nb.out" 2>/dev/null)"
  done
  echo "  A refused command leaves the engine STOPPED and rendering nothing, which reports zero"
  echo "  starves — the same reading a perfectly absorbed load gives. This is a harness failure,"
  echo "  not a result about the pipeline."
  rm -rf "$TMP"
  exit 1
fi

rm -rf "$TMP"
# The telemetry must produce a real number (>=0) in both runs, and deepening the pipeline
# must not INCREASE underruns — it should absorb the jitter (deep <= shallow, deep == 0).
if [ "$SHALLOW" -lt 0 ] || [ "$DEEP" -lt 0 ]; then
  echo "audio_stability_check: FAIL (no telemetry emitted)"; exit 1
fi

# THE MISSING POSITIVE CONTROL. Everything below compares DEEP against SHALLOW, and every one of
# those comparisons is satisfied by SHALLOW being zero — which is not the lever working, it is the
# load never biting. Nothing here ever asserted that it bit.
#
# The 9000us above is MARGINAL on this machine, measured 2026-08-04 across repeated runs: depth 4
# reports 0 starves on some runs and 1-6 on others. On the zero runs depth 16 is also 0, and this
# check printed PASS — two zeroes, the one result that proves nothing at all. So it was not
# permanently vacuous, it was vacuous at random, which is worse: the same command alternates
# between a real result and a meaningless one and reports both identically.
#
# SKIP RATHER THAN FAIL, and rather than silently passing. The load is calibrated in absolute
# microseconds against a block budget that depends on the device's block size and rate, so a
# machine fast enough not to starve at 9000us is a legitimate configuration and not a defect. What
# is NOT legitimate is reporting that as a verified lever. ctest is told SKIP_RETURN_CODE 77.
if [ "$SHALLOW" -eq 0 ]; then
  echo "audio_stability_check: SKIP — the load never bit"
  echo "  depth 4 reported 0 starves, so it was never under stress and 'depth 16 also reported"
  echo "  $DEEP' compares two idle runs. Every assertion this check makes is satisfied by that,"
  echo "  which is why it read PASS while measuring nothing."
  echo
  echo "  DAW_FAKE_BUSY_US=9000 is calibrated against a ~11.6 ms block budget (512 at 44.1k). If"
  echo "  this machine's device gives a bigger budget, or is simply faster, 9 ms of busy-wait no"
  echo "  longer reaches the deadline. Raise it until depth 4 starves, then this check can answer."
  echo
  echo "  Do NOT raise it blindly. At 9000us the lever does hold when the load bites (depth 4"
  echo "  starving 1-6 times, depth 16 zero), but at 11000-12000us depth 16 measured 0, 454 and"
  echo "  528 across three interleaved rounds while depth 4 stayed at 6, 1 and 4 — so a heavier"
  echo "  load does not simply give a cleaner version of this result. See the task filed"
  echo "  2026-08-04 with the numbers before changing this constant."
  exit 77
fi

if [ "$DEEP" -gt "$SHALLOW" ] || [ "$DEEP" -ne 0 ]; then
  echo "audio_stability_check: FAIL (deeper pipeline did not absorb jitter: deep=$DEEP shallow=$SHALLOW)"; exit 1
fi
echo "audio_stability_check: PASS — underrun telemetry works and a deeper pipeline drives starves to zero"
echo "                       (depth 4 starved $SHALLOW time(s), so the load did bite)"
