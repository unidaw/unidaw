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
cp "$ROOT"/presets/projects/rack.uniproj.json "$TMP"/ 2>/dev/null
[ -s "$TMP/rack.uniproj.json" ] || { echo "  FAIL: rack preset missing — this check would assert nothing"; exit 1; }

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 240 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
n_restored() { grep -c '"event":"project.state_restored"' "$TMP/eng.log" 2>/dev/null || true; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

after_command "$TMP" cli do load rack --force
# SAVE FIRST, so blobs exist on disk to be wrongly re-pushed. Without this the check passes
# vacuously: nothing to push means no push to detect, and the bug would be invisible.
after_command "$TMP" cli do save rack --force
after_command "$TMP" cli do load rack --force

AFTER_LOAD="$(n_restored)"
echo "  a load restored ${AFTER_LOAD:-0} plugin state blob(s)"

# ---------------------------------------------------------------- undo must push NONE of them
after_command "$TMP" cli do note --track 0 --pitch 60 --nanotick 0 --duration 480000000
after_command "$TMP" cli do undo
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
