#!/usr/bin/env bash
# A KNOB DRAG IS ONE UNDO STEP — driven through the real wire, with the real flags.
#
# THE GAP THIS FILLS, and it is the gap that let a broken force-close reach main.
# gesture_undo_tests_main.cpp drives DocumentHistory DIRECTLY: beginGesture, commit, amend. It
# pins the SEMANTICS and says so in its own header — "the WIRING is not covered". The wiring is
# the RecordVersion bracket in engine_handle_ui_entry.cpp, and that is precisely where the defect
# was: the force-close fired on ANY mutating command carrying neither flag, and a drag's own
# middle moves carry neither. So the first move closed the gesture the pointerdown had opened,
# every sample committed its own version, and coalescing was unreachable by any UI following the
# contract. Frontend measured it on a real drag: 8 sends, 8 versions.
#
# Nothing in the suite could see it, because nothing in the suite sent a flag. daw-cli grew
# `--gesture begin|end|both` for exactly this, and now something does.
#
# WHAT IT ASSERTS
#   1. BEGIN + N bare moves + END records ONE version.
#   2. THE CONTROL: the same N+2 sends with NO flags records MORE than one. Without this, an
#      engine that recorded nothing at all — a refused command, a dead device, a typo in the uid16
#      — would pass assertion 1 perfectly.
#   3. The undo that follows lands on the PRE-DRAG value, not on the second-to-last sample.
#
# WHY IT COUNTS undo.version_recorded RATHER THAN READING THE PARAM BACK: the engine emits that
# line once per version actually pushed, which is the exact quantity in dispute. A param read-back
# would answer "where did the knob end up", which was never the question.
#
#   tools/gesture_drag_check.sh
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
SHM="/gesturedrag_$$"
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
    ./daw_engine --run-seconds 240 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
cli_out() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" 2>/dev/null; }
n_versions() { grep -c '"event":"undo.version_recorded"' "$TMP/eng.log" 2>/dev/null || true; }
must() {
  local what="$1"; shift
  after_command "$TMP" "$@" && return 0
  echo "  FAIL: $what was refused or never journalled, so every count below is of a command that"
  echo "        did not run and a PASS would mean nothing."
  exit 1
}
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

must "the preset load" cli do load "$PRESET" --force

# Device and param both discovered from the engine: a hardcoded uid16 writes a param the plugin
# does not have, which the handler cannot detect, and the drag would coalesce nothing real.
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
if [ -z "$UID16" ]; then
  echo "  FAIL: no plugin parameters on any slot of any track — nothing to drag, so this check would"
  echo "        assert nothing about coalescing."
  exit 1
fi
echo "  dragging device $DEVICE_ID on track $TRACK_ID, param ${UID16:0:8}..."

# ------------------------------------------------------------------ 1. the drag, with flags
BEFORE="$(n_versions)"
must "the drag's BEGIN"  cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" 200 --gesture begin
for v in 250 300 350 400 450; do
  must "a mid-drag move ($v)" cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" "$v"
done
must "the drag's END"    cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" 500 --gesture end
AFTER="$(n_versions)"
DRAG_VERSIONS=$(( AFTER - BEFORE ))

if [ "$DRAG_VERSIONS" -ne 1 ]; then
  echo "  FAIL: a 7-command drag recorded $DRAG_VERSIONS versions; it must record exactly 1."
  echo "        Every sample of a knob drag is becoming its own undo step, so one gesture costs"
  echo "        the user seven presses of Ctrl-Z and the first lands on a value they never chose."
  echo "        If this is 7, the force-close is firing on the drag's own middle moves — the"
  echo "        arriving command's TYPE must be compared against the type that opened the gesture,"
  echo "        not merely tested for carrying no flags."
  grep -o '"event":"undo\.[a-z_]*"[^}]*' "$TMP/eng.log" 2>/dev/null | tail -10
  fails=$((fails + 1))
else
  echo "  ok — a 7-command drag recorded exactly 1 version"
fi

# ------------------------------------------- 2. THE CONTROL: the same sends WITHOUT the flags
# An engine recording nothing at all would sail through the assertion above. This is what tells
# "coalesced to one" apart from "did not happen".
BEFORE_CTL="$(n_versions)"
for v in 210 260 310 360 410 460 510; do
  must "an unflagged send ($v)" cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" "$v"
done
AFTER_CTL="$(n_versions)"
CTL_VERSIONS=$(( AFTER_CTL - BEFORE_CTL ))

if [ "$CTL_VERSIONS" -le 1 ]; then
  echo "  FAIL: seven UNFLAGGED param writes recorded $CTL_VERSIONS version(s). They must record"
  echo "        one each — seven separate edits are seven undo steps. Recording one or none means"
  echo "        the engine is coalescing commands nobody bracketed, or recording nothing at all,"
  echo "        and either way the assertion above passed for the wrong reason."
  fails=$((fails + 1))
else
  echo "  ok — the control: seven unflagged writes recorded $CTL_VERSIONS versions"
fi

# ----------------------------------------------------- 3. and undo lands BEFORE the whole drag
if ! kill -0 "$ENG" 2>/dev/null; then
  echo "  FAIL: the engine died during the phase — every reading above is void"
  fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
  echo "gesture_drag_check: FAIL ($fails)"
  exit 1
fi
echo "gesture_drag_check: PASS — one drag is one undo step, and unbracketed writes still are not"
