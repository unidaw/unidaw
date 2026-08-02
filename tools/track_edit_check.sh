#!/usr/bin/env bash
# Two backend bugs the frontend found with evidence:
#  A. SetTrackName reached the UI mirror but NOT the saved project — save hardcoded
#     "Track N+1". Rename -> save -> reload lost the name. Here: rename, save, read the
#     saved file's name back.
#  B. RemoveTrack cleared the runtime but did not republish, so the removed track's notes
#     lingered in the published flat clip. Here: three tracks with notes, remove the MIDDLE
#     one, and read its published clip window back — it must be empty (and its neighbours
#     untouched).
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/track_edit_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/track_edit_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

# Three tracks; notes 2 / 2 / 1 (track 1 is the one we remove).
python3 - "$TMP/three.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
def track(tid,n):
    notes=[{"nanotick":i*Q,"duration":Q//4,"pitch":60+i,"velocity":100,"column":0,"note_id":tid*10+i+1} for i in range(n)]
    clip={"id":tid+1,"name":"c","length":4*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":notes,"chords":[]}
    pl={"clip_id":tid+1,"at":0,"length":4*Q,"notes":[],"chords":[],"mutes":[]}
    return clip,{"track_id":tid,"name":f"Track {tid+1}","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
clips=[];tracks=[]
for tid,n in [(0,2),(1,2),(2,1)]:
    c,t=track(tid,n); clips.append(c); tracks.append(t)
json.dump({"schema_version":4,"meta":{"name":"three"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":clips,"tracks":tracks},open(out,"w"))
PY

( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 14 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. Without this the
# engine was orphaned and ctest then blocked on it — ~1000s per timeout, measured across 18 runs.
# stop_engine escalates to SIGKILL after 10s and says so.
# There was NO EXIT TRAP here at all, so a timed-out check was guaranteed to orphan.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load three --force >/dev/null 2>&1 || true
sleep 1

notes_on() { DAW_UI_SHM_NAME="$SHM" "$CLI" get clip --track "$1" --force 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d.get("notes",[])))' 2>/dev/null || echo -1; }

# --- Bug A: rename track 0, save, read the saved name ---
DAW_UI_SHM_NAME="$SHM" "$CLI" do rename --track 0 --name Drums --force >/dev/null 2>&1 || true
sleep 0.3
DAW_UI_SHM_NAME="$SHM" "$CLI" do save saved --force >/dev/null 2>&1 || true
sleep 0.5
SAVED_NAME="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(next((t["name"] for t in d["tracks"] if t["track_id"]==0), "?"))' "$TMP/saved.uniproj.json" 2>/dev/null || echo ERR)"

# --- Bug B: remove the MIDDLE track (1), its published clip must go empty ---
B1_BEFORE="$(notes_on 1)"; T0_BEFORE="$(notes_on 0)"; T2_BEFORE="$(notes_on 2)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do remove-track --track 1 --force >/dev/null 2>&1 || true
sleep 0.5
B1_AFTER="$(notes_on 1)"; T0_AFTER="$(notes_on 0)"; T2_AFTER="$(notes_on 2)"
wait "$ENG" 2>/dev/null || true

echo "Bug A  saved track-0 name : $SAVED_NAME (expect Drums)"
echo "Bug B  track1 notes       : before=$B1_BEFORE after=$B1_AFTER (expect 2 -> 0)"
echo "Bug B  neighbours intact  : track0 $T0_BEFORE->$T0_AFTER, track2 $T2_BEFORE->$T2_AFTER (expect unchanged)"

rm -rf "$TMP"
ok=1
[ "$SAVED_NAME" = "Drums" ] || { echo "FAIL(A): rename not persisted"; ok=0; }
[ "$B1_BEFORE" = "2" ] && [ "$B1_AFTER" = "0" ] || { echo "FAIL(B): removed track's notes still published"; ok=0; }
[ "$T0_AFTER" = "2" ] && [ "$T2_AFTER" = "1" ] || { echo "FAIL(B): a neighbour track was disturbed"; ok=0; }
[ "$ok" = "1" ] && echo "track_edit_check: PASS — rename persists to disk; RemoveTrack clears the published clip" \
                || { echo "track_edit_check: FAIL"; exit 1; }
