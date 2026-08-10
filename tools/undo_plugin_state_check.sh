#!/usr/bin/env bash
# UNDO MUST NOT RE-PUSH SAVED PLUGIN STATE OVER LIVE STATE.
#
# Undo restores a version through applyDocument — the same function a file load uses — so
# everything that path does BECAUSE A LOAD REPLACES THE SONG happened on every undo too. Pushing
# the saved plugin blobs was one of those things, and it is the worst of them, because plugin
# state is NOT in the document:
#
#   Device has no params field. The blobs live in <project>/.state/*.bin and are fetched from the
#   hosts at SAVE time via requestPluginState. So an undo that re-pushes them is not restoring
#   anything — it is OVERWRITING LIVE STATE WITH LAST-SAVED STATE.
#
# The user-visible bug (review finding #123 item 6): tweak a plugin's cutoff, do not save, type a
# note, press Ctrl-Z. The note comes back AND the plugin snaps to whatever was on disk at the last
# save. Undo silently discards unsaved plugin work.
#
# WHAT THIS ASSERTS, and it is directly observable rather than inferred: the engine emits
# project.state_restored once per blob it pushes. A file LOAD must emit them; an UNDO must emit
# NONE. Counting them across the two operations answers the question exactly, with no need to read
# plugin state back out of a host.
#
# THE FIX THIS GUARDS is a MOVE, not a flag: restorePluginStateFromDisk now lives in
# loadProjectFromPath, where "a load replaces the session" is actually true. applyDocument only
# applies the document. Same shape as the loop-region fix in 53b77d5 — a bool parameter would have
# hidden the distinction behind something nobody reads.
#
# THIS DID NOT BECOME FALSE WHEN UNDO STARTED RESTORING PLUGINS (stage 5), and the distinction is
# the whole point. Undo now pushes the blobs held by the VERSION — the state the plugin actually
# had at that step, captured live — and reports them as undo.plugin_state_pushed. It must still
# never push the blobs sitting on DISK, which are last-SAVED state and would discard unsaved
# plugin work exactly as before. Two sources, opposite requirements, two events: this check counts
# project.state_restored, and tools/undo_plugin_version_check.sh counts the other. Both must hold.
#
#   tools/undo_plugin_state_check.sh
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
SHM="/undopstate_$$"
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

# rack has real VST devices, which is what makes state blobs exist at all.
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
n_restored() { grep -c '"event":"project.state_restored"' "$TMP/eng.log" 2>/dev/null || true; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

must() {
  local what="$1"; shift
  after_command "$TMP" "$@" && return 0
  echo "  FAIL: $what was refused or never journalled."
  echo "        Everything after it counts events from a command that did not run, so the"
  echo "        comparison would report PASS having exercised nothing."
  exit 1
}

must "the preset load" cli do load "$PRESET" --force
# SAVE FIRST, so blobs exist on disk to be wrongly re-pushed. Without this the check passes
# vacuously: nothing to push means no push to detect, and the bug would be invisible.
must "the save" cli do save "$PRESET" --force
must "the reload" cli do load "$PRESET" --force

AFTER_LOAD="$(n_restored)"
echo "  a load restored ${AFTER_LOAD:-0} plugin state blob(s)"

# ---------------------------------------------------------------- undo must push NONE of them
must "the note write" cli do note --track 0 --pitch 60 --nanotick 0 --duration 480000000
must "the undo" cli do undo

# AND THE UNDO MUST HAVE ACTUALLY APPLIED A VERSION. This is the vacuous path the check was
# written to avoid and left open anyway: if `do undo` is refused, or finds nothing to return to,
# AFTER_UNDO equals AFTER_LOAD and the comparison below reports PASS having exercised nothing.
# "No blobs were pushed" is only a statement about undo if an undo happened.
if ! grep -q '"event":"undo.applied"[^}]*"ok":true' "$TMP/eng.log" 2>/dev/null; then
  echo "  FAIL: no undo.applied ok:true in the log. The undo did not restore a version, so the"
  echo "        blob count below compares nothing against nothing."
  grep -o '"event":"undo\.[a-z_]*"[^}]*' "$TMP/eng.log" 2>/dev/null | tail -5
  exit 1
fi

# The consumer applies an undone document on its next pass, so give the push (if any) time to
# happen before concluding it did not. Waiting for a NON-event needs a bound, not an anchor.
sleep 1.5
AFTER_UNDO="$(n_restored)"

if ! kill -0 "$ENG" 2>/dev/null; then
  echo "  FAIL: the engine died during the phase — the reading is void"
  fails=$((fails + 1))
elif [ "${AFTER_UNDO:-0}" -gt "${AFTER_LOAD:-0}" ]; then
  echo "  FAIL: undo pushed $(( AFTER_UNDO - AFTER_LOAD )) plugin state blob(s) from disk."
  echo "        Plugin state is NOT in the document, so this is not restoring anything — it is"
  echo "        overwriting LIVE state with LAST-SAVED state. Tweak a cutoff, do not save, type a"
  echo "        note, undo: the plugin snaps back to disk and unsaved work is gone."
  fails=$((fails + 1))
else
  echo "  ok — undo pushed no plugin state (${AFTER_LOAD:-0} -> ${AFTER_UNDO:-0})"
fi

# ------------------------------------------------------- and a real load must STILL push them
# Without this, deleting the restore outright would pass the assertion above and silently break
# File>Open — the same trap the loop-region check guards against.
if [ "${AFTER_LOAD:-0}" -eq 0 ]; then
  echo "  FAIL: a project LOAD restored no plugin state at all. Either the fixture has no VST"
  echo "        blobs — in which case the assertion above proves nothing — or the restore was"
  echo "        DELETED rather than moved."
  fails=$((fails + 1))
else
  echo "  ok — a load still restores plugin state"
fi

if [ "$fails" -ne 0 ]; then
  echo "undo_plugin_state_check: FAIL ($fails)"
  exit 1
fi
echo "undo_plugin_state_check: PASS — undo applies the document without re-pushing saved plugin state"
