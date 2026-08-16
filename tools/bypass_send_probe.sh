#!/usr/bin/env bash
# A PROBE, NOT A GATE — IT DOES NOT DISCRIMINATE, AND THAT IS WHY IT IS NOT REGISTERED.
#
# It was written to demonstrate AE-P1.2 item 15 and FAILED TO. With the bounded send the follow-up
# command landed in 142 ms; with the blocking send deliberately restored, 146 ms. Those are the same
# number. A check whose negative control passes is measuring nothing, so registering this would add
# a green tick that certifies nothing — the exact failure this suite keeps having to unlearn.
#
# It is kept because the harness is real (it boots an engine, freezes a host mid-playback, and times
# whether the command thread still serves anyone) and because the next person to attack item 15
# should start from a reproduction that is KNOWN not to work, rather than rebuild it and reach the
# same dead end. Named `_probe` rather than `_check` so the registry glob does not select it and
# nobody mistakes it for coverage.
#
# WHY IT PROBABLY FAILS, unresolved: either `do set-bypass` does not reach `applyHostBypassStates`
# on this path, or the frozen host's socket buffer never actually fills, or the send returns for a
# reason not yet found. All three are testable and none was tested. Note one real bug found while
# writing it: the CLI flag is `--bypass 0|1`, and `--on 1` — which this and `tsan_command_hammer.sh`
# both passed — is silently ignored, leaving the value at its default.
#
# THE ORIGINAL REASONING, which is sound as far as it goes and is why the hardening still landed:
#
# `applyHostBypassStates` takes `runtime.controllerMutex` across its whole send loop, and it cannot
# narrow that: the mutex guards the controller's LIFETIME against the restart worker reassigning it,
# so releasing it between sends would trade a stall for a use-after-free.
#
# With the BLOCKING sender, each `sendSetBypass` is bounded only by `SO_SNDTIMEO`, which is **60
# seconds** — set that high deliberately, because loading a plugin like Zebra2 takes 10+ seconds. A
# frozen host fills its socket buffer within a few blocks of playback, and then that send blocks.
# Meanwhile `engine_restart_worker.cpp:103`/`:160` need the same mutex to DROP the dead host, so
# recovery waits behind sends addressed to the host it is trying to drop.
#
# WHAT THIS MEASURES, and why it is not the obvious thing. Timing the `set-bypass` invocation itself
# proves nothing: daw-cli's outcome wait is bounded at ~600 ms and returns `Unknown` regardless, so
# the CLI comes back promptly whether or not the ENGINE is wedged. The question is whether the
# command thread is still serving anyone, so this issues set-bypass against the frozen host and then
# times a SUBSEQUENT, unrelated command end to end. If the command thread is parked in a 60-second
# send, the follow-up cannot land.
#
#   tools/bypass_send_probe.sh [budgetMs]            (default 15000)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/ui/target/debug/daw-cli"
SHM="daw_bypass_$$"
BUDGET_MS="${1:-15000}"

[ -x "$ROOT/build/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
FROZEN=""
cleanup() {
  # THE FROZEN HOST IS RESUMED FIRST AND UNCONDITIONALLY. A SIGSTOPped process left behind holds a
  # socket and an engine slot, and this project has lost days to orphaned host processes.
  [ -n "$FROZEN" ] && kill -CONT "$FROZEN" 2>/dev/null
  if [ -n "$ENG" ]; then
    kill "$ENG" 2>/dev/null
    wait "$ENG" 2>/dev/null
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT

cli() { env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
now_ms() { python3 -c 'import time;print(int(time.time()*1000))'; }
track_count() { cli get tracks 2>/dev/null | grep -c '"track_id":'; }

echo "== booting engine (shm $SHM)"
( cd "$ROOT/build" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    ./daw_engine --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 60); do
  grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null && break
  kill -0 "$ENG" 2>/dev/null || { echo "engine died during boot"; tail -20 "$TMP/eng.log"; exit 1; }
  sleep 1
done
grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null || {
  echo "engine never came up"; tail -20 "$TMP/eng.log"; exit 1; }

cli do load maximal --force >/dev/null 2>&1 || { echo "  FAIL: load did not land"; exit 1; }
# Hosts must actually be up, or freezing one proves nothing about a path that never runs.
for _ in $(seq 1 60); do
  [ "$(grep -c 'host ready for track' "$TMP/eng.log" 2>/dev/null)" -ge 1 ] && break
  sleep 1
done
[ "$(grep -c 'host ready for track' "$TMP/eng.log" 2>/dev/null)" -ge 1 ] || {
  echo "  FAIL: no host came up, so there is nothing to freeze and nothing to prove"
  echo "bypass_send_bounded_check: FAILED"; exit 1; }

cli do play --force >/dev/null 2>&1 \
  || { echo "  FAIL: play did not start, so no ProcessBlock traffic fills the frozen socket"
       echo "bypass_send_bounded_check: FAILED"; exit 1; }

FROZEN="$(pgrep -f "juce_host_process.*_0\.sock" | head -1)"
[ -n "$FROZEN" ] || FROZEN="$(pgrep -f juce_host_process | head -1)"
[ -n "$FROZEN" ] || { echo "  FAIL: found no host process to freeze"; exit 1; }
kill -STOP "$FROZEN" || { echo "  FAIL: could not SIGSTOP host $FROZEN"; exit 1; }
echo "   froze host pid $FROZEN"
# Let the streaming ProcessBlock writes fill that socket's buffer. Until it is full a send does not
# block at all, and the check would pass on an engine with the defect present.
sleep 5

BEFORE=$(track_count)
echo "== set-bypass against the frozen host, then timing an unrelated command"
cli do set-bypass --track 0 --device 0 --bypass 1 >/dev/null 2>&1 &
BYPASS_PID=$!
T0=$(now_ms)
cli do add-track >/dev/null 2>&1
LANDED_MS=0
for _ in $(seq 1 200); do
  if [ "$(track_count)" -gt "$BEFORE" ]; then LANDED_MS=$(( $(now_ms) - T0 )); break; fi
  sleep 0.1
done
wait "$BYPASS_PID" 2>/dev/null
kill -CONT "$FROZEN" 2>/dev/null; FROZEN=""

if [ "$LANDED_MS" -eq 0 ]; then
  echo
  echo "  FAIL: the follow-up command never landed at all while a host was frozen."
  echo "        The command thread is parked, which is what a blocking send under controllerMutex"
  echo "        does — and the restart worker needs that same mutex to drop the dead host."
  echo "bypass_send_bounded_check: FAILED"
  exit 1
fi
echo "   follow-up command landed in ${LANDED_MS} ms"
if [ "$LANDED_MS" -gt "$BUDGET_MS" ]; then
  echo
  echo "  FAIL: ${LANDED_MS} ms exceeds the ${BUDGET_MS} ms budget. With the blocking sender this is"
  echo "        bounded by SO_SNDTIMEO = 60 s per device, and recovery queues behind it."
  echo "bypass_send_bounded_check: FAILED"
  exit 1
fi
echo "bypass_send_bounded_check: PASS — command thread still served a request in ${LANDED_MS} ms"
echo "   with a host frozen mid-playback (budget ${BUDGET_MS} ms)"
