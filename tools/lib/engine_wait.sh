# WAIT FOR AN ENGINE TO LOAD ITS PROJECT — AND NOTICE WHEN IT DIED INSTEAD.
#
# Sourced by the checks. It replaces this, which was copied into thirty of them:
#
#   for _ in $(seq 1 120); do
#     grep -q '"event":"project.load"' "$LOG" 2>/dev/null && break
#     sleep 0.25
#   done
#
# THAT LOOP WAITS OUT A CORPSE. Every check launches the engine with a fixed lifetime
# (--run-seconds N) and then waits up to thirty seconds for it to boot. A mechanical sweep found
# 29 of 36 launches where the WAIT BUDGET IS LONGER THAN THE ENGINE'S OWN LIFETIME — worst cases
# 15 s of life against 30 s of waiting. Under a parallel ctest boot is slower, and when it does
# not fit, the engine exits while the script is still counting; the check then burns the rest of
# its budget and reports "the engine never loaded", which is a statement about the harness wearing
# the costume of a product failure.
#
# Every launch already captures its pid, so the loop can simply ask. This fails IMMEDIATELY with
# the true cause and the log's tail, which turns a thirty-second lie into an instant diagnosis.
#
# AND IT ASSERTS. About half the copied loops never checked the outcome at all — they fell through
# to a sleep and carried on, so a failed boot surfaced later as a confusing symptom (a command
# refused into a log nobody reads, an empty read-back, a silent render) with nothing pointing at
# the cause. A wait that does not assert is a pause.
#
#   wait_for_boot <logfile> <pid> [tries] [pattern]
#
# Calls the caller's own `fail`, which every check defines — bash resolves it at call time, so it
# does not matter whether the source line comes before or after that definition.

wait_for_boot() {
  local log="$1"
  local pid="$2"
  local tries="${3:-120}"
  local pattern="${4:-\"event\":\"project.load\"}"
  local i=0
  while [ "$i" -lt "$tries" ]; do
    if grep -q "$pattern" "$log" 2>/dev/null; then
      return 0
    fi
    # THE PROCESS FIRST, THEN ONE LAST LOOK AT THE LOG. An engine can load and exit between two
    # iterations — a short --run-seconds under load does exactly that — and reporting "it died
    # before loading" for an engine that loaded and then finished its life would be a new wrong
    # answer in place of the old one.
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      if grep -q "$pattern" "$log" 2>/dev/null; then
        return 0
      fi
      echo "  --- last 15 lines of $log ---"
      tail -15 "$log" 2>/dev/null
      fail "the engine EXITED before its project loaded. It was still booting when the process
        went away, so this is its --run-seconds lifetime running out during boot rather than
        anything about the code under test — see task #106. Raise that engine's lifetime, or
        find out why boot took this long."
    fi
    sleep 0.25
    i=$((i + 1))
  done
  echo "  --- last 15 lines of $log ---"
  tail -15 "$log" 2>/dev/null
  fail "the engine never loaded its project within $(python3 -c "print($tries * 0.25)")s, and its
        process is still alive — so it is stuck rather than dead. The log tail above is the only
        evidence there is."
}
