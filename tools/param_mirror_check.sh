#!/usr/bin/env bash
# A KNOB TURN MUST REACH THE ENGINE, NOT ONLY THE HOST PROCESS.
#
# handleSetDeviceParam forwarded the value over the control socket to the plugin host and recorded
# it NOWHERE ELSE (task #117). The host stores it, the plugin sounds right, and the engine never
# knows — so the value was lost on host restart, absent from a save, and invisible to undo.
#
# WHY THIS BLOCKS UNDO SPECIFICALLY, and why it is stage 5's first task rather than a part of it:
# undo restores a captured document, and capture reads paramMirror. A value that never lands in
# that map cannot be restored no matter how complete the rest of the undo machinery is. UNDO
# CANNOT RESTORE WHAT WAS NEVER RECORDED.
#
# WHAT THIS ASSERTS, through the command wire rather than the internals:
#   1. a SetDeviceParam that the engine forwards is also MIRRORED
#   2. the mirrored value survives long enough to be read back — i.e. it is engine state, not a
#      message that passed through
#
# WHY IT LOOKS AT THE EVENT AND NOT THE MAP: paramMirror is engine-internal with no read-back
# opcode, so a black-box check cannot query it. device.set_param now reports `mirrored`, which is
# the same trick used for undo.version_recorded — make the invariant observable rather than
# assert it in a comment. If a read-back opcode ever exists, tighten this to compare values.
#
#   tools/param_mirror_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
SHM="/parammirror_$$"
ENG=""
fails=0
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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

# rack has a real device chain, which is what a param write needs to address.
# THE FIXTURE IS DISCOVERED, NOT NAMED. This used to copy `rack` because rack shipped an
# Identity instrument; a branch merge replaced its chain with a sampler and this check began
# reporting "no parameters", which reads as an engine bug and is a fixture bug. Ask which
# shipped preset actually declares a hosted plugin — see preset_with_hosted_plugin.
PRESET="$(preset_with_hosted_plugin "$ROOT" || true)"
if [ -z "$PRESET" ]; then
  echo "  FAIL: no shipped preset declares a hosted VST, so this check cannot address a real"
  echo "        plugin. That is a FIXTURE problem, not an engine one — do not read it as a"
  echo "        pass or as a defect in the code under test."
  exit 1
fi
cp "$ROOT/presets/projects/$PRESET.uniproj.json" "$TMP"/ 2>/dev/null
[ -s "$TMP/$PRESET.uniproj.json" ] || { echo "  FAIL: could not stage preset $PRESET"; exit 1; }
echo "  fixture: $PRESET (the first shipped preset with a hosted plugin)"

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 180 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
cli_out() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

after_command "$TMP" cli do load "$PRESET" --force \
  || { echo "  FAIL: the rack preset never loaded — nothing below addresses a real device"; exit 1; }

# The device id comes from the ENGINE, not from a guess: a hardcoded id that no longer exists
# would make every assertion below vacuous (found=false, and "not mirrored" for the wrong reason).
# BOTH THE DEVICE AND THE PARAM COME FROM THE ENGINE. `do set-param` is positional —
# <track> <device> <uid16hex> <milli> — and a hardcoded uid16 would write a param the plugin does
# not have, which the handler cannot detect (the host owns the param namespace). The write would
# still be "mirrored" and the check would pass while testing nothing real.
#
# THE DEVICE ID IS DISCOVERED TOO, and it used to be `DEVICE_ID=0` sitting directly under that
# paragraph — a guess, under a comment denying there was one. Slot 0 happens to be the plugin in
# rack today; reorder the preset's chain and the check would address an empty slot, report "no
# parameters", and the failure would name the fixture instead of the mirror.
#
# Ask the ENGINE which slot answers with parameters: the first one that does is a real device with
# a real param namespace, which is the only property this check needs of it.
# THE TRACK IS DISCOVERED TOO, not assumed to be 0. `demo` carries its plugin on a later track,
# and scanning only track 0 reported "no parameters" — a fixture that DOES host a plugin, declared
# unusable because the search was too narrow. Ask the engine across both axes.
TRACK_ID=""
DEVICE_ID=""
UID16=""
for _ in $(seq 1 20); do
  for t in 0 1 2 3 4 5 6 7; do
    for candidate in 0 1 2 3 4 5 6 7; do
      UID16="$(cli_out get device-params "$t" "$candidate" 2>/dev/null \
               | grep -oE '"uid16": "[0-9a-fA-F]{32}"' | grep -oE '[0-9a-fA-F]{32}' | head -1 || true)"
      if [ -n "$UID16" ]; then TRACK_ID="$t"; DEVICE_ID="$candidate"; break; fi
    done
    [ -n "$UID16" ] && break
  done
  [ -n "$UID16" ] && break
  sleep 0.25
done
DEVICE_ID="${DEVICE_ID:-0}"
if [ -z "$UID16" ]; then
  echo "  FAIL: no parameters reported for any device on any track, so there is nothing real"
  echo "        to write. Either the plugin did not load or the params read-back is empty — both"
  echo "        are fixture problems, and neither says anything about the mirror."
  fails=$((fails + 1))
else
  echo "  addressing device $DEVICE_ID on track $TRACK_ID, param ${UID16:0:8}..."
  after_command "$TMP" cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" 750 \
    || { echo "  FAIL: set-param was refused or never journalled. Every assertion below reads the"; \
         echo "        log of a command that did not run, so a PASS would mean nothing."; exit 1; }

  LINE="$(grep -o '"event":"device.set_param"[^}]*' "$TMP/eng.log" 2>/dev/null | tail -1 || true)"
  if [ -z "$LINE" ]; then
    echo "  FAIL: no device.set_param event — the command never reached the handler, so nothing"
    echo "        below is a statement about the mirror."
    fails=$((fails + 1))
  elif echo "$LINE" | grep -q '"found":false'; then
    echo "  FAIL: the handler did not resolve the device — the param write was addressed at"
    echo "        nothing, which is a fixture problem rather than a mirror problem."
    echo "        $LINE"
    fails=$((fails + 1))
  elif echo "$LINE" | grep -q '"mirrored":false'; then
    echo "  FAIL: the value was resolved but NOT written to the engine's paramMirror."
    echo "        $LINE"
    echo "        The host has it and the engine does not: lost on host restart, absent from a"
    echo "        save, and invisible to undo — capture reads this map."
    fails=$((fails + 1))
  elif ! echo "$LINE" | grep -q '"mirrored":true'; then
    echo "  FAIL: device.set_param carries no 'mirrored' field. The invariant is unobservable"
    echo "        again, which is how #117 survived in the first place."
    echo "        $LINE"
    fails=$((fails + 1))
  else
    echo "  ok — the write was resolved and mirrored into engine state"
  fi
fi

if [ "$fails" -ne 0 ]; then
  echo "param_mirror_check: FAIL ($fails)"
  exit 1
fi
echo "param_mirror_check: PASS — a knob turn reaches the engine, not only the host"
