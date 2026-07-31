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
# Calls the caller's own `fail` when it has one — bash resolves it at call time, so it does not
# matter whether the source line comes before or after that definition. NOT every check defines
# one (sidechain_check runs on `set -e` and inline exits), so there is a fallback: a library that
# only works for checks written a particular way is a library half the callers cannot use.

_engine_wait_fail() {
  if declare -F fail >/dev/null 2>&1; then
    fail "$@"
  fi
  echo "  FAIL: $*"
  exit 1
}

wait_for_boot() {
  local log="$1"
  local pid="$2"
  local tries="${3:-120}"
  local pattern="${4:-\"event\":\"project.load\"}"
  local i=0
  local dead
  while [ "$i" -lt "$tries" ]; do
    # SAMPLE THE PROCESS BEFORE READING THE LOG, and the order is the whole correctness argument.
    # If the process is already gone at this instant, then everything it will ever write is
    # already in the file — so the grep below is the FINAL word and a match is real. Reading the
    # log first and asking about the process afterwards leaves a window where the line lands
    # between the two, and a engine that loaded and then reached the end of its --run-seconds is
    # declared to have died during boot.
    #
    # An earlier version handled that with a second grep inside the death branch. It worked, and
    # it could not be tested from outside: getting a line to land in that window on purpose is
    # not something a check can stage. This ordering removes the race instead of mitigating it,
    # and the "loaded, then exited" case is now an ordinary deterministic path.
    dead=0
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      dead=1
    fi
    if grep -q "$pattern" "$log" 2>/dev/null; then
      return 0
    fi
    if [ "$dead" = "1" ]; then
      echo "  --- last 15 lines of $log ---"
      tail -15 "$log" 2>/dev/null
      # STATES THE OBSERVATION, NOT A CAUSE. The first time this fired for real the engine had
      # refused to start because the host binary was built against an older kShmVersion, and an
      # earlier wording of this message confidently blamed --run-seconds — sending the reader
      # looking at the wrong thing while the true reason sat in the tail printed just above.
      _engine_wait_fail "the engine EXITED before its project loaded — the tail above is what it
        said on the way out. Two common causes, and the log tells you which: its --run-seconds
        lifetime ran out during boot (likely under a parallel ctest, see task #106), or it
        refused to start at all (a stale juce_host_process after a kShmVersion bump does exactly
        this — rebuild everything, not just daw_engine)."
    fi
    sleep 0.25
    i=$((i + 1))
  done
  echo "  --- last 15 lines of $log ---"
  tail -15 "$log" 2>/dev/null
  _engine_wait_fail "the engine never loaded its project within $(python3 -c "print($tries * 0.25)")s,
        and its process is still alive — so it is stuck rather than dead. The log tail above is
        the only evidence there is."
}

# WAIT FOR THE Nth PROJECT LOAD — a RELOAD, not a boot.
#
# Same corpse problem, one step along: a check that issues `do load` and then waits for
# "project.load" is waiting for an event that may already be in the log from the boot, and is
# waiting on a process that may have reached the end of its --run-seconds meanwhile. Counting the
# loads answers the first; the pid answers the second.
#
# `what` names the load the caller was waiting for ("the second reload"), so the library's
# diagnosis does not cost the caller its own more specific wording.
#
#   wait_for_loads <logfile> <pid> <count> [tries] [what]
wait_for_loads() {
  local log="$1"
  local pid="$2"
  local want="$3"
  local tries="${4:-80}"
  local what="${5:-a project load}"
  local i=0
  local n
  local dead
  while [ "$i" -lt "$tries" ]; do
    # Process first, then the log — see wait_for_boot for why that order and not the other.
    dead=0
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      dead=1
    fi
    n=$(grep -c '"event":"project.load"' "$log" 2>/dev/null; true)
    if [ "${n:-0}" -ge "$want" ]; then
      return 0
    fi
    if [ "$dead" = "1" ]; then
      echo "  --- last 15 lines of $log ---"
      tail -15 "$log" 2>/dev/null
      _engine_wait_fail "the engine EXITED while this check waited for $what: it had loaded a
        project ${n:-0} time(s) and $want were wanted. The tail above is what it said on the way
        out; see wait_for_boot for the two usual causes."
    fi
    sleep 0.25
    i=$((i + 1))
  done
  echo "  --- last 15 lines of $log ---"
  tail -15 "$log" 2>/dev/null
  _engine_wait_fail "waiting for $what: only ${n:-0} of $want project loads happened within
        $(python3 -c "print($tries * 0.25)")s, and the engine is still ALIVE — so the load was
        refused or never arrived rather than the engine dying. Grep the log for *_rejected."
}
