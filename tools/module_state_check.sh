#!/usr/bin/env bash
# A MODULE CARRIES THE SOUND OF ITS PLUGINS, NOT JUST THEIR NAMES.
#
# `.uni` packed project.json and every sample, and NOT ONE BYTE of plugin state. Open a received
# module and the device chain is intact, every plugin is there, every sample plays — and each
# plugin comes up at its DEFAULTS. Nothing looks broken. The patches are simply gone.
#
# That is the worst shape a data-loss bug can have, because the module OPENS. `module_check.sh`
# could not see it: its fixture is a sampler, which has no opaque state, so every property it
# asserts passed with plugin state missing entirely.
#
# THE ROOT CAUSE WAS A LAMBDA'S SCOPE. `pluginStateDir` lived inside daw_engine_main.cpp, so the
# save that WRITES the blobs and the load that RESTORES them agreed with each other, and
# saveProjectModule — which has to FIND those blobs in order to pack them — could not name the
# directory at all. The packer was not ignoring plugin state; it had no way to ask where it was.
# It is now daw::pluginStateDirFor, in one place, and the archive prefix is derived from it.
#
# FOUR PROPERTIES:
#   NOT THE DEFAULT  the value under test DIFFERS from the plugin's default. Without this the
#                    whole check is vacuous: "restored correctly" and "came up at defaults" are
#                    the same observation, and the bug is precisely that they were.
#   PACKS            the blob and its manifest are INSIDE the .uni, under the state directory the
#                    unpacked document will look in
#   TRAVELS          moved to another directory with the originals DELETED, the blob unpacks
#                    byte-identical
#   SOUNDS THE SAME  and the plugin comes back up at the value that was set, not at its default
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/module_state_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

HOME_DIR="$(mktemp -d)"   # where the song is made
AWAY_DIR="$(mktemp -d)"   # the other machine
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS the check on a timeout. This trap
# removed both trees and left the engine running: a timed-out check orphaned it and ctest then
# blocked on it, ~1000s per timeout measured across 18 runs. stop_engine escalates to SIGKILL
# after 10s and says so.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$HOME_DIR" "$AWAY_DIR"; }
trap cleanup EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# The fake Identity has REAL getState/setState — its gain is four bytes of opaque blob — which is
# what makes this testable without a third-party plugin installed.
python3 - "$HOME_DIR/song.uniproj.json" <<'PY'
import json, sys
DIRECT = 4294967294
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
dev = {"device_id": 0, "kind": "vst_instrument", "capability_mask": 5,
       "patcher_node_id": 0, "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "song"}, "nanoticks_per_quarter": 960000,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/modstate_$$"
# `exec`, so $! is the ENGINE and not the subshell around it. Without it `kill "$ENG"` reaps the
# subshell and leaves the engine running: normally it still exits on its own when --run-seconds
# elapses, but an engine BLOCKED waiting for a contended audio device never starts that clock and
# lingers forever — holding the device, which is what makes the NEXT check slow. One such pair
# turned a 10-second run of this file into a 935-second one.
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" \
    DAW_PROJECT_DIR="$HOME_DIR" \
    ./daw_engine --run-seconds 40 >"$HOME_DIR/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$HOME_DIR/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$HOME_DIR" "$CLI" "$@"; }
cli do load song --force >/dev/null 2>&1 || true
wait_for_boot "$HOME_DIR/eng.log" "$ENG" 80
sleep 1.5

PARAMS="$(cli get device-params 0 0 2>/dev/null)"
read -r UID16 DEFAULT <<<"$(echo "$PARAMS" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('NONE NONE'); raise SystemExit
for p in d.get('params', []):
    if p['name'] == 'Gain':
        print(p['uid16'], p['default']); break
else:
    print('NONE NONE')
")"
[ "$UID16" != "NONE" ] || \
  fail "the Identity plugin published no Gain parameter, so there is no state to test with:
        $(echo "$PARAMS" | head -3)"

# ---- NOT THE DEFAULT. This is the guard that gives every assertion below its teeth. The defect
# is that plugins come up at their DEFAULTS after a module move, so a check that happened to set
# the value it would have defaulted to could not tell success from the bug.
SET_MILLI=250
WANT=0.250
python3 -c "raise SystemExit(0 if abs(float('$DEFAULT') - float('$WANT')) > 0.01 else 1)" || \
  fail "the value under test ($WANT) is the plugin's own default ($DEFAULT), so 'the patch
        survived' and 'the plugin came up at defaults' are the SAME observation and this check
        cannot fail. Pick a different value"

cli do set-param 0 0 "$UID16" "$SET_MILLI" >/dev/null 2>&1 || true
readvalue() {  # readvalue <cli-fn>
  $1 get device-params 0 0 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('NONE'); raise SystemExit
for p in d.get('params', []):
    if p['name'] == 'Gain':
        print('%.3f' % p['value']); break
else:
    print('NONE')
"
}
# WAITS FOR THE VALUE rather than sleeping a fixed second before reading it. The parameter is
# published on the engine's own cycle, so a fixed sleep asserts something about the machine's load
# and fails as "set-param did not take" — a statement about the command.
GOT=""
for _ in $(seq 1 60); do
  GOT="$(readvalue cli)"
  [ "$GOT" != "NONE" ] && \
    python3 -c "raise SystemExit(0 if abs(float('$GOT') - float('$WANT')) < 0.02 else 1)" && break
  sleep 0.25
done
python3 -c "raise SystemExit(0 if abs(float('${GOT:-9}') - float('$WANT')) < 0.02 else 1)" || \
  fail "set-param did not take: Gain reads $GOT, wanted $WANT after 15s. Nothing below can mean
        anything if the value was never set in the first place"
echo "  set: Gain is $GOT, and its default is $DEFAULT — the two differ, so this check can fail"

# ---- PACKS.
cli do save-module song >/dev/null 2>&1 || true
# WAITS FOR THE EVENT. A fixed sleep here asserts the engine finishes a zip write within N
# seconds, which under a parallel ctest is a claim about the machine and fails as "the save
# reported failure" — a statement about the product.
for _ in $(seq 1 80); do
  grep -q '"event":"project.module_saved"' "$HOME_DIR/eng.log" && break
  sleep 0.25
done
grep '"event":"project.module_saved"' "$HOME_DIR/eng.log" | tail -1 | grep -q '"ok":true' || \
  fail "the module save reported failure:
        $(grep -o '\"event\":\"project.module_saved\"[^}]*' "$HOME_DIR/eng.log" | tail -1)"
[ -s "$HOME_DIR/song.uni" ] || fail "no song.uni was written"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

BLOB="$HOME_DIR/song.uniproj.state/t0_d0.bin"
[ -s "$BLOB" ] || \
  fail "the loose save wrote no state blob at $BLOB, so there was nothing for the module to pack
        and the rest of this check would pass vacuously:
        $(ls "$HOME_DIR/song.uniproj.state" 2>/dev/null | tr '\n' ' ')"
cp "$BLOB" "$HOME_DIR/original.bin"

if command -v unzip >/dev/null 2>&1; then
  LISTING="$(unzip -l "$HOME_DIR/song.uni" 2>&1)"
  [ -n "$LISTING" ] || fail "unzip -l printed NOTHING for song.uni"
  printf '%s' "$LISTING" | grep -q 'project.state/t0_d0.bin' || \
    fail "the module does not contain project.state/t0_d0.bin — the plugin's state is NOT in the
        file, so every synth in this module is silent on the other machine. Listing:
$LISTING"
  # The manifest too: it is the half that is readable WITHOUT the plugin installed, which is the
  # only thing a received module can offer someone who does not own it.
  printf '%s' "$LISTING" | grep -q 'project.state/t0_d0.params.json' || \
    fail "the module carries the opaque blob but not its parameter manifest. Listing:
$LISTING"
  echo "  packs: project.state/t0_d0.bin and its .params.json are inside the module"
else
  echo "  note: unzip not present, skipping the listing check"
fi

# ---- TRAVELS. Move ONLY the .uni, delete everything else, open it elsewhere.
cp "$HOME_DIR/song.uni" "$AWAY_DIR/song.uni"
rm -rf "$HOME_DIR/song.uniproj.state" "$HOME_DIR"/*.uniproj.json
SHM2="/modstate2_$$"
( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM2" \
    DAW_PROJECT_DIR="$AWAY_DIR" \
    ./daw_engine --run-seconds 30 >"$AWAY_DIR/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$AWAY_DIR/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli2() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$AWAY_DIR" "$CLI" "$@"; }
cli2 do load-module song >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  grep -q '"event":"project.module_loaded"' "$AWAY_DIR/eng.log" && break
  sleep 0.25
done
grep '"event":"project.module_loaded"' "$AWAY_DIR/eng.log" | tail -1 | grep -q '"ok":true' || \
  fail "the module did not load on the other machine:
        $(grep -o '\"event\":\"project.module_loaded\"[^}]*' "$AWAY_DIR/eng.log" | tail -1)"

UNPACKED="$AWAY_DIR/song/project.state/t0_d0.bin"
[ -s "$UNPACKED" ] || \
  fail "no state blob at $UNPACKED after the move. The archive prefix is derived from
        pluginStateDirFor(project.json), so the unpacker's generic write-every-entry loop should
        land it exactly where the loaded document looks for it:
        $(ls -R "$AWAY_DIR/song" 2>/dev/null | head -20)"
cmp -s "$HOME_DIR/original.bin" "$UNPACKED" || \
  fail "the state blob changed in transit: $(wc -c < "$HOME_DIR/original.bin" | tr -d ' ') bytes
        packed, $(wc -c < "$UNPACKED" | tr -d ' ') bytes unpacked. Entries are STORED, not
        deflated, precisely so a blob is byte-identical inside the archive"
echo "  travels: the blob unpacked byte-identical with the originals deleted"

# ---- SOUNDS THE SAME. The assertion the whole file exists for. A blob on disk that the engine
# never hands back to the plugin is the same silence as no blob at all.
GOT2="$(readvalue cli2)"
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ "$GOT2" != "NONE" ] || fail "could not read the plugin's parameters after the move"
python3 -c "raise SystemExit(0 if abs(float('$GOT2') - float('$WANT')) < 0.02 else 1)" || \
  fail "the plugin came back at Gain=$GOT2, wanted $WANT (its default is $DEFAULT). The module
        carried the device and not its sound — which is exactly the failure this check exists for,
        and it is invisible from the outside because the project opens and plays"
echo "  sounds the same: Gain is $GOT2 on the other machine, not its default $DEFAULT"

echo "module_state_check: PASS — a module carries the sound of its plugins, not just their names"
