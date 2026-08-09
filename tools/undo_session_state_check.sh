#!/usr/bin/env bash
# UNDO APPLIES A DOCUMENT. IT DOES NOT REPLACE THE SESSION.
#
# Undo restores a version by running it through applyDocument — the same function a file load uses.
# That is the right mechanism, and it brought a whole class of bug with it: everything the load
# path does BECAUSE A LOAD REPLACES THE SONG now happens on every undo too.
#
# THE ONE THIS CATCHES, confirmed by probe on 2026-08-07 before it was fixed:
#     do loop --start 1920000000 --end 3840000000
#     BEFORE: loop_start 1920000000, loop_end 3840000000
#     do note ... ; do undo
#     AFTER : loop_start 0,          loop_end 13440000
# Set a loop over two bars, type a note, press Ctrl-Z, and the loop is gone. It was reset to the
# whole arrangement, on every undo, because applyDocument cleared it.
#
# WHY IT CANNOT BE FIXED BY MAKING THE LOOP UNDOABLE: SetLoopRange is deliberately classified as
# NOT mutating the document (transport state, re-derived at load), so the loop is not in a version
# and undo has nothing to restore it from. The loop is SESSION state. The fix was to move the reset
# to loadProjectFromPath — where "a load replaces the song" is actually true — rather than gate it
# with a flag, because applying a document and replacing the session are two different operations
# and only one of them is undo.
#
# BOTH DIRECTIONS ARE ASSERTED, and the second is what makes this a check rather than a rubber
# stamp: a "fix" that simply deleted the reset would pass phase 1 and break File>Open. Phase 2
# proves the reset still happens where it belongs.
#
# THE PROBE SHAPE MATTERS. My first two attempts at this read the wrong accessor and returned
# EMPTY, which looks exactly like "the loop survived". So: the BEFORE read is asserted NON-EMPTY
# before anything is concluded from the AFTER read, and the engine is asserted alive at read time.
# An empty reading is a broken probe, not a passing test.
#
#   tools/undo_session_state_check.sh
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
SHM="/undosession_$$"
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

cp "$ROOT"/presets/projects/generator.uniproj.json "$TMP"/ 2>/dev/null
[ -s "$TMP/generator.uniproj.json" ] || { echo "  FAIL: generator preset missing — this check would assert nothing"; exit 1; }

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 240 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }
# `get transport` — NOT `get state`. The loop is printed by get_transport
# (ui/daw-cli/src/main.rs:288). Reading the wrong verb returns empty, which reads as "unchanged".
loop_range() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" get transport 2>/dev/null \
                 | grep -E 'loop_(start|end)' | tr -d ' \n' || true; }
wait_for_boot "$TMP/eng.log" "$ENG" 80 'UI: command thread started'

after_command "$TMP" cli do load generator --force

# ------------------------------------------------------------------ 1. undo must NOT touch the loop
echo "  [1] undo must not reset the loop region"
after_command "$TMP" cli do loop --start 1920000000 --end 3840000000
BEFORE="$(loop_range)"
if [ -z "$BEFORE" ]; then
  echo "      FAIL: could not read the loop back after setting it — no baseline, so nothing below"
  echo "            means anything. Broken probe, not a passing test."
  fails=$((fails + 1))
elif ! echo "$BEFORE" | grep -q '1920000000'; then
  echo "      FAIL: the loop did not take: $BEFORE"
  fails=$((fails + 1))
else
  # ANCHOR ON THE UNDO BEING VISIBLE BEFORE READING THE LOOP.
  #
  # This phase expects the loop NOT to change, so it cannot wait for a change — and reading straight
  # after `do undo` returns the PRE-UNDO published state, which is the value it wants to see. It
  # therefore PASSED WITH THE BUG PRESENT: the negative control (loop reset restored into
  # applyDocument) did not fire until this wait existed. A check that cannot fail is worse than no
  # check, and only the control exposed it.
  #
  # So: wait for the undo's OWN effect — the note disappearing — to become visible, and only then
  # read the loop. That makes the loop read strictly after the undo has been published.
  notes_now() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" get notes --track 0 2>/dev/null | grep -c '"pitch"' || true; }
  N_BEFORE="$(notes_now)"
  # RESEND UNTIL IT LANDS. The first note after a load is routinely REFUSED for a stale base — the
  # published clip version lags the engine's by one consumer pass (task #120, confirmed on the clip
  # and harmony versions both). Sending once gave "the note never became visible (1 -> 1)", which
  # is a refusal wearing the costume of a broken probe. Each resend re-reads the version.
  N_WITH="$N_BEFORE"
  for _ in $(seq 1 10); do
    after_command "$TMP" cli do note --track 0 --pitch 60 --nanotick 0 --duration 480000000
    for _ in 1 2 3 4; do
      N_WITH="$(notes_now)"
      [ "${N_WITH:-0}" -gt "${N_BEFORE:-0}" ] && break
      sleep 0.25
    done
    [ "${N_WITH:-0}" -gt "${N_BEFORE:-0}" ] && break
  done
  if [ "${N_WITH:-0}" -le "${N_BEFORE:-0}" ]; then
    echo "      FAIL: the note never became visible ($N_BEFORE -> $N_WITH), so the undo below has"
    echo "            nothing to undo and this phase would assert nothing."
    fails=$((fails + 1))
  fi
  after_command "$TMP" cli do undo
  for _ in $(seq 1 40); do
    [ "$(notes_now)" -le "${N_BEFORE:-0}" ] && break
    sleep 0.25
  done
  AFTER="$(loop_range)"
  if ! kill -0 "$ENG" 2>/dev/null; then
    echo "      FAIL: the engine died during the phase — the reading is void"
    fails=$((fails + 1))
  elif [ "$BEFORE" != "$AFTER" ]; then
    echo "      FAIL: undo changed the loop region."
    echo "            before: $BEFORE"
    echo "            after : $AFTER"
    echo "            applyDocument is doing something that only belongs to a FILE LOAD. The loop is"
    echo "            session state — SetLoopRange is non-mutating, so undo cannot restore it."
    fails=$((fails + 1))
  else
    echo "      ok — the loop survived undo ($AFTER)"
  fi
fi

# ------------------------------------------------------------- 2. a real load MUST reset the loop
# Without this, deleting the reset outright would pass phase 1 and silently break File>Open.
echo "  [2] a real load must still reset the loop"
after_command "$TMP" cli do loop --start 960000000 --end 1920000000
# WAIT FOR THE PUBLISH, do not assume it. SetLoopRange is transport state and does not journal an
# edit, so after_command has nothing to wait ON here — the read raced ahead of the publish and saw
# the PREVIOUS loop, which made this phase report "could not establish a hand-set loop" against a
# loop that had in fact been set. Fourth time today that reading before the consumer published
# looked like a product bug; see the memory note complete-before-visible.
MID=""
for _ in $(seq 1 40); do
  MID="$(loop_range)"
  echo "$MID" | grep -q '960000000' && break
  sleep 0.25
done
after_command "$TMP" cli do load generator --force
# AND WAIT FOR THE PUBLISH HERE TOO. Reading straight after the load saw the PREVIOUS loop and
# reported "the loop survived a project LOAD" against a load that had in fact reset it. The manual
# probe that first confirmed this fix only got the right answer because it happened to sleep.
#
# Waiting for the value to CHANGE FROM MID rather than for a specific number: the reset derives
# loop_end from the arrangement, so hardcoding it here would be a second copy of a rule the engine
# owns — and it would go stale the day the preset's length changes.
RELOADED="$MID"
for _ in $(seq 1 40); do
  RELOADED="$(loop_range)"
  [ "$RELOADED" != "$MID" ] && break
  sleep 0.25
done
if [ -z "$MID" ] || ! echo "$MID" | grep -q '960000000'; then
  echo "      FAIL: could not establish a hand-set loop before the load ($MID)"
  fails=$((fails + 1))
elif [ "$MID" = "$RELOADED" ]; then
  echo "      FAIL: the loop survived a project LOAD — it should not."
  echo "            $RELOADED"
  echo "            A load replaces the song, so a hand-set loop belonged to the OLD one. If phase 1"
  echo "            passes and this fails, the reset was DELETED rather than moved."
  fails=$((fails + 1))
else
  echo "      ok — the load reset the loop ($RELOADED)"
fi

if [ "$fails" -ne 0 ]; then
  echo "undo_session_state_check: FAIL ($fails)"
  exit 1
fi
echo "undo_session_state_check: PASS — undo applies the document without replacing the session"
