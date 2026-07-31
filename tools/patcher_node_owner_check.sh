#!/usr/bin/env bash
# EVERY PUBLISHED PATCHER NODE NAMES THE DEVICE IT CAME FROM.
#
# The patcher region publishes the ASSEMBLED POOL — a union of every contributing device's graph
# with re-id'd nodes — so `UiPatcherRegion::deviceId` has no answer to give and was never written.
# It was declared and left 0, which a reader could only take as "device 0".
#
# That left a surface able to DRAW the pool and unable to EDIT any of it. Every patcher command
# carries kUiPatcherFlagHasDeviceId to say which graph it means, and a UI cannot set that flag
# without knowing which device the node it is editing belongs to. So all its edits went to the
# shared pool — which, since patcher-is-a-device, is not the graph a project renders. The web-UI
# agent measured the cost from the other end: a knob nudge that is HEARD, DRAWN, and NOT SAVED.
#
# "Which device is this GRAPH" has no answer. "Which device is this NODE" always does, so that is
# what is published — on the node, not in a vector beside it, because a parallel array is a second
# fact about the same thing and desyncs the first time anything reorders or filters.
#
# TWO PROPERTIES:
#   OWNED      every node names a REAL device, and the nodes of two different devices name
#              DIFFERENT ones
#   PARTITION  each device's node count matches the graph it actually authored — which is what
#              catches an owner that is merely non-zero, or that names the same device for all
#
# TWO DEVICES ARE THE WHOLE FIXTURE. With one contributing device every node's owner is the same
# value, and "always device 1" is indistinguishable from "correctly device 1" — the same blind
# spot as a one-track kit fixture, which is how the read-back handed back another track's answer
# for months.
#
#   tools/patcher_node_owner_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP/o.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
DIRECT = 4294967294
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# DEVICE 1: THREE nodes. DEVICE 2: TWO. Different counts, so "which device owns how many" is a
# fact the check can assert rather than a coincidence two equal graphs would hide.
dev1 = {"device_id": 1, "kind": "patcher_event", "capability_mask": 1, "patcher_node_id": 1,
        "host_slot_index": DIRECT, "bypass": False,
        "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
        "patcher": {"nodes": [
            {"id": 0, "type": "euclidean",
             "euclidean": {"steps": 16, "hits": 5, "offset": 0, "duration_ticks": Q // 4,
                           "degree": 1, "octave_offset": 0, "velocity": 100, "base_octave": 4}},
            {"id": 2, "type": "random_degree",
             "random_degree": {"degree": 5, "velocity": 100, "duration_ticks": Q // 4}},
            {"id": 1, "type": "event_out"}],
          "edges": [
            {"src_node_id": 0, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0,
             "kind": "event"},
            {"src_node_id": 2, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0,
             "kind": "event"}]}}
dev2 = {"device_id": 2, "kind": "patcher_event", "capability_mask": 1, "patcher_node_id": 1,
        "host_slot_index": DIRECT, "bypass": False,
        "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
        "patcher": {"nodes": [
            {"id": 0, "type": "euclidean",
             "euclidean": {"steps": 8, "hits": 3, "offset": 0, "duration_ticks": Q // 4,
                           "degree": 1, "octave_offset": 0, "velocity": 100, "base_octave": 4}},
            {"id": 1, "type": "event_out"}],
          "edges": [
            {"src_node_id": 0, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0,
             "kind": "event"}]}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev1, dev2], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "o"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

export DAW_UI_SHM_NAME="/powner_$$" DAW_PROJECT_DIR="$TMP"
( cd "$BUILD" && ./daw_engine --project o --run-seconds 20 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 160); do
  grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null || \
  fail "the engine never loaded its project — see $TMP/eng.log"
sleep 1.0

# WRITTEN TO A FILE AND PASSED BY PATH, not piped. `python3 - <<PY` takes its SCRIPT from stdin,
# so a pipe into it is swallowed and sys.stdin.read() returns empty — which reads as "the command
# produced no JSON" and is a statement about the harness, not about the engine.
"$CLI" get patcher >"$TMP/patcher.json" 2>"$TMP/cli.err"
[ -s "$TMP/cli.err" ] && echo "  cli stderr: $(cat "$TMP/cli.err")"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -s "$TMP/patcher.json" ] || fail "get patcher returned nothing"

python3 - "$TMP/patcher.json" <<'PYC' || exit 1
import sys, json
raw = open(sys.argv[1]).read()
try:
    d = json.loads(raw)
except Exception as e:
    print("  FAIL: get patcher did not return JSON (%s):" % e)
    print(raw[:400])
    raise SystemExit(1)
nodes = d.get("nodes", [])
if not nodes:
    print("  FAIL: the pool published no nodes, so there is nothing to own. Both devices carry a"
          " graph, so the assembly itself did not run")
    raise SystemExit(1)
owners = {}
for n in nodes:
    owners.setdefault(n.get("owner_device"), []).append(n.get("id"))
print("  %d pool nodes, owners: %s" % (
    len(nodes), {k: len(v) for k, v in sorted(owners.items(), key=lambda x: (x[0] is None, x[0]))}))

# ---- OWNED. Nothing may be unowned, and the two devices must be told apart.
if 0 in owners:
    print("  FAIL: %d node(s) published owner_device 0 — no owning device. Every node in the pool"
          " came from a device's graph, so 0 means the owner was never written and a UI reading it"
          " would send its edits to the shared pool." % len(owners[0]))
    raise SystemExit(1)
if len(owners) < 2:
    print("  FAIL: every node names the same device %s. Two devices contributed graphs, so an"
          " owner that cannot tell them apart is not an owner — it is a constant." % list(owners))
    raise SystemExit(1)

# ---- PARTITION. Device 1 authored THREE nodes, device 2 authored TWO.
want = {1: 3, 2: 2}
got = {k: len(v) for k, v in owners.items()}
if got != want:
    print("  FAIL: node counts per device are %r, expected %r. The owners are distinct but do not"
          " match the graphs the devices actually authored, so the stamping is off by a device"
          " somewhere in the assembly." % (got, want))
    raise SystemExit(1)
print("  device 1 owns 3 nodes, device 2 owns 2 — matching what each authored")
PYC

echo "patcher_node_owner_check: PASS — every pool node names the device it came from"
