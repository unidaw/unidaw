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
# TWO PROPERTIES, CHECKED IN TWO CONFIGURATIONS:
#   OWNED      every node names a REAL device, and where two devices contribute, the nodes of
#              each name DIFFERENT ones
#   PARTITION  each device's node count matches the graph it actually authored — which is what
#              catches an owner that is merely non-zero, or that names the same device for all
#
# WHY BOTH A TWO-DEVICE AND A ONE-DEVICE FIXTURE, and this is the whole point of the file:
#
#   With TWO devices, "always device 1" and "correctly device 1" are different runs, so the
#   partition assertion can bite. A one-device fixture is blind to addressing — the same blind
#   spot as a one-track kit fixture, which is how the kit read-back handed back another track's
#   answer for months.
#
#   With ONE device, a DIFFERENT code path runs, and for a long time only the two-device case was
#   covered. Load assembles per-device graphs only when two or more devices carry one; a single
#   graph took a legacy branch that copied it into the pool verbatim and stamped no owner at all.
#   So every node published owner 0 — "no owning device" — in exactly the projects everybody
#   actually has. Here "always 0" and "correctly 0" are the same run, which is why the fixture
#   that covered the case nobody has could stay green while the common case was broken.
#
#   Neither fixture can see the other's defect. That is why there are two.
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
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# fixture <out> <devices>   — 1 device (three nodes) or 2 devices (three nodes + two).
fixture() {
  python3 - "$1" "$Q" "$2" <<'PY'
import json, sys
out, Q, ndev = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
DIRECT = 4294967294
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def patcher_device(dev_id, steps, hits, extra):
    # DEVICE 1 gets THREE nodes, DEVICE 2 gets TWO. Different counts, so "which device owns how
    # many" is a fact the check can assert rather than a coincidence two equal graphs would hide.
    nodes = [{"id": 0, "type": "euclidean",
              "euclidean": {"steps": steps, "hits": hits, "offset": 0,
                            "duration_ticks": Q // 4, "degree": 1, "octave_offset": 0,
                            "velocity": 100, "base_octave": 4}}]
    edges = []
    if extra:
        nodes.append({"id": 2, "type": "random_degree",
                      "random_degree": {"degree": 5, "velocity": 100,
                                        "duration_ticks": Q // 4}})
        edges.append({"src_node_id": 0, "src_port_id": 1, "dst_node_id": 2,
                      "dst_port_id": 0, "kind": "event"})
        edges.append({"src_node_id": 2, "src_port_id": 1, "dst_node_id": 1,
                      "dst_port_id": 0, "kind": "event"})
    else:
        edges.append({"src_node_id": 0, "src_port_id": 1, "dst_node_id": 1,
                      "dst_port_id": 0, "kind": "event"})
    nodes.append({"id": 1, "type": "event_out"})
    return {"device_id": dev_id, "kind": "patcher_event", "capability_mask": 1,
            "patcher_node_id": 1, "host_slot_index": DIRECT, "bypass": False,
            "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
            "patcher": {"nodes": nodes, "edges": edges}}
chain = [patcher_device(1, 16, 5, True)]
if ndev == 2:
    chain.append(patcher_device(2, 8, 3, False))
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": chain, "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "o"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY
}

# run_case <name> <devices> <expected owner->count map as python dict literal>
run_case() {
  local name="$1" ndev="$2" want="$3"
  echo "  --- $ndev-device project ---"
  fixture "$TMP/$name.uniproj.json" "$ndev"
  export DAW_UI_SHM_NAME="/powner_${$}_$name" DAW_PROJECT_DIR="$TMP"
  ( cd "$BUILD" && ./daw_engine --project "$name" --run-seconds 20 >"$TMP/$name.log" 2>&1 ) &
  ENG=$!
  wait_for_boot "$TMP/$name.log" "$ENG" 160
  grep -q '"event":"project.load"' "$TMP/$name.log" 2>/dev/null || \
    fail "the engine never loaded its project — see $TMP/$name.log"
  sleep 1.0

  # WRITTEN TO A FILE AND PASSED BY PATH, not piped. `python3 - <<PY` takes its SCRIPT from stdin,
  # so a pipe into it is swallowed and sys.stdin.read() returns empty — which reads as "the command
  # produced no JSON" and is a statement about the harness, not about the engine.
  "$CLI" get patcher >"$TMP/$name.json" 2>"$TMP/$name.err"
  [ -s "$TMP/$name.err" ] && echo "  cli stderr: $(cat "$TMP/$name.err")"
  kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
  [ -s "$TMP/$name.json" ] || fail "get patcher returned nothing"

  python3 - "$TMP/$name.json" "$want" "$ndev" <<'PYC' || exit 1
import sys, json, ast
raw = open(sys.argv[1]).read()
want, ndev = ast.literal_eval(sys.argv[2]), int(sys.argv[3])
try:
    d = json.loads(raw)
except Exception as e:
    print("  FAIL: get patcher did not return JSON (%s):" % e)
    print(raw[:400])
    raise SystemExit(1)
nodes = d.get("nodes", [])
if not nodes:
    print("  FAIL: the pool published no nodes, so there is nothing to own. The device(s) carry a"
          " graph, so the load itself did not run")
    raise SystemExit(1)
owners = {}
for n in nodes:
    owners.setdefault(n.get("owner_device"), []).append(n.get("id"))
print("  %d pool nodes, owners: %s" % (
    len(nodes), {k: len(v) for k, v in sorted(owners.items(), key=lambda x: (x[0] is None, x[0]))}))

# ---- OWNED. Nothing may be unowned.
if 0 in owners:
    extra = ("" if ndev > 1 else
             " With ONE patcher device the load takes the single-graph branch, which copies the"
             " device's graph into the pool and stamps no owner — and that is every project"
             " anyone has actually made.")
    print("  FAIL: %d node(s) published owner_device 0 — no owning device. Every node in the pool"
          " came from a device's graph, so 0 means the owner was never written and a UI reading it"
          " would send its edits to the shared pool.%s" % (len(owners[0]), extra))
    raise SystemExit(1)
if ndev > 1 and len(owners) < 2:
    print("  FAIL: every node names the same device %s. Two devices contributed graphs, so an"
          " owner that cannot tell them apart is not an owner — it is a constant." % list(owners))
    raise SystemExit(1)

# ---- PARTITION. Each device owns exactly the nodes it authored.
got = {k: len(v) for k, v in owners.items()}
if got != want:
    print("  FAIL: node counts per device are %r, expected %r. The owners are distinct but do not"
          " match the graphs the devices actually authored, so the stamping is off by a device"
          " somewhere in the assembly." % (got, want))
    raise SystemExit(1)
print("  owners match what each device authored: %r" % (want,))
PYC
}

# ONE DEVICE FIRST — the common case, and the one that was broken.
run_case one 1 "{1: 3}"
# TWO DEVICES — the addressing case, which a one-device fixture cannot see.
run_case two 2 "{1: 3, 2: 2}"

echo "patcher_node_owner_check: PASS — every pool node names the device it came from, with one"
echo "                          contributing device and with two"
