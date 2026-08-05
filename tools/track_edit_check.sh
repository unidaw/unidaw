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
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT

# FIVE FIXED SLEEPS USED TO STAND HERE, and under a full parallel ctest the first one lost. On
# 2026-08-05 this check reported "rename not persisted" and "removed track's notes still
# published" while every read came back -1 — including B1_BEFORE, taken BEFORE any edit. A
# baseline that cannot be read is not a failed assertion, it is a check that started measuring
# before the engine existed, and it accuses the product in the engine's own words.
#
# `sleep 2` is a claim about how fast this machine boots an engine while a hundred other tests
# run. wait_for_boot asks the engine instead, and fails immediately with the log tail if it died.
cli() { DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
# THE PATTERN MATTERS: this engine is launched with NO --project, so it never emits a
# project.load at boot and wait_for_boot's DEFAULT condition would wait for something that never
# arrives — which is exactly what it did on the first attempt, burning the engine's whole
# 14-second life and then reporting "the engine EXITED before its project loaded". True, and
# about the harness. What it needs is the command thread, which is what makes `do load` land.
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started"

# ONE load, not two: the boot did not perform one, so the explicit load below is the first.
cli do load three --force >/dev/null 2>&1 || true
wait_for_loads "$TMP/eng.log" "$ENG" 1 60 "the explicit load of the three-track fixture"

notes_on() { cli get clip --track "$1" --force 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d.get("notes",[])))' 2>/dev/null || echo -1; }

# THE READ PATH HAS TO BE WORKING BEFORE ANY ASSERTION RESTS ON IT. Every number below is a note
# count, and -1 means the read FAILED rather than "no notes" — indistinguishable in the
# assertions, and only one of them is about the product. project.load says the document was
# adopted; it does not say the clip window has been published yet.
#
# THESE ARE FUNCTIONS, NOT STRINGS, and that is not style. The first version passed
# `sh -c "[ \"$(...)\" != -1 ]"` — where bash expands the $(...) ONCE, while parsing the line, so
# every iteration re-tested a value frozen at that instant. A wait that does not re-evaluate is a
# sleep wearing a wait's name, and it failed for that reason rather than for anything it measured.
clip_readable() { [ "$(notes_on 0)" != "-1" ]; }
wait_until 30 clip_readable || {
  echo "  FAIL(setup): the published clip window never became readable, so nothing below would"
  echo "        be a statement about the engine's edits."
  tail -8 "$TMP/eng.log" | sed 's/^/          /'; exit 1; }

# --- Bug A: rename track 0, save, read the saved name ---
cli do rename --track 0 --name Drums --force >/dev/null 2>&1 || true
cli do save saved --force >/dev/null 2>&1 || true
# The SAVE is a file appearing, so wait for the file rather than for a duration.
wait_until 30 test -s "$TMP/saved.uniproj.json"
SAVED_NAME="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(next((t["name"] for t in d["tracks"] if t["track_id"]==0), "?"))' "$TMP/saved.uniproj.json" 2>/dev/null || echo ERR)"

# --- Bug B: remove the MIDDLE track (1), its published clip must go empty ---
B1_BEFORE="$(notes_on 1)"; T0_BEFORE="$(notes_on 0)"; T2_BEFORE="$(notes_on 2)"
cli do remove-track --track 1 --force >/dev/null 2>&1 || true
# Wait for the OBSERVABLE the assertion is about — the removed track's window going empty — not
# for half a second. If it never empties the assertion below still fails and says so; this only
# removes the race, it does not replace the check.
removed_track_empty() { [ "$(notes_on 1)" = "0" ]; }
wait_until 30 removed_track_empty || true
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
