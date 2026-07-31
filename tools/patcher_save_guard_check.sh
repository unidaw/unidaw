#!/usr/bin/env bash
# A SAVE MUST NOT INVENT PATCHER DATA the user never authored — and must still keep the data
# they did.
#
# The save has a legacy branch that parks the shared node pool on the first track's
# instrument, so that the one global graph the engine used to run round-trips. The engine also
# SEEDS that pool at startup with a demo graph (Euclidean 16/5 + Passthrough +
# AudioPassthrough). Those two facts together meant that opening a plain one-instrument
# project with no patcher data anywhere and saving it stamped three patcher nodes onto the
# user's instrument. Verified before the fix: a fixture with zero patcher data came back with
# ['euclidean', 'passthrough', 'audio_passthrough'].
#
# It was not audible in that configuration, which is why it went unnoticed — but it is
# authored-looking data invented by a save, it appears in the patcher UI as a generator the
# user never added, and it flips the engine's own `documentHasPerDeviceGraphs` test on the next
# load so the second save takes a different branch than the first.
#
# TWO PROPERTIES, and the second is what stops the fix from being "disable the feature":
#   CLEAN     load -> save with no patcher edit writes NO patcher nodes
#   PRESERVED load -> edit the pool -> save DOES write it, so the round-trip the branch
#             exists for still works
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/patcher_save_guard_check.sh
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
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP/plain.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
# A plain VST instrument and one note. NO patcher data anywhere in the document.
dev = {"device_id": 1, "kind": "vst_instrument", "capability_mask": 5,
       "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
clip = {"id": 1, "name": "n", "length": 4 * Q, "kind": "symbolic",
        "notes": [{"nanotick": Q, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1}]}
tr = {"track_id": 0, "name": "T0", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": 4 * Q,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "plain"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

node_count() {  # total patcher nodes across every device in the file
  python3 - "$1" <<'PYN'
import json, sys
doc = json.load(open(sys.argv[1]))
print(sum(len(d.get("patcher", {}).get("nodes", []))
          for t in doc.get("tracks", []) for d in t.get("device_chain", [])))
PYN
}

ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

BEFORE="$(node_count "$TMP/plain.uniproj.json")"
[ "$BEFORE" = "0" ] || fail "the fixture already has $BEFORE patcher node(s); it must have none"

SHM="/pgchk_$$"
( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 24 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load plain --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1

# ---- CLEAN: no patcher edit, so the save must write no patcher data.
cli do save plainout --force >/dev/null 2>&1 || true
sleep 1.6
[ -f "$TMP/plainout.uniproj.json" ] || fail "the save produced no file"
CLEAN="$(node_count "$TMP/plainout.uniproj.json")"
[ "$CLEAN" = "0" ] || \
  fail "a load -> save with no patcher edit wrote $CLEAN patcher node(s) onto the user's
        device. The engine's boot-default demo graph has been stamped into their project as
        if they had authored it"
echo "  clean: load -> save with no patcher edit writes no patcher nodes"

# ---- PRESERVED: an actual pool edit must still be saved, or the guard has simply broken the
# round-trip the legacy branch exists for.
cli do patcher-node --track 0 --type passthrough >/dev/null 2>&1 || true
sleep 1.2
cli do save plainedit --force >/dev/null 2>&1 || true
sleep 1.6
EDITED="$(node_count "$TMP/plainedit.uniproj.json")"
[ "${EDITED:-0}" -gt 0 ] || \
  fail "after a real patcher edit the save wrote $EDITED patcher node(s) — the guard is
        dropping live edits instead of only declining to invent them"
echo "  preserved: after a real pool edit the graph is saved ($EDITED nodes)"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

echo "patcher_save_guard_check: PASS — a save neither invents patcher data nor loses it"
