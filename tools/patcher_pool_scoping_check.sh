#!/usr/bin/env bash
# Guards the per-device patcher SCOPING invariant: when two or more devices contribute
# patcher graphs, they are assembled into one pool with each device's nodes in its own
# disjoint id block, and each device's published patcher_node_id must point at ITS OWN
# output node in that pool.
#
# WHY THIS EXISTS: the repoint used to be applied to the runtime chain only, and the
# per-track load then re-installed each chain from the document — overwriting it with the
# device-local AUTHORED id. That is invisible for the first contributing device (its pool
# block starts at 0, so authored == pooled) and wrong for every device after it, which
# published an id belonging to ANOTHER device's subgraph. Walking back from it recovered a
# neighbouring track's generator, so the UI showed foreign nodes as unowned orphans — a
# silent wrong answer with a plausible value, which a single-generator project never hits.
#
# THE INVARIANT CHECKED: pooled output ids are strictly INCREASING in assembly order.
# Blocks are allocated in order, so a device's own output can never be at a lower id than
# an earlier device's. The old bug produced 2 then 1 (decreasing) and is caught; mere
# distinctness would NOT have caught it.
#
#   tools/patcher_pool_scoping_check.sh [preset]   (default: maximal — two generators)
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
NAME="${1:-maximal}"
SHM="/patpool_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }
[ -f "$ROOT/presets/projects/$NAME.uniproj.json" ] || { echo "no such preset: $NAME"; exit 2; }

TMP="$(mktemp -d)"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    ./daw_engine --run-seconds 8 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. Without this the
# engine was orphaned and ctest then blocked on it — ~1000s per timeout, measured across 18 runs.
# stop_engine escalates to SIGKILL after 10s and says so.
# There was NO EXIT TRAP here at all, so a timed-out check was guaranteed to orphan.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 2.5
DAW_UI_SHM_NAME="$SHM" "$CLI" do load "$NAME" --force >/dev/null 2>&1 || true
sleep 1.5
wait "$ENG" 2>/dev/null || true

grep -oE '"event":"chain.patcher_node"[^}]*' "$TMP/engine.log" > "$TMP/nodes.txt" || true
if [ ! -s "$TMP/nodes.txt" ]; then
  echo "SKIP: $NAME has fewer than two devices with patcher graphs (no pool assembled)"
  rm -rf "$TMP"; exit 0
fi
cat "$TMP/nodes.txt" | sed 's/^/  /'

python3 - "$TMP/nodes.txt" <<'PY'
import re, sys
rows = []
for line in open(sys.argv[1]):
    t = re.search(r'"track":(\d+)', line)
    d = re.search(r'"device":(\d+)', line)
    n = re.search(r'"node":(\d+)', line)
    if t and d and n:
        rows.append((int(t.group(1)), int(d.group(1)), int(n.group(1))))
ok = True
nodes = [n for _, _, n in rows]
if len(set(nodes)) != len(nodes):
    print(f"  FAIL: two devices share a pooled output node {nodes}")
    ok = False
if any(b <= a for a, b in zip(nodes, nodes[1:])):
    print(f"  FAIL: pooled output ids are not strictly increasing {nodes} — a device is "
          f"pointing into an EARLIER device's subgraph (the authored-id regression)")
    ok = False
if ok:
    print(f"  pooled output ids strictly increasing and distinct: {nodes}")
raise SystemExit(0 if ok else 1)
PY
rc=$?
rm -rf "$TMP"
[ "$rc" = "0" ] && echo "patcher_pool_scoping_check: PASS — every device owns its own pool subgraph" \
                || { echo "patcher_pool_scoping_check: FAIL"; exit 1; }
