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

# WAIT FOR AN EVENT INSTEAD OF SLEEPING AND HOPING (task #91).
#
# A `sleep 2.5` before an assertion is not a wait, it is a claim that the engine finishes within
# 2.5 seconds — a statement about the machine's load, not about the product. Under a full ctest
# run, or beside a build, it is false often enough to produce a failure whose message names the
# wrong thing: module_check reported "the sample did not load" and "the module did not load on the
# other machine" about an engine that did both, a few hundred milliseconds later. That is #91, and
# it survived two investigations as "unreproducible" because the message sent everyone to the
# product instead of to the harness.
#
# RETURNS NON-ZERO RATHER THAN CALLING _engine_wait_fail, deliberately: the call sites already
# have specific messages naming what they wanted and quoting the event they found instead, and
# those are better than anything this function could say. It prints the log tail — which the call
# sites did not — and then lets the caller's own `fail` speak.
#
# CHECKS THE PROCESS FIRST, THEN THE LOG, for the reason wait_for_boot gives: a dead engine writes
# nothing more, so testing the log first spends the whole timeout on a corpse.
#
#   wait_for_event_count <log> <pattern> <count> [tries=80] [what] [pid]
#   wait_for_event       <log> <pattern>         [tries=80] [what] [pid]     (count = 1)
#
# THE COUNT FORM IS NOT A LUXURY. When a step repeats — two saves of the same project, say — both
# write the SAME event, so a presence check is already satisfied by the first one and returns
# before the second command has been read off the ring. In module_check that would have compared
# the first file against itself, which passes forever.
wait_for_event_count() {
  local log="$1"
  local pattern="$2"
  local want="$3"
  local tries="${4:-80}"
  local what="${5:-$2}"
  local pid="${6:-}"
  local i=0
  local n=0
  local dead
  while [ "$i" -lt "$tries" ]; do
    dead=0
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      dead=1
    fi
    # `; true` because grep -c exits 1 when it counts zero, which under `set -e` would kill the
    # caller mid-wait with no message at all.
    n=$(grep -c "$pattern" "$log" 2>/dev/null; true)
    if [ "${n:-0}" -ge "$want" ]; then
      return 0
    fi
    if [ "$dead" = "1" ]; then
      echo "  --- the engine EXITED while waiting for $what (saw ${n:-0} of $want);"
      echo "      last 15 lines of $log ---"
      tail -15 "$log" 2>/dev/null
      return 1
    fi
    sleep 0.25
    i=$((i + 1))
  done
  echo "  --- timed out after $(python3 -c "print($tries * 0.25)")s waiting for $what"
  echo "      (saw ${n:-0} of $want, and the engine is still alive — so it was refused or never"
  echo "      arrived rather than the engine dying; grep the log for *_rejected);"
  echo "      last 15 lines of $log ---"
  tail -15 "$log" 2>/dev/null
  return 1
}

wait_for_event() {
  wait_for_event_count "$1" "$2" 1 "${3:-80}" "${4:-$2}" "${5:-}"
}

# WHY A LIVE CAPTURE CAME BACK EMPTY — the same argument as wait_for_boot, one layer along.
#
# Several checks arm DAW_CAPTURE_WAV and assert on the file. When it is empty they each say some
# version of "captured no audio — that is the harness, not the kit". True, and it stops exactly
# where the useful part starts, because "the harness" covers a dozen unrelated causes.
#
# On 2026-07-31 that message cost an hour and produced a wrong conclusion twice: first that the
# tests were flaky, then that the capture window was too short. Neither. The machine's default
# output was an AirPlay speaker (TP-Link_Music) that ACCEPTS being opened and then never runs a
# playback callback. The engine's own log said so plainly — "Audio output started", then
# "0 of 0 playback callbacks", and a pipeline depth that grew with runtime because nothing was
# pulling — and no check read any of it.
#
# THE DISCRIMINATOR IS THE CALLBACK COUNT, not the captured frames. Zero callbacks means the
# DEVICE never asked for audio, which is indistinguishable from silence at every layer above and
# cannot be caused by anything under test. Frames captured but wrong is a different failure and
# stays the caller's to describe.
#
#   capture_diagnosis <logfile>   -> one line naming the cause it can actually prove
capture_diagnosis() {
  local log="$1"
  [ -f "$log" ] || { echo "(no engine log at $log to diagnose from)"; return; }
  local dev callbacks
  dev="$(grep -m1 '^Audio device: ' "$log" 2>/dev/null | sed 's/^Audio device: //')"
  # "Audio device callbacks: N from the DEVICE" — N is how many times the device asked for audio.
  #
  # THIS USED TO READ THE WRONG NUMBER, and the comment here asserted the wrong meaning of it. It
  # parsed "N of M playback callbacks dropped a track" and called M "how many times the device
  # asked for audio". M is nothing of the kind: it counts callbacks that HAD A TRACK TO PLAY, so
  # an engine sitting with its transport stopped reports M=0 on a perfectly healthy device, and
  # this function would have accused that device of never running. On the machine it was written
  # against the two numbers coincided, which is exactly why it went unnoticed — and the same
  # misreading put a wrong cause in a memory file and in two agents' bug reports.
  callbacks="$(grep -o 'Audio device callbacks: [0-9]*' "$log" 2>/dev/null | tail -1 |
               grep -oE '[0-9]+' | head -1)"
  if [ "${callbacks:-unknown}" = "0" ]; then
    echo "the audio device ${dev:-(unnamed)} NEVER RAN A PLAYBACK CALLBACK: it opened, reported
        its rate and block size, and then asked for audio zero times. No audio can reach the
        capture tap, which lives in that callback, and the producer queue grows without bound.
        Nothing under test can cause or fix this. NOTE the device's own isPlaying() answers TRUE
        in this state, so it is not a usable second signal — the callback count is"
  elif [ -n "$callbacks" ]; then
    # THE DEVICE PULLED, SO LOOK UPSTREAM. The underrun summary counts DEVICE dropouts; a
    # PRODUCER starved before the device ever asked logs nothing there at all, so "0 underruns"
    # and "the audio was never generated" read identically. project.load carries over_budget and
    # peak_load_milli, which is where that shows up — the web-UI agent hit the same blind spot
    # from the browser side (three meter checks failing together at load average 91 with zero
    # underruns logged) and it is the same distinction.
    local over peak
    over="$(grep -o '"over_budget":[0-9]*' "$log" 2>/dev/null | tail -1 | grep -oE '[0-9]+')"
    peak="$(grep -o '"peak_load_milli":[0-9]*' "$log" 2>/dev/null | tail -1 | grep -oE '[0-9]+')"
    if [ "${over:-0}" -gt 0 ] 2>/dev/null; then
      echo "the device ${dev:-(unnamed)} ran $callbacks playback callbacks, so it WAS pulling —
        but the PRODUCER missed its budget on ${over} block(s) (peak load ${peak:-?}/1000). The
        underrun summary counts device dropouts and says nothing about a producer starved
        upstream, so treat this as INCONCLUSIVE rather than as a failure of whatever is under
        test: on a loaded machine the audio may simply never have been generated in time"
    else
      echo "the device ${dev:-(unnamed)} ran $callbacks playback callbacks and the producer met
        its budget, so both ends were working — an empty capture here is not the device's fault
        and is worth reading $log directly"
    fi
  else
    echo "the engine log has no underrun summary, so it did not reach a clean shutdown — read
        $log directly rather than trusting the capture"
  fi
}

# LAUNCHING AND REAPING AN ENGINE, so the next check cannot repeat the bug the last forty have.
#
# THE BUG: `( cd "$BUILD" && env ... ./daw_engine ... ) &` followed by `ENG=$!` captures the
# SUBSHELL, not the engine. `kill "$ENG"` then reaps the subshell and leaves the engine running.
# It is invisible almost always, because the engine exits by itself when --run-seconds elapses —
# so the pattern survived being copied into most of tools/. It stops being invisible when an
# engine BLOCKS before its run clock starts (a contended or dead audio device), because then it
# never exits at all: one such pair turned a 10-second check into a 935-second one, and four
# `ctest -j8` runs left 0, 11, 7 and 11 of them alive.
#
# `exec` is the whole fix — the subshell replaces itself with the engine, so $! IS the engine.
# It is one word, and it is one word that has to be remembered in forty places, which is why it
# is a function here instead.
#
#   start_engine <logfile> <argv...>     runs argv in $BUILD, sets ENGINE_PID to the ENGINE
#   stop_engine  [pid]                   TERM, wait, KILL if it ignored TERM, and SAY so
#
start_engine() {
  local log="$1"; shift
  ( cd "${BUILD:?start_engine needs BUILD set}" && exec "$@" >"$log" 2>&1 ) &
  ENGINE_PID=$!
}

# REPORTS WHEN IT HAD TO ESCALATE, rather than recovering in silence. A process that ignored
# SIGTERM is a defect somewhere even when the harness cleans up after it, and a recovered leak
# nobody mentions is how the NEXT one gets attributed to whatever was under test. (The web-UI
# agent reached the same conclusion from the other side and made their stack.mjs say it too.)
stop_engine() {
  local pid="${1:-${ENGINE_PID:-}}"
  [ -n "$pid" ] || return 0
  kill "$pid" 2>/dev/null || return 0
  local i=0
  while [ "$i" -lt 40 ]; do
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; return 0; }
    sleep 0.25
    i=$((i + 1))
  done
  echo "  note: engine $pid ignored SIGTERM for 10s and had to be killed — the run above is"
  echo "        still valid, but something in it did not shut down when asked"
  kill -9 "$pid" 2>/dev/null
  # CONFIRMS DEATH RATHER THAN ASSUMING IT, and `wait` cannot be what confirms it: when this
  # function is called inside a command substitution — `out="$(stop_engine "$p")"` — the pid is
  # not a child of THAT subshell, so `wait` fails instantly and returns without blocking. SIGKILL
  # is not synchronous either, so the caller's `kill -0` could still see the process and
  # reasonably conclude stop_engine had not killed it. Under `ctest -j8` that window is wide
  # enough to fail one run in three.
  wait "$pid" 2>/dev/null
  local j=0
  while [ "$j" -lt 40 ]; do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.05
    j=$((j + 1))
  done
  echo "  note: engine $pid survived SIGKILL for 2s, which should not be possible for an ordinary"
  echo "        process — it is most likely stuck in an uninterruptible wait"
  return 0
}

# ASSERT A CAPTURE EXISTS BEFORE MEASURING IT — one definition, because four checks were each
# reading a wav that was not there and reporting it as an unhandled Python exception:
#
#   FileNotFoundError: [Errno 2] No such file or directory: '.../m.wav'
#
# That is the worst message this repo produces. It names no audio, no device and no capture; a
# reader learns only that a file is missing, and the actual cause (a default output device that
# never runs a playback callback) is three layers away and never mentioned. level_match_bypass,
# midi_per_bus, preview_note and sidechain all failed exactly that way in one sweep.
#
#   require_capture <wavfile> <enginelog>
#
# Calls the caller's own `fail` when it has one, like the rest of this library.
require_capture() {
  [ -s "$1" ] && return 0
  _engine_wait_fail "the live run captured no audio at $1, so nothing measured below would mean
        anything. That is the harness rather than the thing under test — specifically:
        $(capture_diagnosis "$2")"
}
