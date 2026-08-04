#!/usr/bin/env bash
# Check AddTrack/RemoveTrack with STABLE ids (v22, Option B). Start from a 1-track project,
# append two tracks (ids 1, 2), remove the MIDDLE one (id 1), and save. The saved project
# must contain tracks 0 and 2 — proving (a) AddTrack created real tracks, (b) RemoveTrack
# dropped the right one, and (c) the id of the track AFTER the removed one did NOT
# renumber to 1: identity stayed put, which is the whole point of the tombstone model.
# Also confirms the engine stays up through add/remove/save (no crash).
#
# The MASTER track (patcher-is-a-device item 4a) is a real published track and is written
# into the saved `tracks` array as an is_master entry, so every count here is of the
# DOCUMENT tracks specifically — filtered on the master flag rather than assumed to be the
# whole list. Bumping the expected totals instead would have hidden which extra track had
# appeared, so master is asserted separately: exactly one, saved, flagged.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/add_remove_track_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/addrm_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/one.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
clip={"id":1,"name":"n","length":4*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic",
      "notes":[{"nanotick":Q,"duration":Q,"pitch":60,"velocity":100,"column":0,"note_id":1}],"chords":[]}
pl={"clip_id":1,"at":0,"length":4*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"T0","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"one"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY

( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 12 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
# NO TRAP AT ALL until now, so a check killed by a ctest timeout left its engine running and ctest
# blocked on the orphan. Two engines here, hence two names; ${..:-} because the trap is installed
# before the second one exists.
cleanup() {
  [ -n "${ENG:-}" ] && stop_engine "$ENG"
  [ -n "${ENG2:-}" ] && stop_engine "$ENG2"
  rm -rf "$TMP"
}
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
sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load one --force >/dev/null 2>&1 || true
sleep 1
# Count only the DOCUMENT tracks: master is published alongside them and would otherwise
# inflate every total by one.
doc_tracks() {
  # `"absent": false` as well as non-master: track_count is the id EXTENT, so a removed track
  # leaves a tombstone inside it that is published for its id and is not a track.
  { DAW_UI_SHM_NAME="$SHM" "$CLI" get tracks 2>/dev/null \
    | grep '"master": false' | grep -c '"absent": false'; } || true
}
master_strips() {
  DAW_UI_SHM_NAME="$SHM" "$CLI" get tracks 2>/dev/null | grep -c '"master": true' || true
}
before="$(doc_tracks)"
master_before="$(master_strips)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do add-track --force >/dev/null 2>&1 || true; sleep 0.4   # -> track 1
DAW_UI_SHM_NAME="$SHM" "$CLI" do add-track --force >/dev/null 2>&1 || true; sleep 0.4   # -> track 2
after_add="$(doc_tracks)"
master_after="$(master_strips)"
DAW_UI_SHM_NAME="$SHM" "$CLI" do remove-track --track 1 --force >/dev/null 2>&1 || true; sleep 0.4
# Save under a new name and read back the surviving track ids.
DAW_UI_SHM_NAME="$SHM" "$CLI" do save result --force >/dev/null 2>&1 || true
sleep 0.6
wait "$ENG"; ENG_RC=$?

# ---- AND IT MUST LOAD BACK. Everything above proved the FILE is right; nothing proved the
# engine could read it again, and it could not.
#
# Ids never renumber, so a project saved after a removal has SPARSE ids — here [0,2]. The load
# stored `document.tracks.size()` as the live track count, which is 2, and every publisher
# clamps to it while the save skips `trackId >= liveTrackCount`. So track 2 was adopted and
# loaded correctly and then hidden from the UI and dropped by the next save, while the
# unclaimed slot 1 came back as an editable empty lane the same save wrote out as a real
# track. One track destroyed, one invented, nothing reported.
#
# The frontend found this from the UI ("a track disappears on load") and it read as a rename
# failure for days. No fixture caught it because every fixture has dense ids from zero —
# fixtures are authored, not edited, and this needs a REMOVAL followed by a SAVE.
#
# A FRESH ENGINE is the point: the same process still holds the tracks from the edits above.
SHM2="/addrm2_check_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 18 >"$TMP/eng2.log" 2>&1 ) &
ENG2=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
DAW_UI_SHM_NAME="$SHM2" "$CLI" do load result --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng2.log" "$ENG2" 80
# The MASTER always publishes and always has a track_id, so grepping for one is satisfied
# before a single real track has come back — which is how this read an empty list and blamed the
# loader. Wait for a NON-master, NON-absent track: the thing the assertion below is about.
tracks_ready() { DAW_UI_SHM_NAME="$SHM2" "$CLI" get tracks 2>/dev/null \
  | grep '"master": false' | grep -q '"absent": false'; }
wait_until 20 tracks_ready || true
# The ids that came back LIVE (a tombstone publishes with absent:true and is not a track).
# `|| true` on the whole pipeline: if the engine is not answering yet the first grep matches
# nothing, exits 1, and under `set -o pipefail` inside a command substitution that kills the
# script before it prints anything — a silent death that looks like a crash. Fourth time this
# exact shape has bitten in one night; see the note in all_checks.sh.
RELOADED="$({ DAW_UI_SHM_NAME="$SHM2" "$CLI" get tracks 2>/dev/null \
  | grep '"master": false' | grep '"absent": false' \
  | sed -n 's/.*"track_id": \([0-9]*\).*/\1/p' | paste -sd, -; } || true)"
TOMBSTONES="$(DAW_UI_SHM_NAME="$SHM2" "$CLI" get tracks 2>/dev/null \
  | grep -c '"absent": true' || true)"
# Save again: a load that hid a track would let this save delete it from disk for good.
DAW_UI_SHM_NAME="$SHM2" "$CLI" do save result2 --force >/dev/null 2>&1 || true
sleep 1.6
kill "$ENG2" 2>/dev/null || true; wait "$ENG2" 2>/dev/null || true
IDS2="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(",".join(str(t["track_id"]) for t in d.get("tracks",[]) if not t.get("is_master")))' "$TMP/result2.uniproj.json" 2>/dev/null || echo ERR)"
echo "reloaded live ids       : [$RELOADED] (expect [0,2]) with $TOMBSTONES tombstone(s) (expect 1)"
echo "re-saved track ids      : [$IDS2] (expect [0,2] — a second save must not lose one)"

IDS="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(",".join(str(t["track_id"]) for t in d.get("tracks",[]) if not t.get("is_master")))' "$TMP/result.uniproj.json" 2>/dev/null || echo ERR)"
SAVED_MASTERS="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(sum(1 for t in d.get("tracks",[]) if t.get("is_master")))' "$TMP/result.uniproj.json" 2>/dev/null || echo ERR)"
echo "doc tracks before add   : $before (expect 1)"
echo "doc tracks after 2 adds : $after_add (expect 3)"
echo "master strips           : $master_before before / $master_after after (expect 1/1)"
echo "saved is_master entries : $SAVED_MASTERS (expect 1)"
echo "AddTrack log            : $(grep -c 'AddTrack ->' "$TMP/eng.log") lines (expect 2)"
echo "RemoveTrack log         : $(grep 'RemoveTrack ' "$TMP/eng.log" | head -1)"
echo "saved track ids         : [$IDS] (expect [0,2] — id 2 did NOT renumber to 1)"
echo "engine exit code        : $ENG_RC (expect 0)"

rm -rf "$TMP"
ok=1
[ "$before" = "1" ] || { echo "FAIL: expected 1 document track before add"; ok=0; }
[ "$after_add" = "3" ] || { echo "FAIL: expected 3 document tracks after two adds"; ok=0; }
[ "$IDS" = "0,2" ] || { echo "FAIL: saved ids not [0,2] — id stability broken"; ok=0; }
# Master is exactly one strip, always — never zero (it stopped being published) and never
# duplicated by an add (which would give the mix two faders and one of them would do
# nothing).
[ "$master_before" = "1" ] && [ "$master_after" = "1" ] \
  || { echo "FAIL: master strips $master_before -> $master_after, expected 1 -> 1"; ok=0; }
[ "$RELOADED" = "0,2" ] \
  || { echo "FAIL: loading the saved project back gave live ids [$RELOADED], expected [0,2].
        A project saved after a track was removed has sparse ids, and a load that treats the
        track COUNT as the id EXTENT hides the highest track and publishes the unclaimed slot
        as a real one"; ok=0; }
[ "$TOMBSTONES" = "1" ] \
  || { echo "FAIL: $TOMBSTONES tombstone(s) after the reload, expected 1 — the removed id must
        come back as a hole the reader skips, not as an editable empty lane"; ok=0; }
[ "$IDS2" = "0,2" ] \
  || { echo "FAIL: load -> save gave ids [$IDS2], expected [0,2]. The reload hid a track and
        this save deleted it from disk"; ok=0; }
[ "$SAVED_MASTERS" = "1" ] \
  || { echo "FAIL: the save wrote $SAVED_MASTERS is_master entries, expected exactly 1 —
        0 loses the master chain on reload, 2+ means the load will fight over which wins"; ok=0; }
[ "$ENG_RC" = "0" ] || { echo "FAIL: engine did not exit cleanly"; ok=0; }
[ "$ok" = "1" ] && echo "add_remove_track_check: PASS — add/remove work and ids stay stable across a middle removal" \
                || { echo "add_remove_track_check: FAIL"; exit 1; }
