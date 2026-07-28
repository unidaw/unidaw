#!/usr/bin/env bash
# End-to-end check for multi-out CHILD TRACKS (Movement 4 Phase 5b). A multi-out fake
# instrument routes pitch 61 -> aux bus 0 and pitch 62 -> aux bus 1 (NOTHING on the main
# bus). The engine auto-creates a child track per aux bus; each child is an ordinary
# track whose audio is a view into the parent's aux-plane slice, mixed to master. Because
# the pitches only ever hit aux buses, the master is audible ONLY IF the children route
# their stems to it — the aux plane itself never reaches the master mix. So a non-silent
# master + two child_created events is the proof.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/child_track_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/child_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/mo.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def route(k="none"): return {"kind":k,"track_id":0,"input_id":0}
def routing(): return {"midi_in":route(),"midi_out":route(),"audio_in":route(),"audio_out":route("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"multiout","path":"","uid16":""}}
def note(nid,p): return {"nanotick":2*Q,"duration":Q,"pitch":p,"velocity":100,"column":0,"note_id":nid}
clip={"id":1,"name":"n","length":4*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":[note(1,61),note(2,62)],"chords":[]}
pl={"clip_id":1,"at":0,"length":4*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"Drums","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"mo","created_utc":0,"modified_utc":0},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY

LOG="$TMP/eng.log"
( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_CAPTURE_WAV="$TMP/m.wav" DAW_CAPTURE_SECONDS=5 ./daw_engine --run-seconds 5 >"$LOG" 2>&1 ) &
ENG=$!; sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load mo --force >/dev/null 2>&1 || true; sleep 1
DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
wait "$ENG"

CHILDREN=$(grep -c "multiout.child_created" "$LOG" || true)
echo "children created: $CHILDREN (expect 2)"
grep -o '"bus":[0-9]*,"plane_offset":[0-9]*' "$LOG" | sort -u | sed 's/^/  /'
PEAK=$(python3 - "$TMP/m.wav" <<'PY'
import sys,wave,struct
w=wave.open(sys.argv[1],'rb');ch=w.getnchannels();n=w.getnframes();s=struct.unpack('<'+'h'*(n*ch),w.readframes(n));w.close()
print(f"{max((abs(v) for v in s),default=0)/32768.0:.3f}")
PY
)
echo "master peak (aux-only pitches, audible only via children): $PEAK"

rc=0
[ "$CHILDREN" = "2" ] || { echo "FAIL: expected 2 child tracks, got $CHILDREN"; rc=1; }
python3 -c "import sys; sys.exit(0 if float('$PEAK')>0.3 else 1)" \
  || { echo "FAIL: master silent — aux stems did not reach master through the children"; rc=1; }
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "child_track_check: PASS — aux stems reach master via auto-created child tracks" \
  || { echo "child_track_check: FAIL"; exit 1; }
