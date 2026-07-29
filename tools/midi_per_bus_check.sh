#!/usr/bin/env bash
# End-to-end check for multitimbral MIDI-per-bus (Movement 4 Phase 5). A multi-out fake
# instrument is on track 0 with NO notes of its own; the engine auto-creates a child per
# aux output bus. A note entered on CHILD track 1 is rendered into the parent's host on
# the child's bus MIDI channel (1); the multitimbral fake routes channel 1 to output bus
# 1, whose stem is exactly child 1's audio. So the master is audible ONLY IF the child's
# note steered through the parent on its bus channel — the parent plays nothing on its
# own. That is the proof MIDI-per-bus routes correctly.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/midi_per_bus_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/mpb_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/mo.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"multiout","path":"","uid16":""}}
tr={"track_id":0,"name":"Drums","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[]}
json.dump({"schema_version":4,"meta":{"name":"mo"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[],"tracks":[tr]},open(out,"w"))
PY

LOG="$TMP/eng.log"
( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_CAPTURE_WAV="$TMP/m.wav" DAW_CAPTURE_SECONDS=6 ./daw_engine --run-seconds 7 >"$LOG" 2>&1 ) &
ENG=$!; sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load mo --force >/dev/null 2>&1 || true
sleep 2   # let the children get derived from the bus layout
CHILDREN=$(grep -c "multiout.child_created" "$LOG" || true)
# Note on CHILD track 1 -> renders on MIDI channel 1 -> parent bus 1 -> child 1 audio.
DAW_UI_SHM_NAME="$SHM" "$CLI" do note --force --track 1 --nanotick $((2*Q)) --pitch 60 --duration "$Q" >/dev/null 2>&1 || true
sleep 0.5
# Did the edit actually LAND on the child? Asserted separately from the audio, because a
# refused edit and a mis-steered note both show up downstream as "the master is silent"
# and they need completely different fixes. This caught the real one: creating a child did
# not bump the version-gated regions, so the child's published base stayed at the global
# value while its acceptance counter sat at 0 — every note on a stem was refused as stale,
# forever, and the sender was told it had succeeded.
CHILD_NOTES=$(DAW_UI_SHM_NAME="$SHM" "$CLI" get notes --track 1 2>/dev/null \
  | grep -oE '"note_count": [0-9]+' | grep -oE '[0-9]+' || echo 0)
REFUSALS=$(grep -c 'clip.version_mismatch\|clip.unknown_track' "$LOG" || true)
DAW_UI_SHM_NAME="$SHM" "$CLI" do play --force >/dev/null 2>&1 || true
wait "$ENG"

PEAK=$(python3 - "$TMP/m.wav" <<'PY'
import sys,wave,struct
w=wave.open(sys.argv[1],'rb');ch=w.getnchannels();n=w.getnframes();s=struct.unpack('<'+'h'*(n*ch),w.readframes(n));w.close()
print(f"{max((abs(v) for v in s),default=0)/32768.0:.3f}")
PY
)
echo "children: $CHILDREN (expect 2)"
echo "notes accepted on child 1: $CHILD_NOTES (expect 1), engine refusals: $REFUSALS (expect 0)"
echo "master peak with a note on child 1 only: $PEAK (expect >0.3)"
# `|| true`: with no audio there are no aux_channel lines, grep exits 1, and under
# `set -o pipefail` the script died right here — before printing a single FAIL line. The
# check did still fail, but silently, which is the least useful way to fail: the run that
# found the child-track bug showed one peak line and nothing else.
{ grep -o '"aux_channel":[0-9]*' "$LOG" | sort -u | paste -sd, - \
    | sed 's/^/aux channels active: /'; } || true

rc=0
[ "$CHILDREN" = "2" ] || { echo "FAIL: expected 2 children, got $CHILDREN"; rc=1; }
[ "$CHILD_NOTES" = "1" ] || { echo "FAIL: the note did not land on child 1 (count $CHILD_NOTES)
        — the edit was refused or went to the wrong track, before any routing happened"; rc=1; }
[ "${REFUSALS:-0}" = "0" ] || { echo "FAIL: the engine refused $REFUSALS edit(s) — a child
        track's published clip version disagrees with the version it accepts against"; rc=1; }
python3 -c "import sys; sys.exit(0 if float('$PEAK')>0.3 else 1)" \
  || { echo "FAIL: master silent — the child's note did not route through the parent on its bus channel"; rc=1; }
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "midi_per_bus_check: PASS — a child's note steers to the parent's plugin on its bus channel" \
  || { echo "midi_per_bus_check: FAIL"; exit 1; }
