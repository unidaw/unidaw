#!/usr/bin/env bash
# FOUR THINGS UNDO MUST NOT DO, each of which it DID until 2026-08-06.
#
# undo_ratchet_check answers "how many commands are undoable" — a count over the enum. It cannot
# see any of these, because every one of them needs a SEQUENCE: an edit that gets refused, a
# session that never loaded a project, a command that only exists inside an envelope, a directory
# that is read long after the undo that blanked it. All four were found by an adversarial review
# panel rather than by a check, which is the gap this file closes.
#
#   1. A REFUSED COMMAND MUST NOT DESTROY REDO.
#      The recording bracket fires on every mutating opcode, whether or not the handler agreed to
#      do anything — and commit() truncates everything ahead of the cursor. So "undo, then send a
#      command the engine refuses" (a stale baseVersion is the everyday case) threw away the redo
#      tail and gave nothing back for it. Silent, and it looks like the user's own mistake.
#
#   2. THE FIRST EDIT OF A BARE SESSION MUST BE UNDOABLE.
#      seed() had one caller, inside loadProjectFromPath. With no project loaded there was no
#      version 0, so commit()'s empty branch recorded the POST-edit document as the base and the
#      first edit could never be undone. Invisible to every other check in the tree, because they
#      all `do load` before they edit — which is exactly why this one must not.
#
#   3. A COMMAND THAT ARRIVES ONLY IN AN ENVELOPE MUST STILL BE RECORDED.
#      SetClipText, SamplerSetSlotName and SamplerSetEnvelopePoints are too big for the 40-byte
#      command payload and can ONLY arrive as BulkChunk. The bracket read the OUTER opcode, which
#      is the carrier and changes nothing, so all three were classified undoable and recorded
#      nothing. Draw an envelope, undo, and it is gone with no version to redo from.
#
#   4. UNDO MUST NOT MOVE THE PROJECT DIRECTORY.
#      applyDocument derives loadedProjectDir and the plugin state directory from its `path`
#      argument. Undo has no file to name and passed an empty string, so parent_path("") blanked
#      the directory: relative sample paths began resolving against the engine's working
#      directory, and pluginStateDirFor("") is "./.state", restoring plugins at FACTORY state.
#
#   tools/undo_invariants_check.sh
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
fails=0

cleanup_engine() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; ENG=""; }
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  cleanup_engine
  rm -rf "$TMP"
  exit $rc
}
trap 'keep_evidence_then' EXIT

# Each phase gets its OWN engine on its OWN segment. Phase 2 is meaningless if a previous phase
# left a project loaded, and sharing one engine would make that the default rather than a mistake.
start_engine() {  # start_engine <shm-suffix> <logfile> [extra env...]
  ( cd "$BUILD" && exec env DAW_UI_SHM_NAME="/undoinv_$$_$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 240 >"$2" 2>&1 ) &
  ENG=$!
  SHM_SUFFIX="$1"
  wait_for_boot "$2" "$ENG" 80 'UI: command thread started'
}
cli() { env DAW_UI_SHM_NAME="/undoinv_$$_$SHM_SUFFIX" DAW_PROJECT_DIR="$TMP" "$CLI" "$@" >/dev/null 2>&1; }

# COUNTED, NOT GREPPED FOR PRESENCE. "a version was recorded" is a number that must move by a
# known amount; `grep -q` would pass on a version recorded by some earlier step.
n_recorded() { grep -c '"event":"undo.version_recorded"' "$1" 2>/dev/null || true; }
n_unchanged() { grep -c '"event":"undo.version_unchanged"' "$1" 2>/dev/null || true; }

# SEND UNTIL THE ENGINE ACCEPTS, BOUNDED — and fail loudly if it never does.
#
# The published per-track clip version lags the engine's own after a load, so a client that stamps
# its base from the published state (which is every client, including daw-cli) gets refused. That
# lag is a real defect and is filed separately; here it is a HAZARD, and a check that lets it
# through reports "undo recorded nothing" for a command the engine never ran. Two runs of this
# file disagreed on phases 1 and 4 for exactly that reason before this existed — the classic shape
# of a test that passes or fails on timing while claiming to measure behaviour.
#
# Each send re-reads the version, so this converges the moment the publish catches up.
#   send_until_accepted <op-name-in-journal> <what> <cli args...>
send_until_accepted() {
  local op="$1" what="$2"; shift 2
  local rejected_before attempts=0
  while [ "$attempts" -lt 10 ]; do
    rejected_before="$(grep -c "\"op\":\"$op\",\"outcome\":\"rejected" "$TMP/history.jsonl" 2>/dev/null || true)"
    after_command "$TMP" cli "$@" 2>/dev/null || true
    if [ "$(grep -c "\"op\":\"$op\",\"outcome\":\"rejected" "$TMP/history.jsonl" 2>/dev/null || true)" \
         -eq "$rejected_before" ]; then
      return 0
    fi
    attempts=$((attempts + 1))
    sleep 0.3
  done
  echo "      FAIL: the engine refused '$what' on all 10 attempts (stale base every time), so this"
  echo "            phase never exercised what it claims to. The published per-track clip version"
  echo "            is not catching up with the engine's."
  fails=$((fails + 1))
  return 1
}

# generator is the smallest preset that has BOTH a track to write a note on and a
# named clip (id 4, "seven") for the envelope-only phase to rename.
cp "$ROOT"/presets/projects/generator.uniproj.json "$TMP/" 2>/dev/null

# ---------------------------------------------------------------- 1. a refusal keeps redo alive
echo "  [1] a refused command must not destroy the redo tail"
LOG="$TMP/refuse.log"
start_engine refuse "$LOG"
after_command "$TMP" cli do load generator --force
send_until_accepted write_note "the note that phase 1 undoes" \
  do note --track 0 --pitch 60 --nanotick 0 --duration 480000000
after_command "$TMP" cli do undo

# A DELIBERATELY STALE base-version. This is the ordinary shape of a refusal — two UIs, or one UI
# that acted on a snapshot it had already been passed. The engine must refuse it AND must leave
# the history alone; the point of the phase is that those are two different things.
BEFORE_REDO_LOG="$(grep -c '"event":"redo.applied"' "$LOG" 2>/dev/null || true)"
# --base 1 WITHOUT --retry-stale: the CLI reports the refusal instead of re-reading and
# sending again, which is what makes this a refusal rather than a slow success.
after_command "$TMP" cli do note --track 0 --pitch 62 --nanotick 0 --duration 480000000 \
  --base 1 2>/dev/null || true
after_command "$TMP" cli do redo
AFTER_REDO_LOG="$(grep -c '"event":"redo.applied"' "$LOG" 2>/dev/null || true)"
REDO_NULL="$(grep -c '"event":"redo.picked"[^}]*"null":true' "$LOG" 2>/dev/null || true)"
# THE NULL CASE IS CHECKED FIRST, and the order is the whole diagnosis. handleRedo returns BEFORE
# emitting redo.applied when the history has nowhere to go, so "no redo.applied line" is true of
# both "redo never ran" and "redo ran and found an empty tail" — and only the second is the defect.
# Testing the applied-count first reported the truncation as "redo never ran at all", which sent
# the reader looking at the command path instead of at the history. The negative control caught it.
if [ "${REDO_NULL:-0}" -gt 0 ]; then
  echo "      FAIL: redo found nothing to redo — the refused command truncated the tail."
  echo "            This is the data loss the equality test in DocumentHistory::commit exists to"
  echo "            prevent: a command the engine DECLINED threw away the user's redo."
  fails=$((fails + 1))
elif [ "$AFTER_REDO_LOG" -le "$BEFORE_REDO_LOG" ]; then
  echo "      FAIL: redo never ran at all after the refused command — no redo.applied and no"
  echo "            redo.picked, so the command did not reach handleRedo."
  fails=$((fails + 1))
else
  echo "      ok — redo survived a refused command"
fi
cleanup_engine

# ------------------------------------------------------- 2. the first edit of a bare boot session
echo "  [2] the first edit of a session that never loaded a project must be undoable"
LOG="$TMP/bare.log"
start_engine bare "$LOG"
# NO `do load` HERE, and that is the entire phase. Adding one would make it pass on a bug.
after_command "$TMP" cli do add-track
SEEDED="$(grep -c '"event":"undo.seeded"' "$LOG" 2>/dev/null || true)"
after_command "$TMP" cli do undo
UNDO_NULL="$(grep -c '"event":"undo.picked"[^}]*"null":true' "$LOG" 2>/dev/null || true)"
if [ "${SEEDED:-0}" -eq 0 ]; then
  echo "      FAIL: no undo.seeded event — nothing established version 0 before the first edit,"
  echo "            so commit() recorded the POST-edit document as the base."
  fails=$((fails + 1))
elif [ "${UNDO_NULL:-0}" -gt 0 ]; then
  echo "      FAIL: undo found no earlier version — the first edit of the session is unreachable."
  fails=$((fails + 1))
else
  echo "      ok — the session seeded itself and the first edit undid"
fi
cleanup_engine

# ------------------------------------------------------------ 3. an envelope-only command records
echo "  [3] a command that can only arrive in a bulk envelope must record a version"
LOG="$TMP/bulk.log"
start_engine bulk "$LOG"
after_command "$TMP" cli do load generator --force
# WAIT FOR THE UI TO SEE THE LOAD, not merely for the engine to have acted on it. clip-name reads
# the clip version from the PUBLISHED state to stamp its base, so a journal-only wait let it send
# base 1 against an engine already at 2 — and the engine, correctly, refused. The check then
# reported "no version recorded" for a command that never ran, which is a harness bug wearing the
# costume of the defect under test. This is the history-vs-published distinction: `after_command`
# means the engine acted, `wait_for_published` means a client can read the result.
#
# RETRY UNTIL THE VERSIONS AGREE, BOUNDED. The published per-track clip version lags the engine's
# after a load — clip-name stamped base 1 while the engine held 2 — and clip-name has no
# --retry-stale of its own, so the refusal is permanent from one invocation. Each send re-reads
# the published version, so the loop converges as soon as the publish catches up; the bound is
# what stops it becoming a hang if it never does.
BEFORE="$(n_recorded "$LOG")"
# clip-name sends SetClipText, which is bulk-only: a name does not fit the 40-byte payload.
send_until_accepted set_clip_text "the clip rename" \
  do clip-name --clip 4 --name "renamed by the check"
AFTER="$(n_recorded "$LOG")"
ASSEMBLED="$(grep -c '"event":"bulk.assembled"' "$LOG" 2>/dev/null || true)"
if [ "${ASSEMBLED:-0}" -eq 0 ]; then
  echo "      SKIPPED — no bulk envelope was assembled, so the command never reached the engine"
  echo "               by the path under test. Not a pass: the CLI verb or the clip id is wrong."
  fails=$((fails + 1))
elif [ "$AFTER" -le "$BEFORE" ]; then
  echo "      FAIL: the envelope was assembled and applied, and no version was recorded."
  echo "            The bracket read the CARRIER's opcode (BulkChunk, which mutates nothing)"
  echo "            instead of the opcode the envelope spells."
  fails=$((fails + 1))
else
  echo "      ok — the inner opcode was recorded ($BEFORE -> $AFTER)"
fi
cleanup_engine

# -------------------------------------------------------- 4. undo leaves the project dir in place
echo "  [4] undo must not blank the project directory"
LOG="$TMP/dir.log"
start_engine dir "$LOG"
after_command "$TMP" cli do load generator --force
send_until_accepted write_note "the note that phase 4 undoes" \
  do note --track 0 --pitch 64 --nanotick 0 --duration 480000000
after_command "$TMP" cli do undo
DIR_LINE="$(grep -o '"event":"undo.applied"[^}]*' "$LOG" 2>/dev/null | tail -1 || true)"
if [ -z "$DIR_LINE" ]; then
  echo "      FAIL: no undo.applied event — undo did not run, so this phase asserts nothing"
  fails=$((fails + 1))
elif echo "$DIR_LINE" | grep -q '"project_dir":""'; then
  echo "      FAIL: undo blanked loadedProjectDir."
  echo "            $DIR_LINE"
  echo "            Every relative sample path now resolves against the engine's working"
  echo "            directory, and pluginStateDirFor(\"\") is ./.state — an undone device edit"
  echo "            restores the plugin at factory state."
  fails=$((fails + 1))
else
  echo "      ok — the directory survived the undo"
fi
cleanup_engine

if [ "$fails" -ne 0 ]; then
  echo "undo_invariants_check: FAIL ($fails)"
  exit 1
fi
echo "undo_invariants_check: PASS — refusal, bare boot, envelope and directory all hold"
