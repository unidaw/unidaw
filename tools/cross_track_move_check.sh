#!/usr/bin/env bash
# Check cross-track MovePlacement (lane drag): a placement + its clip move to another lane
# with its stable id intact, and the move is ONE atomic undo (both tracks restored together,
# never a state where the clip belongs to neither).
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/cross_track_move_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
BAR=$((4 * Q))
TMP="$(mktemp -d)"
SHM="/xtrack_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/p.uniproj.json" "$Q" "$BAR" <<'PY'
import json,sys
out,Q,BAR=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]);DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev():return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
clip={"id":1,"name":"c1","length":BAR,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":[{"nanotick":0,"duration":Q//2,"pitch":60,"velocity":100,"column":0,"note_id":1}],"chords":[]}
t0={"track_id":0,"name":"A","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[{"clip_id":1,"at":0,"length":BAR,"notes":[],"chords":[],"mutes":[]}]}
t1={"track_id":1,"name":"B","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[]}
json.dump({"schema_version":4,"meta":{"name":"p"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[t0,t1]},open(sys.argv[1],"w"))
PY

( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 14 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load p --force >/dev/null 2>&1 || true
sleep 1
cat > "$TMP/w.py" <<'PY'
import sys,re,json
d=json.loads(re.sub(r",(\s*])",r"\1",sys.stdin.read() or "[]"))
print(" ".join("p%d@t%d" % (e["placement"], e["track"]) for e in d) or "none")
PY
where() { DAW_UI_SHM_NAME="$SHM" "$CLI" get extents 2>/dev/null | python3 "$TMP/w.py"; }

BEFORE="$(where)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do move-placement --track 0 --placement 1 --at $((2*BAR)) --to-track 1 --force >/dev/null 2>&1 || true
sleep 0.4
MOVED="$(where)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do undo --force >/dev/null 2>&1 || true
sleep 0.4
UNDONE="$(where)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do redo --force >/dev/null 2>&1 || true
sleep 0.4
REDONE="$(where)"
wait "$ENG" 2>/dev/null || true

echo "before : $BEFORE (expect p1@t0)"
echo "moved  : $MOVED  (expect p1@t1 — same id, other lane)"
echo "undone : $UNDONE (expect p1@t0 — atomic restore of both tracks)"
echo "redone : $REDONE (expect p1@t1)"

rm -rf "$TMP"
ok=1
[ "$BEFORE" = "p1@t0" ] || { echo "FAIL: setup"; ok=0; }
[ "$MOVED" = "p1@t1" ]  || { echo "FAIL: cross-track move (id must stay 1, land on track 1)"; ok=0; }
[ "$UNDONE" = "p1@t0" ] || { echo "FAIL: undo not atomic (clip did not return to track 0)"; ok=0; }
[ "$REDONE" = "p1@t1" ] || { echo "FAIL: redo"; ok=0; }
[ "$ok" = "1" ] && echo "cross_track_move_check: PASS — lane drag keeps the id and undo/redo are atomic" \
                || { echo "cross_track_move_check: FAIL"; exit 1; }
