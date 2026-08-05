#!/usr/bin/env bash
# Check the arrangement placement ops (48-51) + the STABLE placement id. Two clips placed
# on one track; then Move / Resize / Remove / Add via daw-cli, reading the published clip
# extents back after each. The placement id must NOT change across a Move/Resize (the whole
# point — the frontend keys drags on it), and each op must land on the right placement.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/placement_ops_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
BAR=$((4 * Q))
TMP="$(mktemp -d)"
SHM="/placement_ops_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/p.uniproj.json" "$Q" "$BAR" <<'PY'
import json,sys
out,Q,BAR=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
def clip(cid):
    return {"id":cid,"name":f"c{cid}","length":BAR,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":[{"nanotick":0,"duration":Q//2,"pitch":60,"velocity":100,"column":0,"note_id":cid}],"chords":[]}
clips=[clip(1),clip(2)]
# two placements: clip 1 at bar 0, clip 2 at bar 2. id omitted -> engine assigns stable ids.
pls=[{"clip_id":1,"at":0,"length":BAR,"notes":[],"chords":[],"mutes":[]},
     {"clip_id":2,"at":2*BAR,"length":BAR,"notes":[],"chords":[],"mutes":[]}]
tr={"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":pls}
json.dump({"schema_version":4,"meta":{"name":"p"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":clips,"tracks":[tr]},open(out,"w"))
PY

( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 16 >"$TMP/eng.log" 2>&1 ) &
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
# SEVEN FIXED SLEEPS USED TO STAND IN THIS FILE, and `sleep 2` for the boot is a claim about how
# fast this machine starts an engine while a hundred other tests run. The sibling check
# track_edit_check lost exactly that race on 2026-08-05 and reported two product defects with
# every read at -1.
#
# THE PATTERN MATTERS: this engine is launched with NO --project, so it never emits a
# project.load at boot and wait_for_boot's default condition would wait for something that never
# arrives — consuming the engine's whole life and then blaming it for exiting.
cli() { DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started"
cli do load p --force >/dev/null 2>&1 || true
# ONE load, not two: the boot performed none, so the explicit load is the first.
wait_for_loads "$TMP/eng.log" "$ENG" 1 60 "the explicit load of the placement fixture"

# `get extents` used to print a trailing comma before ']', so it announced itself as JSON and
# would not parse — fixed once something finally called json.load on it instead of grepping. The
# tolerant re.sub below is kept as a belt for an older engine and is a no-op on valid output. A
# helper file rather than inline python avoids quoting hell. `parse.py byclip <clipId>` -> "pid start end";
# `parse.py count` -> N. Reads the last snapshot in ext.json.
cat > "$TMP/parse.py" <<'PY'
import json, re, sys
data = re.sub(r",(\s*])", r"\1", open(sys.argv[1]).read() or "[]")
d = json.loads(data)
mode = sys.argv[2]
if mode == "count":
    print(len(d))
else:
    c = int(sys.argv[3])
    e = next((x for x in d if x["clip"] == c), None)
    print(f'{e["placement"]} {e["start"]} {e["end"]}' if e else "NONE 0 0")
PY
snap() { DAW_UI_SHM_NAME="$SHM" "$CLI" get extents >"$TMP/ext.json" 2>/dev/null || true; }
byclip() { python3 "$TMP/parse.py" "$TMP/ext.json" byclip "$1"; }
count() { python3 "$TMP/parse.py" "$TMP/ext.json" count; }

# RE-SNAP ON EVERY POLL. The observable lives in a FILE this check re-fetches, so a predicate
# that reads ext.json without calling snap first tests a stale copy for the whole timeout — the
# same defect as a $(...) expanded once at parse time, wearing a different coat.
snap_until() {  # snap_until <secs> <predicate-fn>
  local secs="$1"; shift
  local i=0
  while [ "$i" -lt $(( secs * 4 )) ]; do
    snap
    "$@" && return 0
    sleep 0.25
    i=$(( i + 1 ))
  done
  snap
  return 1
}

# THE LOAD EVENT IS NOT THE PUBLISH. wait_for_loads above says the engine ADOPTED the document;
# the extents reach shared memory on the consumer's own tick, some time later. The `sleep 1` this
# replaced was covering that second step, and dropping it made the very first read return
# `count=0` — after which every assertion below failed, describing an engine that had done
# nothing wrong. This is the per-site rule: next thing reads PUBLISHED state, so wait on the
# published state.
initial_extents_ready() { [ "$(count)" -ge 2 ]; }
snap_until 30 initial_extents_ready || {
  echo "  FAIL(setup): the fixture's two placements never reached the published extents, so"
  echo "        nothing below would be a statement about Move/Resize/Remove/Add."
  tail -8 "$TMP/eng.log" | sed 's/^/          /'; exit 1; }

read -r P1 S1 _ <<<"$(byclip 1)"     # clip 1's placement id + start
read -r P2 _  _ <<<"$(byclip 2)"
echo "initial: clip1 placement=$P1 start=$S1 ; clip2 placement=$P2 ; count=$(count)"

# Move clip1's placement to bar 4.
cli do move-placement --track 0 --placement "$P1" --at $((4*BAR)) --force >/dev/null 2>&1 || true
moved_to_bar4() { local _p _s; read -r _p _s _ <<<"$(byclip 1)"; [ "$_s" = "$((4*BAR))" ]; }
snap_until 20 moved_to_bar4 || true
read -r P1b S1b _ <<<"$(byclip 1)"
# Resize clip1's placement to length = 1 bar.
cli do resize-placement --track 0 --placement "$P1" --length $BAR --force >/dev/null 2>&1 || true
resized_to_one_bar() { local _p _s _e; read -r _p _s _e <<<"$(byclip 1)"; [ "$(( _e - _s ))" = "$BAR" ]; }
snap_until 20 resized_to_one_bar || true
read -r P1c S1c E1c <<<"$(byclip 1)"
LEN1=$((E1c - S1c))
# Remove clip2's placement. THE BASELINE IS TAKEN BEFORE THE COMMAND, not after: reading it
# afterwards happens to work only because ext.json still holds the previous snapshot, which is an
# accident of ordering rather than a fact anyone stated.
C_BEFORE_RM=$(count)
count_dropped() { [ "$(count)" -lt "$C_BEFORE_RM" ]; }
cli do remove-placement --track 0 --placement "$P2" --force >/dev/null 2>&1 || true
snap_until 20 count_dropped || true
C_AFTER_RM=$(count)
# Add a new placement of clip 1 at bar 6.
C_BEFORE_ADD=$(count)
count_rose() { [ "$(count)" -gt "$C_BEFORE_ADD" ]; }
cli do add-placement --track 0 --clip 1 --at $((6*BAR)) --length $BAR --force >/dev/null 2>&1 || true
snap_until 20 count_rose || true
C_AFTER_ADD=$(count)

# ---- A LENGTH-0 PLACEMENT OF A LENGTH-0 CLIP STILL COVERS ITS CONTENT.
#
# Length 0 means "use the clip's length"; a clip length of 0 means a LINEAR clip that plays once
# and does not loop. Both zero used to publish startTick == endTick, so a client testing
# containment found the placement EMPTY — and the web UI's shared-clip warning went silent on
# exactly the placement somebody had just created, which is when they are most likely to type into
# it. Note entry already resolved this correctly (locateEditTarget's three-step rule: explicit
# length, else the clip's loop length, else its content end), so the engine held two answers to
# "how far does this placement reach" and published the wrong one.
python3 - "$TMP/zero.uniproj.json" "$Q" <<'PYZ'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
# Content ends at 2Q: two quarter notes back to back.
clip = {"id": 1, "name": "linear", "length": 0, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1},
                  {"nanotick": Q, "duration": Q, "pitch": 64, "velocity": 100,
                   "column": 0, "note_id": 2}]}
tr = {"track_id": 0, "name": "A", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 0,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "zero"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PYZ
DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" do load zero --force >/dev/null 2>&1 || true
sleep 1.5
snap
ZSPAN="$(python3 "$TMP/parse.py" "$TMP/ext.json" byclip 1 2>/dev/null || echo "NONE")"
case "$ZSPAN" in
  "1 0 $((2 * Q))") echo "  zero-length: a linear clip's placement covers its content ($ZSPAN)" ;;
  "1 0 0") echo "FAIL: a length-0 placement of a length-0 clip publishes an EMPTY extent (start ==
        end). A client testing containment finds nothing there, so anything keyed on 'which
        placement is the cursor in' goes silent on a freshly created placement"; ok=0 ;;
  *) echo "FAIL: unexpected extent for the linear clip: [$ZSPAN], expected '1 0 $((2 * Q))'"; ok=0 ;;
esac

wait "$ENG" 2>/dev/null || true

echo "after move  : clip1 placement=$P1b start=$S1b (want id==$P1, start==$((4*BAR)))"
echo "after resize: clip1 placement=$P1c len=$LEN1 (want id==$P1, len==$BAR)"
echo "after remove: count=$C_AFTER_RM (want 1)"
echo "after add   : count=$C_AFTER_ADD (want 2)"

rm -rf "$TMP"
ok=1
[ -n "$P1" ] && [ "$P1" != "0" ] && [ "$P1" != "NONE" ] || { echo "FAIL: no stable placement id published"; ok=0; }
[ "$P1b" = "$P1" ] && [ "$S1b" = "$((4*BAR))" ] || { echo "FAIL: Move — id changed or wrong start"; ok=0; }
[ "$P1c" = "$P1" ] && [ "$LEN1" = "$BAR" ] || { echo "FAIL: Resize — id changed or wrong length"; ok=0; }
[ "$C_AFTER_RM" = "1" ] || { echo "FAIL: Remove — count not 1"; ok=0; }
[ "$C_AFTER_ADD" = "2" ] || { echo "FAIL: Add — count not 2"; ok=0; }
[ "$ok" = "1" ] && echo "placement_ops_check: PASS — Move/Resize/Remove/Add work and the placement id stays stable" \
                || { echo "placement_ops_check: FAIL"; exit 1; }
