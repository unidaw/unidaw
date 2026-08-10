#!/usr/bin/env bash
# UNDOING A KNOB TURN MUST PUT THE PLUGIN BACK — and undoing a note must leave it alone.
#
# Undo stage 5. A version now carries each hosted plugin's own opaque state alongside the
# document, because the document does not contain it: turn a cutoff and the serialized project is
# byte-identical, so before this the engine classified the single most common plugin edit as "the
# command changed nothing", recorded no version, and Ctrl-Z stepped straight over it.
#
# TWO ASSERTIONS, AND THE SECOND IS THE ONE THAT KEEPS THE FIRST HONEST:
#
#   1. Undo a SetDeviceParam -> the engine pushes plugin state back to the host.
#   2. Undo a NOTE WRITE     -> it pushes NOTHING.
#
# Without (2) the whole thing could be "push every plugin on every undo", which passes (1) while
# being both slow and audible — plugins reset voices and cut tails on setState, so an unrelated
# undo would click. (2) is what proves the comparison in restorePluginSnapshot is real.
#
# NOT THE SAME QUESTION AS undo_plugin_state_check.sh, and they must both hold. That one asserts
# undo never re-pushes the blobs SAVED ON DISK (project.state_restored), because those are
# last-saved state and pushing them discards unsaved plugin work. This one asserts undo DOES push
# the blobs held by the VERSION (undo.plugin_state_pushed), which is the state the plugin actually
# had at that step. Different sources, opposite requirements, two events.
#
#   tools/undo_plugin_version_check.sh
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
SHM="/undopver_$$"
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

# rack has real VST devices — the only fixture where a plugin has state to version at all.
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
n_pushed() { grep -c '"event":"undo.plugin_state_pushed"' "$TMP/eng.log" 2>/dev/null || true; }
n_undone() { grep -c '"event":"undo.applied"[^}]*"ok":true' "$TMP/eng.log" 2>/dev/null || true; }
must() {
  local what="$1"; shift
  after_command "$TMP" "$@" && return 0
  echo "  FAIL: $what was refused or never journalled — every count below would be of a command"
  echo "        that did not run, so a PASS would mean nothing."
  exit 1
}
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

must "the preset load" cli do load "$PRESET" --force

# THE DEVICE AND THE PARAM BOTH COME FROM THE ENGINE. A hardcoded slot addresses whatever happens
# to be there, and a hardcoded uid16 writes a param the plugin does not have — the handler cannot
# tell (the host owns the param namespace), so the write would look fine and mean nothing.
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
  echo "  FAIL: no plugin parameters on any slot of any track. Either no VST loaded or the params"
  echo "        read-back is empty — either way there is no plugin state to version, and every"
  echo "        assertion below would be about nothing."
  exit 1
fi
echo "  addressing device $DEVICE_ID on track $TRACK_ID, param ${UID16:0:8}..."

# ------------------------------------------------------------------ 1. undo a knob turn
must "the param write" cli do set-param "$TRACK_ID" "$DEVICE_ID" "$UID16" 250
BEFORE_PARAM_UNDO="$(n_pushed)"
UNDOS_BEFORE="$(n_undone)"
must "the undo of the param write" cli do undo
# A push happens on the command thread inside handleUndo, but the event lands on a ring another
# thread drains. Wait for the undo to be REPORTED, then give the push its bound.
for _ in $(seq 1 40); do
  [ "$(n_undone)" -gt "${UNDOS_BEFORE:-0}" ] && break
  sleep 0.25
done
if [ "$(n_undone)" -le "${UNDOS_BEFORE:-0}" ]; then
  echo "  FAIL: no undo.applied ok:true — the undo did not restore a version, so nothing below is"
  echo "        a statement about plugin state."
  grep -o '"event":"undo\.[a-z_]*"[^}]*' "$TMP/eng.log" 2>/dev/null | tail -5
  exit 1
fi
for _ in $(seq 1 20); do
  [ "$(n_pushed)" -gt "${BEFORE_PARAM_UNDO:-0}" ] && break
  sleep 0.25
done
AFTER_PARAM_UNDO="$(n_pushed)"

if [ "${AFTER_PARAM_UNDO:-0}" -le "${BEFORE_PARAM_UNDO:-0}" ]; then
  echo "  FAIL: undoing a knob turn pushed NO plugin state back to the host."
  echo "        The version holds the plugin's pre-turn blob and undo did not send it, so the"
  echo "        document went back and the plugin stayed where the user left it — a state that"
  echo "        never existed. Undo is partial and says nothing about it."
  grep -o '"event":"undo\.[a-z_]*"[^}]*' "$TMP/eng.log" 2>/dev/null | tail -6
  fails=$((fails + 1))
else
  echo "  ok — undoing a param change pushed $(( AFTER_PARAM_UNDO - BEFORE_PARAM_UNDO )) plugin blob(s) back"
fi

# ------------------------------------------------- 2. and undoing a NOTE must push nothing
must "the note write" cli do note --track 0 --pitch 60 --nanotick 0 --duration 480000000
BEFORE_NOTE_UNDO="$(n_pushed)"
UNDOS_BEFORE="$(n_undone)"
must "the undo of the note" cli do undo
for _ in $(seq 1 40); do
  [ "$(n_undone)" -gt "${UNDOS_BEFORE:-0}" ] && break
  sleep 0.25
done
if [ "$(n_undone)" -le "${UNDOS_BEFORE:-0}" ]; then
  echo "  FAIL: the note undo never applied, so 'it pushed nothing' is not evidence of anything."
  exit 1
fi
# Waiting for a NON-event needs a bound rather than an anchor: give any push time to appear.
sleep 1.5
AFTER_NOTE_UNDO="$(n_pushed)"

if ! kill -0 "$ENG" 2>/dev/null; then
  echo "  FAIL: the engine died during the phase — the reading is void"
  fails=$((fails + 1))
elif [ "${AFTER_NOTE_UNDO:-0}" -gt "${BEFORE_NOTE_UNDO:-0}" ]; then
  echo "  FAIL: undoing a NOTE pushed $(( AFTER_NOTE_UNDO - BEFORE_NOTE_UNDO )) plugin blob(s)."
  echo "        A note edit changes no plugin state, so the version's blobs are the ones the host"
  echo "        already holds and this should send nothing. Pushing anyway is not merely wasteful:"
  echo "        plugins reset voices and cut tails on setState, so an unrelated undo clicks."
  fails=$((fails + 1))
else
  echo "  ok — undoing a note pushed no plugin state (${BEFORE_NOTE_UNDO:-0} -> ${AFTER_NOTE_UNDO:-0})"
fi

if [ "$fails" -ne 0 ]; then
  echo "undo_plugin_version_check: FAIL ($fails)"
  exit 1
fi
echo "undo_plugin_version_check: PASS — a version carries the plugins, and only what changed is pushed"
