# A BOUNDED CPU LOAD GENERATOR, for asking whether a check survives a busy machine.
#
# A fixed sleep gets less correct as the machine gets busier, and a wait just gets slower — so the
# way to tell a real wait from a lucky one is to run the check against competition. Every flake this
# repo has chased has been load-sensitive, and every investigation that could not reproduce one was
# run on an idle machine.
#
# TWO THINGS THIS GETS RIGHT, BOTH BECAUSE THEY HAVE GONE WRONG HERE BEFORE.
#
#   IT BOUNDS ITSELF. Eight load generators were once orphaned at 97% CPU each for fourteen hours,
#   because the parent was SIGKILLed and `trap EXIT` does not run on SIGKILL. A generator that
#   depends on its parent to stop it will eventually not be stopped. Each worker here holds its own
#   deadline and exits on its own, whatever happens to whoever started it.
#
#   IT BURNS CPU WITHOUT FORKING. $SECONDS is a shell builtin, so the inner loop is arithmetic
#   only. My first attempt polled `date +%s` every iteration and spent its time in fork and wait —
#   which does load a machine, but with process creation rather than with work, and it is the wrong
#   kind of load to compete with an audio engine for a core.
#
# AND IT IS MEASURED PER WORKER, NOT ASSUMED. This is the part that actually went wrong: the first
# report ran `ps -r`, which without `-A` lists only processes on the CONTROLLING TERMINAL — and
# these workers are detached. It printed an idle machine while three workers sat at 100% of a core
# each, so six "under load" runs of a check were claimed and never verified. The line that exists
# to prove the load was blind to it. It reads each worker by pid now.
#
# (I first blamed `local` inside the worker subshell for that. It is not a bug: the subshell is
# created inside a function, so `local` is legal there — putting it back leaves the workers at 100%.
# The wrong explanation was in this header for one commit; the control is what found it.)
#
# USAGE:
#   . tools/lib/load_generator.sh
#   start_load 6 120        # 6 workers, each exiting after 120 seconds no matter what
#   ...run the check...
#   stop_load               # optional; they expire on their own regardless

_LOAD_PIDS=""

start_load() {
  local n="${1:-4}" secs="${2:-60}" i
  _LOAD_PIDS=""
  for ((i = 0; i < n; i++)); do
    (
      # SECONDS is reset per subshell and is a builtin: no fork per iteration, so the time goes
      # into the arithmetic rather than into process creation.
      k=0
      while [ "$SECONDS" -lt "$secs" ]; do
        k=$((k + 1))
      done
    ) &
    _LOAD_PIDS="$_LOAD_PIDS $!"
  done
  # Give them a moment to spin up, then SAY what the machine is doing. A caller that skips this is
  # back to claiming load rather than having it.
  sleep 2
  echo "  load: $n worker(s), each self-limited to ${secs}s. Measured:"
  # `ps -A`, NOT `ps -r` alone. Without -A, ps lists only processes on the controlling terminal, and
  # these workers are detached — so the report showed the machine as idle while three of them were
  # sitting at 100% of a core each. The line that exists to prove the load was itself blind to it,
  # which is the same defect as the generator that did not load: a measurement nobody checked.
  local p
  for p in $_LOAD_PIDS; do
    ps -p "$p" -o pid,%cpu= 2>/dev/null | tail -1 | sed 's/^/    worker /'
  done
}

stop_load() {
  [ -n "$_LOAD_PIDS" ] || return 0
  kill $_LOAD_PIDS 2>/dev/null
  wait $_LOAD_PIDS 2>/dev/null
  _LOAD_PIDS=""
}
