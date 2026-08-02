#!/usr/bin/env bash
# MODULATION THAT A UI CAN ACTUALLY DRIVE. Four findings from the frontend agent wiring the
# modulation surface, all on the engine side, all in the same family: the command set worked and
# the STATE around it did not.
#
#   PUBLISHES ON LOAD   the per-track load loop emitted the chain, routing and mod snapshots about
#                       150 lines BEFORE `modRegistry.links = source.modLinks`, so it published an
#                       EMPTY registry — and emitModSnapshot iterated the links, so empty emitted
#                       NOTHING AT ALL. Open a project with modulation and the UI was told nothing,
#                       forever: there is no RequestModSnapshot, so it was absent rather than late.
#                       The chain had already been fixed for exactly this, one line away; routing
#                       and mod were left behind.
#   REMOVES BY ID       RemoveModLink fell through the ADD's validation — both device ids looked up
#                       in the chain, plus the forward-order test — so a caller that knew a link's
#                       id had to send the devices it happens to connect. Unstated ids default to
#                       0, so on a project whose device ids start higher EVERY removal was refused
#                       while the caller was told it worked, and links piled up. It looked correct
#                       only because rack.uniproj.json has a device 0.
#   EMPTY IS A STATE    an empty registry published nothing, so removing a track's LAST link was
#                       invisible: the rest republish under a new version when there are others,
#                       but the last one left a lit badge for a link that no longer exists. Now a
#                       one-entry kModLinkIdAuto sentinel carries the version, the same trick the
#                       chain snapshot uses for an empty chain.
#   DEPTH IN PLACE      there was no way to CHANGE a link. AddModLink with an existing id is
#                       refused, so "make this shallower" was remove + add — which changed the id,
#                       dropped the uid16 (silently disabling the modulation) and was not atomic.
#                       That put a depth SLIDER out of reach: a continuous gesture would tear the
#                       link down and rebuild it every frame. SetModLinkDepth (63) edits in place.
#
# AddModLink still REFUSES an existing id rather than replacing, deliberately: a colliding add
# must not silently overwrite a link.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/modlink_check.sh
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
trap cleanup EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# DEVICE IDS START AT 5, which is the whole point of the fixture. With ids 0 and 1 a removal that
# forgot to send them would resolve the 0 default and pass — that is exactly why this went
# unnoticed against the rack preset.
python3 - "$TMP/ml.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def dev(did, kind, name):
    return {"device_id": did, "kind": kind, "capability_mask": 5,
            "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
            "vst_ref": {"vendor": "", "name": name, "path": "", "uid16": ""}}
# Two links on one track, so the LAST-link case and the one-of-several case are both reachable.
# Forward order (5 before 6) because a later device may not modulate an earlier one.
links = [
    {"link_id": 3, "src": {"device_id": 5, "source_id": 0, "kind": "macro"},
     "dst": {"device_id": 6, "target_id": 1, "kind": "vst_param", "uid16": ""},
     "depth": 1.0, "bias": 0.0, "rate": "block", "enabled": True},
    {"link_id": 4, "src": {"device_id": 5, "source_id": 1, "kind": "macro"},
     "dst": {"device_id": 6, "target_id": 2, "kind": "vst_param", "uid16": ""},
     "depth": 0.5, "bias": 0.0, "rate": "block", "enabled": True},
]
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(),
      "device_chain": [dev(5, "vst_instrument", "identity"), dev(6, "vst_effect", "identity")],
      "mod_links": links, "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "ml"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/mlchk_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load ml --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
wait_for_event "$TMP/eng.log" '"event":"modsnapshot.published"' 80 "the first mod snapshot" \
  || true

# The count of links in the LAST snapshot the engine published for track 0.
last_published() {
  grep '"event":"modsnapshot.published"' "$TMP/eng.log" | grep '"track":0' | tail -1 \
    | sed -n 's/.*"links":\([0-9]*\).*/\1/p'
}

# ---- PUBLISHES ON LOAD. Two links in the file, so the load must publish two.
wait_for_published 30 "2" last_published || true
PUB="$(last_published)"
[ "${PUB:-}" = "2" ] || \
  fail "a load published '${PUB:-nothing}' mod links for a project carrying 2. The per-track load
        loop emits the snapshots BEFORE it adopts source.modLinks, so it publishes an empty
        registry — and an empty registry emitted nothing at all, so a UI opening this project was
        told nothing and had no way to ask (there is no RequestModSnapshot)"
echo "  publishes on load: both links reach the UI without an edit ($PUB)"

# ---- DEPTH IN PLACE, keeping the id. Sent with NO device arguments at all.
cli do mod-depth --track 0 --link 4 --depth 0.25 >/dev/null 2>&1 || true
sleep 1.2
grep '"event":"modlink.depth_set"' "$TMP/eng.log" | grep -q '"link":4' || \
  fail "SetModLinkDepth did not apply. Without it, changing a depth is remove + add: the id
        changes, the uid16 is dropped (which silently disables the modulation) and it is not
        atomic — so a depth slider would tear the link down and rebuild it every frame:
        $(grep -o '"event":"modlink[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
[ "$(last_published)" = "2" ] || \
  fail "a depth change altered the link COUNT — it must edit in place, not replace"
echo "  depth in place: link 4's depth changed with no device arguments and no id change"

# ---- REMOVES BY ID, with no device arguments. This is the one that was refused as
# kModErrInvalidDevice because the unstated device ids defaulted to 0.
cli do unmod-link --track 0 --link 3 >/dev/null 2>&1 || true
sleep 1.2
grep '"event":"modlink.removed"' "$TMP/eng.log" | grep -q '"link":3' || \
  fail "removing a link by id alone was refused. A remove has no business needing the devices the
        link connects: they default to 0, so on any project whose device ids start higher EVERY
        removal was refused while the caller was told it succeeded, and the links piled up:
        $(grep -o '"event":"modlink[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
[ "$(last_published)" = "1" ] || \
  fail "after removing one of two links the snapshot publishes $(last_published), expected 1"
echo "  removes by id: no device arguments, and the survivor republishes"

# ---- EMPTY IS A STATE. Removing the LAST link must still publish, or the badge stays lit for a
# link that no longer exists.
cli do unmod-link --track 0 --link 4 >/dev/null 2>&1 || true
sleep 1.2
LAST="$(grep '"event":"modsnapshot.published"' "$TMP/eng.log" | grep '"track":0' | tail -1)"
echo "$LAST" | grep -q '"links":0' || \
  fail "removing the LAST link published '$LAST' — an empty registry used to emit nothing, so the
        removal was invisible and the UI kept a lit badge for a link that is gone"
echo "$LAST" | grep -q '"empty_sentinel":true' || \
  fail "the empty snapshot went out without the sentinel, so it carries no version for a client
        to cache-break on: $LAST"
echo "  empty is a state: the last removal publishes a sentinel that still carries the version"

# ---- AND AN ADD ONTO AN EXISTING ID IS STILL REFUSED, not silently replaced. The update path is
# a separate opcode ON PURPOSE: a colliding add must not overwrite a link.
cli do mod-link --track 0 --link 9 --source-device 5 --target-device 6 --target-id 3 >/dev/null 2>&1 || true
sleep 1.1
cli do mod-link --track 0 --link 9 --source-device 5 --target-device 6 --target-id 4 >/dev/null 2>&1 || true
sleep 1.1
[ "$(last_published)" = "1" ] || \
  fail "a second add onto id 9 changed the published set — an add must refuse an existing id
        rather than replace it, or a mistyped id silently overwrites a link"
echo "  refuses: a colliding add is refused rather than replacing the link"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "modlink_check: PASS — modulation publishes on load, removes by id, and has an in-place depth"
