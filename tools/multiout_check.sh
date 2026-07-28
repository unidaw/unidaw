#!/usr/bin/env bash
# Foundation check for multi-out instruments (Movement 4 Phase 5): a plugin's aux OUTPUT
# buses each reach the engine on their own channels of the aux plane. The fake fixture
# routes a note to output bus (pitch % (1+auxBuses)): pitch 60 -> main, 61 -> aux bus 0
# (aux channels 0/1), 62 -> aux bus 1 (aux channels 2/3). Playing ONE pitch at a time and
# reading which aux channels go active proves the stems are SEPARATED, not fanned to all
# buses. The engine logs multiout.aux_active per channel as it first sounds.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/multiout_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/mo_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

gen() {  # $1=out  $2=pitch
  python3 - "$1" "$2" "$Q" <<'PY'
import json,sys
out,pitch,Q=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]); DIRECT=4294967294
def route(k="none"): return {"kind":k,"track_id":0,"input_id":0}
def routing(): return {"midi_in":route(),"midi_out":route(),"audio_in":route(),"audio_out":route("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"multiout","path":"","uid16":""}}
clip={"id":1,"name":"n","length":4*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic",
      "notes":[{"nanotick":Q,"duration":Q,"pitch":pitch,"velocity":100,"column":0,"note_id":1}],"chords":[]}
pl={"clip_id":1,"at":0,"length":4*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"Drums","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
    "routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"mo","created_utc":0,"modified_utc":0},"nanoticks_per_quarter":Q,
           "tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY
}

run() {  # $1=name  -> prints the sorted active aux channels
  local name="$1"
  local log="$TMP/$name.log"
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 5 >"$log" 2>&1 ) &
  local e=$!; sleep 2
  DAW_UI_SHM_NAME="$SHM" "$CLI" do load "$name" --force >/dev/null 2>&1 || true; sleep 1
  DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
  wait "$e"
  grep -o '"aux_channel":[0-9]*' "$log" | grep -o '[0-9]*' | sort -n | uniq | paste -sd, -
}

gen "$TMP/p61.uniproj.json" 61
gen "$TMP/p62.uniproj.json" 62
CH61="$(run p61)"
CH62="$(run p62)"
echo "pitch 61 -> aux channels: [${CH61}]  (expect 0,1)"
echo "pitch 62 -> aux channels: [${CH62}]  (expect 2,3)"

rc=0
[ "$CH61" = "0,1" ] || { echo "FAIL: pitch 61 should light aux bus 0 (channels 0,1) only"; rc=1; }
[ "$CH62" = "2,3" ] || { echo "FAIL: pitch 62 should light aux bus 1 (channels 2,3) only"; rc=1; }
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "multiout_check: PASS — each stem reaches the engine on its own channels" \
  || { echo "multiout_check: FAIL"; exit 1; }
