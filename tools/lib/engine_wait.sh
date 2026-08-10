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
      # THE DEVICE'S OWN DROPOUT COUNT, which this function used to ignore entirely — and that
      # made it announce the opposite of the truth. On 2026-08-05 panic_check captured 14s of
      # silence and this printed "both ends were working" over a log reading
      #
      #   Audio underrun summary: 1427 of 1468 callbacks that HAD A TRACK TO PLAY dropped one
      #
      # 97% of callbacks got nothing to play. over_budget was 0 because that counter is written
      # at project.load and describes the producer's own budget, not what the device received;
      # taking its silence as proof of health is the same mistake as reading a green suite as
      # proof of coverage. A wrong diagnosis is worse than none — it sends the next person to
      # the wrong layer, which is what this whole function exists to prevent.
      local dropped total
      dropped="$(grep -oE 'underrun summary: [0-9]+ of [0-9]+' "$log" 2>/dev/null | tail -1 |
                 grep -oE '[0-9]+' | head -1)"
      total="$(grep -oE 'underrun summary: [0-9]+ of [0-9]+' "$log" 2>/dev/null | tail -1 |
               grep -oE '[0-9]+' | tail -1)"
      if [ "${dropped:-0}" -gt 0 ] 2>/dev/null; then
        # ATTRIBUTE THIS TO THE PRODUCER, NOT THE DEVICE. An underrun is counted when a callback
        # HAD a track to play and the producer had no block ready for it. The device asking is
        # what `$callbacks` counts, and it asked every time. The first version of this branch
        # said the device "was handed nothing to play" and blamed a high-latency output — which
        # is the same misreading the counter's own comment in engine_audio_callback.h records
        # having been made twice before in writing, and it made three.
        echo "the device ${dev:-(unnamed)} ran $callbacks playback callbacks and the PRODUCER had
        no block ready for ${dropped} of ${total:-?} of them. The device did its job; the engine
        did not fill it. A capture written from inside that callback is silence for that reason,
        so read this as the producer failing to keep up with the fixture's load — see the
        underrun summary and pipeline depth in $log — and NOT as a fault of the output device"
      else
        echo "the device ${dev:-(unnamed)} ran $callbacks playback callbacks, the producer met
        its budget and the device dropped none, so all three were working — an empty capture here
        is not the device's fault and is worth reading $log directly"
      fi
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
# A BACKSTOP FOR ORPHANED HOSTS, AND IT HAS NEVER BEEN SEEN TO FIRE. Read that before trusting it.
#
# Two juce_host_process children of this repo's build were found alive after 1h28m tonight,
# reparented to init at 0% CPU, holding their sockets. Orphaned hosts accumulate, hold the audio
# device, and make "is the machine busy" read yes on an idle box — both agents sharing this
# machine have been misled by that.
#
# THE OBVIOUS EXPLANATION IS DISPROVEN. I assumed a SIGKILLed engine leaves its hosts behind,
# since HostController::killHostProcess only runs on an orderly shutdown. It does not: a host
# exits on its own when its socket peer dies. Measured both ways — engine SIGSTOPped so
# stop_engine must escalate to SIGKILL, and engine SIGKILLed directly with no cleanup path at
# all — and in both cases every host was gone within seconds WITH THIS REAPER DISABLED. The
# negative control killed the theory, not the hosts.
#
# So the mechanism that produced those two is still unknown, and this code has no demonstrated
# effect. It is kept because it is eight lines that cannot misfire, not because it is known to
# help: it kills only pids recorded as children of an engine we have just killed ourselves.
# `pgrep -P` matches by PARENT rather than command line, so it cannot match a pattern, the other
# agent's processes, or itself — the three ways this has gone wrong in this repo before. The list
# must be captured BEFORE the kill, because afterwards the children are reparented to init and
# nothing can say whose they were.
#
# If orphans appear again, note what was running: this reaper firing would be the first real
# evidence about the cause, so the message below is deliberately loud.
stop_engine() {
  local pid="${1:-${ENGINE_PID:-}}"
  [ -n "$pid" ] || return 0
  local hosts
  # `|| true` ON THE SUBSTITUTION, and this is the THIRD time today this exact shape has bitten.
  # `pgrep` EXITS NON-ZERO WHEN IT MATCHES NOTHING — an engine with no live children is the normal
  # case by cleanup time — and these checks run under `set -euo pipefail`, where an assignment from
  # a failing command substitution aborts the shell. This function is called from an EXIT trap, so
  # the abort landed AFTER the check had printed PASS and turned nine green suites red with their
  # own success message still on screen.
  #
  # The general shape: a command whose non-zero exit is a normal ANSWER rather than a failure —
  # pgrep matching nothing, grep finding nothing, ls on an empty directory, a linter reporting
  # errors — used inside `x="$(...)"` under set -e. The status belongs to the answer, not to the
  # assignment.
  hosts="$(pgrep -P "$pid" 2>/dev/null | tr '\n' ' ' || true)"
  kill "$pid" 2>/dev/null || { reap_hosts "$hosts"; return 0; }
  local i=0
  while [ "$i" -lt 40 ]; do
    kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null; reap_hosts "$hosts"; return 0; }
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
    kill -0 "$pid" 2>/dev/null || { reap_hosts "$hosts"; return 0; }
    sleep 0.05
    j=$((j + 1))
  done
  echo "  note: engine $pid survived SIGKILL for 2s, which should not be possible for an ordinary"
  echo "        process — it is most likely stuck in an uninterruptible wait"
  reap_hosts "$hosts"
  return 0
}

# Kill whichever of the recorded child pids are still alive. Silent when there is nothing to do,
# which is the normal case: an orderly shutdown has already taken them.
reap_hosts() {
  local left=0 h
  for h in ${1:-}; do
    kill -0 "$h" 2>/dev/null || continue
    kill -9 "$h" 2>/dev/null
    left=$((left + 1))
  done
  [ "$left" -gt 0 ] && echo "  note: REAPED $left ORPHANED HOST(S). This has never fired in a
        constructed test — a host normally exits when its engine dies. Whatever run produced this
        is the one that explains the orphans nobody has accounted for; say what it was."
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

# WAIT FOR THE THING YOU ARE ABOUT TO ASSERT ON, not for an event that normally precedes it.
#
# `wait_for_boot` returning means the engine LOADED the project. It does not mean any published
# REGION has been written yet, and most checks go straight on to read one. Seventeen checks bridge
# that gap with a fixed `sleep 1.0`-`sleep 1.5`, which is a bet on how busy the machine is:
#
#   lane_quantize_check    failed ~1 run in 3 with "no published notes for track 0 — the fixture
#                          did not load". The fixture was fine; the read was early. Replacing the
#                          sleeps with wait_for_boot did NOT fix it (4 failures in 12) — only
#                          polling for the notes themselves did (0 in 12).
#   automation_readback    failed inside a full ctest with "lane 'cutoff' reports MISSING points",
#                          which is that check's name for "the lane list does not include it at
#                          all" — a serious-looking product defect that was a race. 6/6 standalone.
#
# Both now poll. This helper is here so the remaining fifteen can be converted the same way rather
# than each inventing its own loop, and so the reason is written down once.
#
# WAIT FOR A PUBLISHED VALUE, re-reading each time.
#
# wait_until takes a COMMAND, so `wait_until 30 test "$(field x)" = 1` evaluates the substitution
# once, before the first poll — which is the very race it was meant to close. This takes the
# ACCESSOR and calls it again on every iteration.
#
# USAGE:  wait_for_published <seconds> <expected> <accessor> [args...]
#
# Returns non-zero on timeout and the caller still asserts, exactly as wait_until documents: this
# removes the race, it does not substitute for the assertion.
wait_for_published() {
  local secs="$1" want="$2"; shift 2
  local tries=$(( secs * 4 ))
  local i=0
  local got=""
  while [ "$i" -lt "$tries" ]; do
    got="$("$@" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
      return 0
    fi
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}

# WAIT FOR THE ENGINE TO HAVE ACTED ON N COMMANDS.
#
# The engine appends one line per ACCEPTED command to history.jsonl in the project directory —
# {seq, ts_ms, author, scope, base_version, op, outcome, params} — written from the command thread
# after it has acted. So "the file has N lines" is precisely the thing a `sleep 1.2` after a
# `cli do ...` was guessing at, and it is the same signal whatever the command was.
#
# THIS IS "THE ENGINE ACTED", NOT "THE UI CAN SEE IT". The consumer thread publishes to shared
# memory on its own tick, so a check that then reads published state through `daw-cli get` wants
# wait_for_published instead. Use this one when what happens next reads the LOG, the SAVED FILE, or
# issues another command.
#
# Counting LINES rather than parsing seq is deliberate: the writer holds a mutex per line, so a
# partial line cannot appear, and a count needs no JSON parser in the hot path of a poll.
#
# DAW_NO_HISTORY disables the journal entirely. A check that sets it must not use this.
#
# USAGE:  wait_for_history <project-dir> <count> [seconds=20]
#
# Returns non-zero on timeout and the CALLER still asserts, exactly as wait_until documents.
wait_for_history() {
  local dir="$1" want="$2" secs="${3:-20}"
  local tries=$(( secs * 4 ))
  local i=0 n=0
  while [ "$i" -lt "$tries" ]; do
    # `wc -l < file` fails in the SHELL's redirection when the file is absent, and 2>/dev/null on
    # wc cannot suppress that — the redirect is evaluated first. Pass the path as an argument so
    # the error is wc's own and can be silenced: a polling library must not print once per poll.
    n=$(wc -l "$dir/history.jsonl" 2>/dev/null | awk '{print $1}')
    [ -n "$n" ] && [ "$n" -ge "$want" ] 2>/dev/null && return 0
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}

# USAGE:  wait_until <seconds> <command...>     — polls every 0.25s until the command succeeds.
# Returns non-zero on timeout, and the CALLER still asserts: this removes the race, it does not
# substitute for the assertion. A check whose poll times out must still fail with its own message
# naming what was missing, or the poll has merely moved the silence.
wait_until() {
  local secs="$1"; shift
  local tries=$(( secs * 4 ))
  local i=0
  while [ "$i" -lt "$tries" ]; do
    if "$@" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.25
    i=$((i + 1))
  done
  return 1
}

# HOW MANY COMMANDS THE ENGINE HAS ACTED ON SO FAR. Prints 0 when the journal does not exist yet,
# which is the honest answer before the first accepted command and keeps callers from having to
# special-case a fresh project directory.
#
# USAGE:  history_lines <project-dir>
history_lines() {
  local n
  n=$(wc -l "$1/history.jsonl" 2>/dev/null | awk '{print $1}')
  [ -n "$n" ] && echo "$n" || echo 0
}

# ISSUE ONE COMMAND AND WAIT FOR THE ENGINE TO HAVE ACTED ON IT.
#
# This is the shape almost every `cli do ... ; sleep 0.6` in this repo was reaching for. The sleep
# was a guess at how long the command thread would take; the journal says exactly when it is done.
#
# IT READS THE COUNT BEFORE AND WAITS FOR ONE MORE, rather than tracking a running total in the
# caller. A hand-maintained counter is wrong the first time someone inserts a command in the middle
# of a check and does not renumber the rest — and it is wrong SILENTLY, because waiting for a count
# that has already been passed returns immediately and the race comes straight back.
#
# THE CALLER STILL ASSERTS. A refused command may not be journalled at all, in which case this
# times out and returns non-zero — which is the caller's `|| fail "..."` doing its job, with its
# own message about what was refused.
#
# PASS `env` IF THE INVOCATION HAS AN ASSIGNMENT PREFIX. `DAW_UI_SHM_NAME=x "$CLI" do ...` is a
# SHELL parse: the prefix is an assignment only because the shell is reading that line. Handed to
# this function it arrives in "$@", where it is argv[0] — and the shell then looks for a program
# literally named `DAW_UI_SHM_NAME=/lm_123`, fails, and this returns non-zero. A site written
# `... || true` swallows that, AND THE COMMAND NEVER RAN. Three checks failed with "the save
# produced no file" before I saw it, which reads as a race and is not one.
#
# USAGE:  after_command <project-dir> <cli-invocation...>
#   e.g.  after_command "$TMP" cli do set-row-ops --track 0 --note 100 --prob 60 \
#             || fail "row-ops write refused"
#   e.g.  after_command "$TMP" env DAW_UI_SHM_NAME="$shm" "$CLI" do load lm --force || true
after_command() {
  local dir="$1"; shift
  local before
  before=$(history_lines "$dir")
  "$@" >/dev/null 2>&1 || return 1
  wait_for_history "$dir" $((before + 1)) 20
}

# A SHIPPED PRESET THAT ACTUALLY HOSTS A PLUGIN, chosen by reading the presets rather than named.
#
# Three checks need a real hosted VST — a param write must reach one, undo must push state back
# into one — and all three hardcoded `rack` because rack had an Identity instrument. It stopped
# having one: a branch merge brought in a version of the preset whose chain is a sampler, and all
# three checks began failing with "no parameters reported", which reads as a mirror bug and is a
# FIXTURE bug. A hardcoded fixture name is a claim about a file's contents that nothing rechecks.
#
# Prints the preset STEM (what `do load` takes) of the first preset declaring a hosted plugin, or
# nothing at all — the caller must treat empty as "this check cannot run" and say so, rather than
# falling back to a name and reporting the wrong component.
#
# USAGE:  PRESET="$(preset_with_hosted_plugin "$ROOT")"
preset_with_hosted_plugin() {
  local root="$1"
  local f
  for f in "$root"/presets/projects/*.uniproj.json; do
    [ -s "$f" ] || continue
    if grep -q '"vst_instrument"\|"vst_effect"' "$f" 2>/dev/null; then
      basename "$f" .uniproj.json
      return 0
    fi
  done
  return 1
}
