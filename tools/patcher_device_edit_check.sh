#!/usr/bin/env bash
# A PATCHER EDIT LANDS ON THE DEVICE YOU AIMED IT AT, AND IS SAVED.
#
# "Patcher is a device" moved the DATA model and the read-back to per-device graphs. The EDIT
# commands were never migrated — AddPatcherNode / RemovePatcherNode / ConnectPatcherNodes all
# addressed the one shared pool, and their payload had a trackId used only to label the emitted
# error, with no deviceId at all.
#
# For any project carrying per-device graphs that meant the edit was applied to the pool and never
# saved: accepted, reported as applied, and gone on reload. Before the save guard landed it was
# worse — the same edit parked the whole pool on the first instrument, overwriting its real graph
# and dropping every other device's.
#
# FOUR PROPERTIES:
#   LANDS      the edit goes into the addressed device's OWN graph
#   ISOLATED   the OTHER device's graph is untouched — the failure mode being fixed was one
#              device's edit overwriting another's
#   SAVED      it survives a save, and comes back on a reload in a FRESH engine
#   EXECUTES   the pool is re-derived, so the edit affects what RUNS rather than only what is
#              written. Assembly used to happen only at load, which made every runtime patcher
#              edit a no-op until the next open.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/patcher_device_edit_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
# THE ENGINE MUST DIE WHEN THIS CHECK DOES, including when ctest KILLS the check on a timeout.
# This trap used to remove $TMP and leave the engine running: it was only stopped on the normal
# path and inside fail(). A timed-out check therefore orphaned a possibly-hung engine, and ctest
# then blocked on it — measured at about 1000s per timeout across 18 runs, perfectly correlated
# with the timeout count. override showed it plainly: 909.87s against a TIMEOUT of 600, passing
# standalone in 23.2s.
#
# stop_engine escalates to SIGKILL after 10s and SAYS SO, so a hang stops being something to
# infer from a sample stack and becomes a line in the run.
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
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# TWO devices, each with its OWN patcher graph. Two is the minimum that can show isolation: with
# one device, an edit that overwrote "the wrong" graph would look identical to one that worked.
python3 - "$TMP/pd.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def graph(node_id, kind):
    # A minimal VALID per-device graph: one generator into an event out.
    #
    # The FIELD NAMES and the PORT NUMBER both matter and both bit on the first attempt. They are
    # src_node_id/src_port_id, not src_node/src_port — misspelled, every field parses as 0, the
    # edge becomes node 0 port 0 -> node 0 port 0, and the graph fails to build as a self-cycle
    # with nothing pointing at the typo. And the event OUTPUT port is 1, not 0.
    return {"nodes": [{"id": node_id, "type": kind,
                       "euclidean": {"steps": 16, "hits": 5, "offset": 0,
                                     "duration_ticks": 0, "degree": 1, "octave_offset": 0,
                                     "velocity": 100, "base_octave": 4}},
                      {"id": node_id + 1, "type": "event_out"}],
            "edges": [{"src_node_id": node_id, "src_port_id": 1,
                       "dst_node_id": node_id + 1, "dst_port_id": 0, "kind": "event"}]}
def dev(dev_id, node_base, kind):
    return {"device_id": dev_id, "kind": "patcher_instrument", "capability_mask": 5,
            "patcher_node_id": node_base + 1, "host_slot_index": DIRECT, "bypass": False,
            "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
            "patcher": graph(node_base, kind)}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(),
      "device_chain": [dev(0, 0, "euclidean"), dev(1, 10, "euclidean")],
      "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "pd"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

# Node counts per device, straight out of a saved file.
nodes_of() {  # nodes_of <file> -> "devId:count devId:count"
  python3 - "$1" <<'PYN'
import json, sys
doc = json.load(open(sys.argv[1]))
out = []
for t in doc.get("tracks", []):
    if t.get("is_master"):
        continue
    for d in t.get("device_chain", []):
        out.append("%s:%d" % (d.get("device_id"), len(d.get("patcher", {}).get("nodes", []))))
print(" ".join(out))
PYN
}

BEFORE="$(nodes_of "$TMP/pd.uniproj.json")"
[ "$BEFORE" = "0:2 1:2" ] || fail "the fixture should start with 2 nodes on each device, got [$BEFORE]"

SHM="/pdchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 24 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load pd --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.2

# ---- LANDS + ISOLATED. Add an LFO to DEVICE 1 only.
after_command "$TMP" cli do patcher-node --track 0 --device 1 --type lfo || true
grep -q '"event":"patcher_device_edit.applied"' "$TMP/eng.log" || \
  fail "the per-device edit was not applied. Without --device the command edits the shared pool
        and is never saved for a project with per-device graphs, so the engine must report which
        device it landed on: $(grep -o '"event":"patcher_device_edit[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"

cli do save pdout --force >/dev/null 2>&1 || true
sleep 1.6
AFTER="$(nodes_of "$TMP/pdout.uniproj.json")"
[ "$AFTER" = "0:2 1:3" ] || \
  fail "after adding one node to DEVICE 1 the saved graphs are [$AFTER], expected [0:2 1:3].
        0:3 means the edit landed on the wrong device; 0:2 1:2 means it went to the shared pool
        and was dropped by the save; anything with 5 nodes means the pool was parked on a device
        and overwrote its graph"
echo "  lands + isolated: the node went to device 1 only ([$AFTER])"

# ---- EXECUTES. Assembly used to happen only at load, so a runtime patcher edit changed nothing
# about what was running until the next open — saved and inert.
grep '"event":"patcher_device_edit.applied"' "$TMP/eng.log" | grep -q '"executing":true' || \
  fail "the edit was saved but the pool was not re-derived, so it does not affect what RUNS until
        the next load. A saved-but-inert edit is its own kind of lie: $(grep -o '"event":"patcher.reassembl[a-z_]*"[^}]*' "$TMP/eng.log" | tail -1)"
grep -q '"event":"patcher.reassembled"' "$TMP/eng.log" || \
  fail "no patcher.reassembled event — the pool was not rebuilt from the device graphs"
echo "  executes: the pool was re-derived from the device graphs after the edit"

# ---- A BAD ADDRESS IS REFUSED, not silently applied somewhere else.
after_command "$TMP" cli do patcher-node --track 0 --device 99 --type lfo || true
grep '"event":"patcher_device_edit.rejected"' "$TMP/eng.log" | grep -q '"reason":"no_such_device"' || \
  fail "an edit naming a device that does not exist was not refused. Retrying will never help, so
        the caller has to be told rather than left to assume it worked"
echo "  refuses: an unknown device id is rejected by name"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- SAVED: reload in a FRESH engine and save again. The point is what the person still has
# tomorrow, not what the process happened to hold.
SHM2="/pdchk2_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 18 >"$TMP/eng2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load pdout --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng2.log" "$ENG" 80
sleep 1.2
after_command "$TMP" cli do save pdagain --force || true
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
AGAIN="$(nodes_of "$TMP/pdagain.uniproj.json")"
[ "$AGAIN" = "$AFTER" ] || \
  fail "load -> save in a fresh engine changed the graphs: [$AFTER] became [$AGAIN]. The edit was
        written once and lost on the round trip, which is the same failure in a different place"
echo "  saved: the edit survives a reload in a fresh engine and a second save ([$AGAIN])"

echo "patcher_device_edit_check: PASS — a patcher edit lands on its device, is saved, and runs"
