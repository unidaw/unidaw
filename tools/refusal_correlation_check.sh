#!/usr/bin/env bash
# AN EXACT V41 REFUSAL REACHES THE SENDER THAT CAUSED IT — AGAINST A LIVE ENGINE.
#
# The sender CAS-allocates a non-zero command id from `UiCommandOutcomeRegion`, carries it in
# `EventEntry::sampleTime`, and records a pre-send sequence mark. The engine publishes a terminal
# record to the append-only broadcast region after the guard decides, keyed by the complete ticket:
# `(command id, opcode, scope, sent base)`. No consumer cursor exists, so a sidecar drain cannot
# steal this answer from another process.
#
# The CLI accepts success only from an exact Completed record and refusal only from an exact Refused
# record. Absent, torn, duplicate, malformed, overrun, exhausted, or timed-out evidence is
# Indeterminate and exits 4; it is never bucketed with success. This live test forces a stale base
# and requires exit 3, proving the refusal closes through the v41 region rather than through the
# diagnostic UI-out ring or a version-counter inference.
#
# THE REFUSAL IS FORCED, not waited for. `--base` presents a version the caller claims to have read
# earlier; pinning it also suppresses the automatic retry, because a caller who names a base is a
# concurrent author testing staleness and the refusal is the answer they asked for. One completed
# setup handler moves the version, then a write against base 0 is stale by construction.
#
# NEGATIVE CONTROL:
#   cp apps/engine_ui_publish.cpp /tmp/pub.bak
#   # in publishCommandOutcome, replace the command-id store with a literal zero:
#   #   slot.commandId.store(0, std::memory_order_relaxed);
#   cmake --build build --target daw_engine -j8 && bash tools/refusal_correlation_check.sh
#   cp /tmp/pub.bak apps/engine_ui_publish.cpp   # cp, NOT `git checkout --`, which would revert to
#                                                # HEAD and delete any uncommitted work beside it
# Expected: sabotaged exits 4 (Indeterminate) and this gate fails; restored exits 3 with the exact
# refusal. The publisher unit test separately pins every stored identity field and sequence order.
#
# The restore used `cp` from a backup taken before the edit. `git checkout --` would have reverted to
# HEAD and destroyed the uncommitted work sitting beside it in the same tree.
#
#   tools/refusal_correlation_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="$ROOT/build/daw_engine"
CLI="$ROOT/ui/target/debug/daw-cli"
SHM="daw_corr_$$"

[ -x "$ENGINE" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() {
  # `wait` after the kill, so the shell reaps the child quietly. Without it the job-control notice
  # ("Terminated: 15") lands on the terminal AFTER the verdict line, which reads like a failure in
  # ctest output for a check that passed.
  if [ -n "$ENG" ]; then
    kill "$ENG" 2>/dev/null
    wait "$ENG" 2>/dev/null
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

echo "== booting engine (shm $SHM)"
( cd "$ROOT/build" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    ./daw_engine --run-seconds 60 >"$TMP/eng.log" 2>&1 ) &
ENG=$!

# Waits for the thread that dispatches commands, rather than sleeping a guessed interval.
for _ in $(seq 1 60); do
  grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null && break
  kill -0 "$ENG" 2>/dev/null || { echo "engine died during boot"; tail -20 "$TMP/eng.log"; exit 1; }
  sleep 1
done
grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null || {
  echo "engine never came up"; tail -20 "$TMP/eng.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

cli do load maximal --force >/dev/null 2>&1 || { echo "  FAIL: load did not land"; exit 1; }

# ONE COMPLETED WRITE FIRST. Its handler advances the track version, making base 0 stale, and its
# exact terminal also proves the v41 send/read path works before the refusal half begins.
if ! cli do note --track 0 --nanotick 0 --column 0 --pitch 60 >"$TMP/apply.log" 2>&1; then
  echo "  FAIL: the setup write did not receive an exact completion."
  cat "$TMP/apply.log"
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
grep -q '"completed": true' "$TMP/apply.log" || {
  echo "  FAIL: setup write reported no exact completion:"; cat "$TMP/apply.log"
  echo "refusal_correlation_check: FAILED"; exit 1; }
echo "   setup handler completed through the exact outcome region"

echo "== writing against a deliberately stale base"
cli do note --track 0 --nanotick 960000 --column 0 --pitch 62 --base 0 >"$TMP/stale.out" 2>"$TMP/stale.err"
RC=$?
cat "$TMP/stale.err"

# EXIT 3 is an exact Refused record. Exit 4 is conservative Indeterminate evidence; exit 0 is an
# exact Completed record and would be a false success for this deliberately stale command.
if [ "$RC" -eq 0 ]; then
  echo
  echo "  FAIL: the CLI reported SUCCESS for a write against a stale base."
  echo "        A Completed record matched the stale command's full ticket, which is invalid."
  echo "        stdout was: $(cat "$TMP/stale.out")"
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
if [ "$RC" -ne 3 ]; then
  echo "  FAIL: expected exit 3 (refusal reported), got $RC — the command failed for some other"
  echo "        reason and this check proved nothing about correlation."
  echo "refusal_correlation_check: FAILED"
  exit 1
fi
grep -q "REFUSED" "$TMP/stale.err" || {
  echo "  FAIL: exit 3 without the refusal message — the exit code and the report disagree."
  echo "refusal_correlation_check: FAILED"; exit 1; }


echo
echo "refusal_correlation_check: PASS — exact v41 ticket matched the engine's refusal"
