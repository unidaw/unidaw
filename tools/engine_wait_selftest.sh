#!/usr/bin/env bash
# THE WAIT LIBRARY'S OWN DIAGNOSIS IS CORRECT.
#
# tools/lib/engine_wait.sh is sourced by thirty-five checks. It exists to replace a copied loop
# that would wait thirty seconds for an engine that had already exited and then report "the engine
# never loaded" — a statement about the harness wearing the costume of a product failure (task
# #106). Every one of those checks now depends on it to tell them WHY they failed.
#
# So a bug in here does not fail one check, it makes thirty-five of them lie. That is worth its own
# control, and this one costs about a second because none of it needs a real engine: a `sleep` is
# a perfectly good corpse and a text file is a perfectly good log.
#
# FOUR PROPERTIES:
#   DEAD     a pid that is gone with no load in the log fails FAST and says the engine exited.
#            Fast is half the point — the old loop's sin was burning the budget before lying
#   RACED    a pid that is gone but WHOSE LOAD IS IN THE LOG returns success. An engine can load
#            and exit between two iterations of the poll, and reporting "it died before loading"
#            for one that loaded and then finished its life is a new wrong answer in place of the
#            old one. This is the branch that is easy to get wrong and impossible to notice
#   STUCK    a pid that is still ALIVE and has not loaded says so, and says it differently — the
#            reader needs "hung" and "died" to be distinguishable, since they have no common cause
#   COUNTED  wait_for_loads waits for the Nth load, so a check that reloads is not satisfied by
#            the load its engine did at boot
#
#   tools/engine_wait_selftest.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

LIB="$ROOT/tools/lib/engine_wait.sh"
[ -f "$LIB" ] || fail "tools/lib/engine_wait.sh is missing — thirty-five checks source it"

# Runs one case in a SUBSHELL, because the library exits on failure by design. Captures its
# output and its elapsed whole seconds.
run_case() {  # run_case <script-body-file>  -> prints "<exit> <seconds>" then the output
  local body="$1"
  local start end out rc
  start=$(date +%s)
  out="$(bash "$body" 2>&1)"
  rc=$?
  end=$(date +%s)
  printf '%s %s\n' "$rc" "$((end - start))"
  printf '%s\n' "$out"
}

# ---- DEAD. A corpse with an empty log.
: >"$TMP/dead.log"
cat >"$TMP/dead.sh" <<EOF
. "$LIB"
fail() { echo "  FAIL: \$*"; exit 1; }
sleep 0.1 &
CORPSE=\$!
wait \$CORPSE 2>/dev/null
wait_for_boot "$TMP/dead.log" "\$CORPSE" 120
echo "RETURNED-OK"
EOF
DEAD="$(run_case "$TMP/dead.sh")"
DEAD_RC="$(printf '%s' "$DEAD" | head -1 | cut -d' ' -f1)"
DEAD_SECS="$(printf '%s' "$DEAD" | head -1 | cut -d' ' -f2)"
DEAD_OUT="$(printf '%s' "$DEAD" | tail -n +2)"
echo "  dead engine: exit $DEAD_RC after ${DEAD_SECS}s"
[ "$DEAD_RC" != "0" ] || \
  fail "wait_for_boot RETURNED SUCCESS for an engine that had exited without loading. Every
        check that sources this would then carry on against a dead process and fail later on a
        symptom instead"
case "$DEAD_OUT" in
  *"EXITED before its project loaded"*) : ;;
  *) fail "the death was not reported as a death. It said: $DEAD_OUT" ;;
esac
# The budget here is 120 tries = 30 s. Anything near that means it waited the corpse out, which
# is the entire defect this library was written to remove.
[ "$DEAD_SECS" -le 3 ] || \
  fail "it took ${DEAD_SECS}s to notice a process that was ALREADY GONE before the call. The
        whole point is to fail at once rather than burn the budget and then misreport it"

# ---- RACED. The load IS in the log; the process is gone. Must succeed.
printf '%s\n' '{"ts_ms":1,"event":"project.load","name":"x"}' >"$TMP/raced.log"
cat >"$TMP/raced.sh" <<EOF
. "$LIB"
fail() { echo "  FAIL: \$*"; exit 1; }
sleep 0.1 &
CORPSE=\$!
wait \$CORPSE 2>/dev/null
wait_for_boot "$TMP/raced.log" "\$CORPSE" 120
echo "RETURNED-OK"
EOF
RACED="$(run_case "$TMP/raced.sh")"
RACED_RC="$(printf '%s' "$RACED" | head -1 | cut -d' ' -f1)"
RACED_OUT="$(printf '%s' "$RACED" | tail -n +2)"
echo "  loaded-then-exited: exit $RACED_RC"
[ "$RACED_RC" = "0" ] || \
  fail "an engine that LOADED and then exited was reported as a failure. A short --run-seconds
        does exactly this, so every check with a brief engine would start failing for a reason
        that is not a bug. It said: $RACED_OUT"
case "$RACED_OUT" in
  *RETURNED-OK*) : ;;
  *) fail "the raced case exited 0 without reaching the caller's next line: $RACED_OUT" ;;
esac

# ---- STUCK. Alive, no load, a short budget. Must say ALIVE, not dead.
: >"$TMP/stuck.log"
cat >"$TMP/stuck.sh" <<EOF
. "$LIB"
fail() { echo "  FAIL: \$*"; exit 1; }
sleep 30 &
LIVE=\$!
wait_for_boot "$TMP/stuck.log" "\$LIVE" 4
echo "RETURNED-OK"
EOF
STUCK="$(run_case "$TMP/stuck.sh")"
STUCK_RC="$(printf '%s' "$STUCK" | head -1 | cut -d' ' -f1)"
STUCK_OUT="$(printf '%s' "$STUCK" | tail -n +2)"
pkill -P $$ -f "sleep 30" 2>/dev/null || true
echo "  alive but not loading: exit $STUCK_RC"
[ "$STUCK_RC" != "0" ] || fail "a stuck engine was reported as booted"
case "$STUCK_OUT" in
  *"still alive"*) : ;;
  *) fail "a HUNG engine must be described differently from a DEAD one — they have no cause in
           common and the reader is about to go looking. It said: $STUCK_OUT" ;;
esac

# ---- COUNTED. One load in the log, waiting for two.
printf '%s\n' '{"ts_ms":1,"event":"project.load","name":"x"}' >"$TMP/count.log"
cat >"$TMP/count.sh" <<EOF
. "$LIB"
fail() { echo "  FAIL: \$*"; exit 1; }
sleep 30 &
LIVE=\$!
wait_for_loads "$TMP/count.log" "\$LIVE" 2 4 "the second reload"
echo "RETURNED-OK"
EOF
COUNT="$(run_case "$TMP/count.sh")"
COUNT_RC="$(printf '%s' "$COUNT" | head -1 | cut -d' ' -f1)"
COUNT_OUT="$(printf '%s' "$COUNT" | tail -n +2)"
pkill -P $$ -f "sleep 30" 2>/dev/null || true
echo "  one load, waiting for two: exit $COUNT_RC"
[ "$COUNT_RC" != "0" ] || \
  fail "wait_for_loads was satisfied by ONE load while waiting for two. A check that reloads
        would then be satisfied by the load its engine did at boot and would assert against the
        state before its own edit — which passes, and proves nothing"
case "$COUNT_OUT" in
  *"the second reload"*) : ;;
  *) fail "the caller's description of what it was waiting for did not reach the message: $COUNT_OUT" ;;
esac

echo "engine_wait_selftest: PASS — a corpse is diagnosed at once, a load-then-exit is not"
echo "                      mistaken for a death, a hang reads differently from a death, and"
echo "                      wait_for_loads counts"
