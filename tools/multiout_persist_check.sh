#!/usr/bin/env bash
# Checks that what you author on a MULTI-OUT STEM survives a save (Movement 4 phase 5).
#
# A child track is DERIVED: the engine asks a multi-out plugin for its bus layout and
# creates one lane per aux output bus. midi_per_bus_check already proves you can type a
# note on a stem and hear it steered to the parent's plugin on that bus's MIDI channel —
# so authoring on a stem is a supported feature, not an accident.
#
# But the save skipped every aux child, with a comment explaining why: a child written as a
# plain track would reload as a phantom top-level lane fed by nothing. The consequence went
# unnoticed, because no check saved a project after touching a stem — the notes were
# accepted, they sounded, and they were gone after a reload, with nothing reporting a loss.
# That is the same shape as the mod links that were parsed and never installed, and the
# automation that could be heard and never saved.
#
# A child is now persisted as a FLAGGED entry (`is_aux_child` + `aux_bus_index`) and lifted
# back out at load, exactly as the master track is — so it cannot reload as a top-level
# lane, and it reattaches to the bus it was authored against rather than to a slot index
# that a different plugin or a changed bus layout would renumber.
#
# THE RELOAD MUST BE A FRESH ENGINE. A same-process reload passes even with the install
# deleted, because the runtime still holds the notes from the writes above — that is how the
# automation reload test came to prove nothing.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/multiout_persist_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
TMP="$(mktemp -d)"
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. This trap removed
# $TMP and left the engine running, so a timed-out check orphaned it and ctest then blocked on the
# orphan — ~1000s per timeout, measured across 18 runs. override was the demonstrated case: 909.87s
# against a TIMEOUT of 600 while passing standalone in 23.2s.
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

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

# TWO document tracks, and the second one is what makes this test able to fail.
#
# With a single parent the first stem is derived at track id 1 and comes from bus 1, the
# second at id 2 from bus 2 — the ids and the bus indices COINCIDE. Every assertion below
# then passes whether the engine reattaches by bus or by saved track id, which a negative
# control proved: keying the overlay by trackId left the whole check green. A filler track
# at id 1 pushes the stems to ids 2 and 3 while their buses stay 1 and 2, so the two
# keyings disagree and the wrong one is caught.
python3 - "$TMP/mo.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,
                   "host_slot_index":DIRECT,"bypass":False,
                   "vst_ref":{"vendor":"","name":"multiout","path":"","uid16":""}}
def track(tid,name,chain):
    return {"track_id":tid,"name":name,"harmony_quantize":False,"lines_per_beat":4,
            "mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
            "routing":routing(),"device_chain":chain,"mod_links":[],"placements":[]}
json.dump({"schema_version":4,"meta":{"name":"mo"},"nanoticks_per_quarter":Q,
           "tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[],
           "tracks":[track(0,"Drums",[dev()]), track(1,"Pad",[])]},open(out,"w"))
PY

ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}
# Count notes of a pitch on a track.
count_pitch() {
  DAW_UI_SHM_NAME="$1" "$CLI" get notes --track "$2" 2>/dev/null \
    | grep -c "\"pitch\": $3," || true
}
# Wait for a condition rather than sleeping a guessed amount: a fixed sleep that reads 0
# looks exactly like the feature being broken, which is the worst way for this to fail.
wait_for() {  # wait_for <shm> <track> <pitch> <want>
  for _ in $(seq 1 40); do
    [ "$(count_pitch "$1" "$2" "$3")" = "$4" ] && return 0
    sleep 0.25
  done
  return 1
}

# ---- AUTHOR: a note on each stem, then save. With the filler track at id 1, the stems
# are derived at ids 2 and 3 while their BUSES are 1 and 2 — so ids and buses differ and an
# engine keying on the wrong one is caught rather than accidentally right.
SHM="/mop1_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/eng1.log" 2>&1 ) &
ENG=$!
# Wait for the engine to be READY rather than sleeping a guessed amount. The pattern is
# "UI: command thread started" because this engine boots with no project, so wait_for_boot's
# default (a project.load) would never appear; that thread reads the command ring, so it is
# the marker that means "ready to be told something".
wait_for_boot "$TMP/eng1.log" "$ENG" 80 "UI: command thread started"
DAW_UI_SHM_NAME="$SHM" "$CLI" do load mo --force >/dev/null 2>&1 || true
# The children are derived from the bus layout once the load settles, so wait for the FIRST
# one to be announced rather than for two seconds. The count below still decides the verdict;
# this only stops us reading it before the engine has said anything.
wait_for_event "$TMP/eng1.log" "multiout.child_created" 80 "an aux child" >/dev/null 2>&1 || true

CHILDREN=$(grep -c "multiout.child_created" "$TMP/eng1.log" || true)
[ "$CHILDREN" = "2" ] || fail "expected 2 derived children, got $CHILDREN — nothing to author on"

DAW_UI_SHM_NAME="$SHM" "$CLI" do note --force --track 2 --nanotick $((2 * Q)) \
  --pitch 60 --duration "$Q" >/dev/null 2>&1 || true
wait_for "$SHM" 2 60 1 || \
  fail "the note was not accepted on the bus-1 stem (track 2, count $(count_pitch "$SHM" 2 60))
        — this is midi_per_bus_check's territory and it must pass before persistence can
        mean anything"
# A second note on the OTHER stem, so the reload has to put each one back on its own bus
# rather than collapsing both onto one lane.
DAW_UI_SHM_NAME="$SHM" "$CLI" do note --force --track 3 --nanotick $((3 * Q)) \
  --pitch 67 --duration "$Q" >/dev/null 2>&1 || true
wait_for "$SHM" 3 67 1 || fail "the note was not accepted on the bus-2 stem (track 3)"
echo "  authored: pitch 60 on the bus-1 stem (track 2), pitch 67 on the bus-2 stem (track 3)"

after_command "$TMP" env DAW_UI_SHM_NAME="$SHM" "$CLI" do save moout --force || true
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

[ -f "$TMP/moout.uniproj.json" ] || fail "the save produced no file"

# The stems are saved as FLAGGED entries, never as plain tracks: a plain track would reload
# as a top-level lane fed by nothing, which is exactly why they used to be skipped.
python3 - "$TMP/moout.uniproj.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
kids=[t for t in d.get("tracks",[]) if t.get("is_aux_child")]
plain=[t for t in d.get("tracks",[]) if not t.get("is_aux_child") and not t.get("is_master")]
print("  saved: %d document track(s), %d aux child entr(ies), %d clip(s)"
      % (len(plain), len(kids), len(d.get("clips",[]))))
if len(kids) != 2:
    print("  FAIL: expected 2 is_aux_child entries, got %d" % len(kids)); sys.exit(1)
buses = sorted(t.get("aux_bus_index", -1) for t in kids)
if buses != [1, 2]:
    print("  FAIL: stems saved against buses %s, expected [1, 2] — a stem keyed by its slot"
          " index instead of its BUS reattaches to the wrong lane whenever the plugin's bus"
          " layout changes" % buses); sys.exit(1)
if any(t.get("parent_id") != 0 for t in kids):
    print("  FAIL: a saved stem does not name its parent"); sys.exit(1)
if not d.get("clips"):
    print("  FAIL: the stems' notes reference clips that were not written to the pool —"
          " the placements would reload pointing at nothing"); sys.exit(1)
PY
[ $? -eq 0 ] || exit 1

# ---- RELOAD IN A FRESH ENGINE. A new process starts with no notes anywhere, so a note
# that comes back on a stem can only have come from the file.
SHM2="/mop2_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/eng2.log" 2>&1 ) &
ENG=$!
# Wait for the engine to be READY rather than sleeping a guessed amount. The pattern is
# "UI: command thread started" because this engine boots with no project, so wait_for_boot's
# default (a project.load) would never appear; that thread reads the command ring, so it is
# the marker that means "ready to be told something".
wait_for_boot "$TMP/eng2.log" "$ENG" 80 "UI: command thread started"
DAW_UI_SHM_NAME="$SHM2" "$CLI" do load moout --force >/dev/null 2>&1 || true
sleep 2

RECHILD=$(grep -c "multiout.child_created" "$TMP/eng2.log" || true)
[ "$RECHILD" = "2" ] || fail "after the reload there are $RECHILD derived children, not 2"

wait_for "$SHM2" 2 60 1 || \
  fail "the bus-1 stem's note did not survive the reload (track 2, count
        $(count_pitch "$SHM2" 2 60)) — it was accepted, it sounded, and the save threw it
        away. restored: $(grep -o '\"event\":\"multiout.child_restored\"[^}]*' "$TMP/eng2.log" | tr '\n' ' ')"
wait_for "$SHM2" 3 67 1 || \
  fail "the bus-2 stem's note did not survive the reload (track 3, count $(count_pitch "$SHM2" 3 67))"
# Each note came back on ITS OWN stem, not both on one. A reattach keyed on the wrong thing
# would satisfy the counts above while putting the whole arrangement on one lane.
[ "$(count_pitch "$SHM2" 2 67)" = "0" ] || fail "the bus-2 note also appeared on the bus-1 stem"
[ "$(count_pitch "$SHM2" 3 60)" = "0" ] || fail "the bus-1 note also appeared on the bus-2 stem"
echo "  reload (fresh engine): pitch 60 back on the bus-1 stem, 67 on the bus-2 stem, not mixed"

# And the parent did NOT inherit the stems' notes — a child folded into its parent would
# play every stem's material through the parent's main output.
[ "$(count_pitch "$SHM2" 0 60)" = "0" ] && [ "$(count_pitch "$SHM2" 0 67)" = "0" ] || \
  fail "the parent track picked up the stems' notes"
[ "$(count_pitch "$SHM2" 1 60)" = "0" ] && [ "$(count_pitch "$SHM2" 1 67)" = "0" ] || \
  fail "the unrelated document track picked up the stems' notes"
echo "  the parent and the filler track are still empty — the stems did not fold into them"

# ---- SAVE AGAIN: a load -> save round trip must be faithful, or the second save quietly
# deletes what the first one wrote (the mod-link failure mode).
after_command "$TMP" env DAW_UI_SHM_NAME="$SHM2" "$CLI" do save moagain --force || true
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

python3 - "$TMP/moout.uniproj.json" "$TMP/moagain.uniproj.json" <<'PY'
import json,sys
def stems(p):
    d=json.load(open(p))
    clips={c["id"]: sorted((n["nanotick"], n["pitch"]) for n in c.get("notes",[]))
           for c in d.get("clips",[])}
    out={}
    for t in d.get("tracks",[]):
        if not t.get("is_aux_child"): continue
        out[t.get("aux_bus_index")] = sorted(
            (pl["at"], tuple(clips.get(pl["clip_id"], []))) for pl in t.get("placements",[]))
    return out
a,b=stems(sys.argv[1]),stems(sys.argv[2])
if a!=b:
    print("  FAIL: load -> save is not faithful for stems.\n    first: %s\n    again: %s"%(a,b))
    sys.exit(1)
print("  round trip: the second save reproduces the stems exactly (%d)"%len(a))
PY
[ $? -eq 0 ] || exit 1

# ---- AND THE SAME MATERIAL SURVIVES THE TRACK COUNT CHANGING.
#
# A child's id is handed out from the live track count at derivation time, so adding one
# document track renumbers every stem. This inserts a THIRD document track into the saved
# file, pushing the stems from ids 2/3 to 3/4, and reloads. Their buses have not moved, so
# keyed by bus the material follows; keyed by anything that renumbers with the track count,
# it lands on the wrong lane or nowhere the first time the user adds a track.
python3 - "$TMP/moout.uniproj.json" "$TMP/shifted.uniproj.json" <<'PYSHIFT'
import json,sys
src,dst=sys.argv[1],sys.argv[2]
d=json.load(open(src))
plain=[t for t in d["tracks"] if not t.get("is_aux_child") and not t.get("is_master")]
kids=[t for t in d["tracks"] if t.get("is_aux_child")]
master=[t for t in d["tracks"] if t.get("is_master")]
extra=json.loads(json.dumps(plain[-1]))       # same shape, nothing on it
extra.update({"track_id":2,"name":"Extra","device_chain":[],"placements":[],
              "mod_links":[],"automation":[]})
d["tracks"]=plain+[extra]+master+kids
d["meta"]["name"]="shifted"
json.dump(d,open(dst,"w"))
print("  shifted fixture: a third document track inserted, so the stems land at ids 3 and 4")
PYSHIFT
[ $? -eq 0 ] || exit 1

# AND ITS ARTIFACTS COME WITH IT. A schema-6 document names an immutable generation directory and
# commits every plugin artifact in it by digest (AE-P1.2 G2-B item 18), so a document and its
# `<name>.state/generations/<generation>/` are ONE UNIT. Copying the json alone produced a project
# claiming a blob that was not there, which the load now refuses outright — correctly: that is
# `present_file_rules`. The edit this fixture makes is "insert a track", not "throw the plugin
# state away", so the state directory is copied with it.
cp -R "$TMP/moout.uniproj.state" "$TMP/shifted.uniproj.state" 2>/dev/null || {
  echo "  FAIL: could not copy the state directory alongside the shifted fixture"
  exit 1
}

SHM3="/mop3_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/eng3.log" 2>&1 ) &
ENG=$!
# Wait for the engine to be READY rather than sleeping a guessed amount. The pattern is
# "UI: command thread started" because this engine boots with no project, so wait_for_boot's
# default (a project.load) would never appear; that thread reads the command ring, so it is
# the marker that means "ready to be told something".
wait_for_boot "$TMP/eng3.log" "$ENG" 80 "UI: command thread started"
DAW_UI_SHM_NAME="$SHM3" "$CLI" do load shifted --force >/dev/null 2>&1 || true
sleep 2.5

wait_for "$SHM3" 3 60 1 || \
  fail "with a third document track inserted, the bus-1 stem (now track 3) does not carry
        pitch 60 (count $(count_pitch "$SHM3" 3 60)). The reattach is keyed on something
        that renumbers with the track count — the material has been misfiled.
        restored: $(grep -o '"event":"multiout.child_restored"[^}]*' "$TMP/eng3.log" | tr '\n' ' ')"
wait_for "$SHM3" 4 67 1 || \
  fail "the bus-2 stem (now track 4) does not carry pitch 67 (count $(count_pitch "$SHM3" 4 67))"
# Nothing landed on the parent, the filler, or the inserted track.
for t in 0 1 2; do
  [ "$(count_pitch "$SHM3" "$t" 60)" = "0" ] && [ "$(count_pitch "$SHM3" "$t" 67)" = "0" ] || \
    fail "document track $t picked up a stem's material"
done
echo "  renumbered: with a track inserted, each stem's material followed its BUS (3 and 4)"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

echo "multiout_persist_check: PASS — what you author on a stem survives save and reload"
