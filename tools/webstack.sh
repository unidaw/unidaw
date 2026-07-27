#!/usr/bin/env bash
# Start exactly one engine + one sidecar + one page server, or report why not.
#
# Written because "is the engine running?" has been answered wrongly three times
# in this project: once by a `ps` pattern that matched nothing (the engine runs as
# ./daw_engine, not build/daw_engine), and twice by two engines racing on the same
# shared-memory segment, which produces frozen frames that look exactly like a
# healthy stopped transport.
# NOTE for automated callers: run this with output redirected to a file and the
# invocation backgrounded. The processes it starts are orphaned out of its
# process group, but a tool that waits on the group still waits — this stalled a
# five-minute timeout while everything it had started was running perfectly.
#
#   ./tools/webstack.sh > /tmp/stack.out 2>&1     (backgrounded by the caller)
set -euo pipefail

WEB=/Users/jak/src/daw-web
SHARED=/Users/jak/src/daw
# THIS checkout's own build. It used to be the other agent's, on the belief that
# "an engine built in this worktree loses its plugin host a few seconds in
# ('Failed to receive control header') and exits, every time". That belief was
# wrong twice over, and it cost us a shared build directory and a tangled tree:
#
#  - "Failed to receive control header" is not a failure signature. The host
#    prints it whenever recvHeader() returns false, which includes the clean EOF
#    it gets when the engine closes the socket. It is the host's normal epitaph
#    on every orderly shutdown, so it is the last line of a healthy log too.
#  - The real difference between the trees was the WORKING DIRECTORY, not the
#    build. Default-plugin discovery is cwd-relative and probed only the flat
#    `identity_plugin_artefacts/VST3/` layout, which JUCE stopped emitting long
#    ago and which survives solely as a leftover in older build dirs. A fresh
#    checkout therefore came up with "plugin paths=0" — silently, with no plugin
#    at all. Fixed in apps/daw_engine_main.cpp, which now probes the
#    per-configuration subdirectory as well.
#
# The failure does not reproduce: this build, driven for 90s with a project
# loaded, 25 note writes and the transport running, kept its engine and all six
# hosts. The historic sighting was almost certainly the four-engines-on-one-
# segment race that the lock below now prevents.
#
# Set ENGINE=... to override, e.g. to run against the other checkout's build.
ENGINE=${ENGINE:-$WEB/build/daw_engine}
HOST=${HOST:-$WEB/build/juce_host_process}
RUNDIR=$(dirname "$ENGINE")
SHM=${SHM:-/daw_web_ui}
PROJECTS=$WEB/presets/projects
PORT=${PORT:-8173}

say() { printf '  %s\n' "$*"; }

# Two agents share this machine, so kill only what is on OUR segment. The backend
# agent runs `daw_engine --run-seconds N` against its own shm constantly; killing
# by process name would shoot down its test runs, and refusing to start because
# one is up would block on something that is none of our business.
# Scoped by SEGMENT, so a second stack on a different shm can run beside the
# first instead of evicting it. They were global, which meant the only way to
# test an alternative engine was to take the working one down — and that is
# exactly the experiment worth running most often.
SEG=$(printf '%s' "$SHM" | tr -c 'A-Za-z0-9' '_')
PIDFILE=/tmp/uni-web-stack$SEG.pids
LOCK=/tmp/uni-web-stack$SEG.lock
# Sidecar ports follow the page port, so one override moves the whole stack.
WS_STATE=${WS_STATE:-$((PORT + 1))}
WS_CMD=${WS_CMD:-$((PORT + 2))}

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

# EXCLUSIVE. Two copies of this script overlapping is how four engines ended up
# writing one segment: each read the pidfile, each killed what was in it, each
# truncated it, and each started an engine the others never learned about. The
# window is wide — the sidecar build alone is tens of seconds — and an automated
# caller that backgrounds this and sleeps a fixed number of seconds walks into it
# every time it guesses low. mkdir is atomic on every filesystem that matters.
if ! mkdir "$LOCK" 2>/dev/null; then
  # A lock left by a killed run must not wedge the machine for ever.
  if [ -n "$(find "$LOCK" -maxdepth 0 -mmin +10 2>/dev/null)" ]; then
    say "taking a stale lock ($LOCK, older than 10 min)"
    rmdir "$LOCK" 2>/dev/null || true
    mkdir "$LOCK" 2>/dev/null || { say "REFUSING TO START: cannot take $LOCK"; exit 1; }
  else
    say "REFUSING TO START: another webstack.sh holds $LOCK"
    say "if that is wrong: rmdir $LOCK"
    exit 1
  fi
fi
trap 'rmdir "$LOCK" 2>/dev/null || true' EXIT

# Every process whose ENVIRONMENT names our segment.
#
# This is the authority on "what is running on /daw_web_ui", and the pidfile is
# not. The engine takes its segment from DAW_UI_SHM_NAME, so it is not in the
# command line — but `ps eww` prints the environment, so it can be matched
# exactly. That finds an engine started by a previous run of this script, by
# hand, or by a concurrent copy, none of which any pidfile knows about; and it
# cannot touch the backend agent's engines, which run on their own segment names.
#
# This script used to report `pgrep -f daw_engine`, which prints SOME engine
# rather than the one it started — so its own output said the stack was healthy
# while four engines fought over one seqlock, producing frozen frames, plugin
# hosts torn down by rivals, and engine deaths that looked like an engine bug.
our_engines() {
  local pid
  for pid in $(pgrep -f "$(basename "$ENGINE")" 2>/dev/null || true); do
    if ps eww -p "$pid" 2>/dev/null | tr ' ' '\n' | grep -qx "DAW_UI_SHM_NAME=$SHM"; then
      printf '%s\n' "$pid"
    fi
  done
}

for pid in $(our_engines); do kill "$pid" 2>/dev/null || true; done
if [ -f "$PIDFILE" ]; then
  while read -r pid; do alive "$pid" && kill "$pid" 2>/dev/null || true; done < "$PIDFILE"
fi
sleep 2
for pid in $(our_engines); do kill -9 "$pid" 2>/dev/null || true; done
sleep 1
still=$(our_engines | tr '\n' ' ')
if [ -n "${still// /}" ]; then
  say "REFUSING TO START: engine(s) on $SHM would not die: $still"
  ps -o pid,command -p $(echo "$still" | tr ' ' ',' | sed 's/,*$//') 2>/dev/null || true
  exit 1
fi
rm -f "$PIDFILE"
# Also clear anything holding OUR ports. A sidecar started by hand is not in the
# pidfile, and "Address already in use" three steps later is a much worse way to
# find out about it than here.
for port in $WS_STATE $WS_CMD; do
  for pid in $(lsof -nP -iTCP:$port -sTCP:LISTEN -t 2>/dev/null || true); do
    kill "$pid" 2>/dev/null || true
  done
done
# NOT killing juce_host_process. It is tempting — orphaned hosts do linger after
# an engine dies — but the backend agent's engines spawn hosts too, and a global
# pkill would shoot down their test runs. That is the same cross-agent hazard the
# engine kill above is scoped to avoid; leaving a few orphans is the cheaper
# mistake. They are harmless: HostController::launch() unlinks the socket path
# before it binds, so a leftover file cannot route a new engine into an old
# host — checked in host_controller.cpp rather than assumed.
sleep 1

[ -x "$ENGINE" ] || { say "no engine at $ENGINE — run: cmake --build build --target daw_engine"; exit 1; }
[ -x "$HOST" ] || { say "no host at $HOST — run: cmake --build build --target juce_host_process"; exit 1; }

# Each child is launched inside `( ... & )` so the subshell exits immediately and
# the child is orphaned OUT of this script's process group. `nohup cmd &` alone
# does not do that: the child stays in the group, an automated caller waits on
# the group for output that never ends, and the script appears to hang while
# everything it started is running perfectly. Cost me a five-minute timeout on
# the one run where the page server was not already up.
#
# Pids come back through the file, since `$!` inside a subshell is not visible
# out here.
: > "$PIDFILE"

( cd "$RUNDIR" && DAW_UI_SHM_NAME=$SHM DAW_PROJECT_DIR=$PROJECTS DAW_HOST_BINARY=$HOST \
    nohup "$ENGINE" "$@" > /tmp/eng$SEG.log 2>&1 < /dev/null & echo $! >> "$PIDFILE" )
sleep 6
# The engine's OWN pid, resolved from the segment rather than from `$!`. `$!`
# here is the subshell that wraps the `cd &&`, one below the engine, so it was
# reported and health-checked in place of the process it started — off by one and
# right often enough to look correct.
ENGINE_PID=$(our_engines | head -1)
alive "$ENGINE_PID" || { say "engine exited during startup:"; tail -5 /tmp/eng$SEG.log; exit 1; }

# Build before launching. This script used to run whatever release binary was
# lying around, so an edited sidecar started silently as the previous one and
# answered a brand-new command with "unknown command" — twenty minutes of looking
# for a bug in code that was never running.
say "building the sidecar…"
( cd "$WEB/ui" && cargo build --release -p daw-sidecar ) > /tmp/side-build.log 2>&1 \
  || { say "sidecar build failed:"; tail -20 /tmp/side-build.log; exit 1; }

( cd "$WEB/ui" && DAW_PROJECT_DIR=$PROJECTS \
    nohup ./target/release/daw-sidecar --shm "$SHM" --port "$WS_STATE" --cmd-port "$WS_CMD" \
      > /tmp/side$SEG.log 2>&1 < /dev/null & echo $! >> "$PIDFILE" )
sleep 2
SIDECAR_PID=$(sed -n 2p "$PIDFILE")
grep -q 'attached to' /tmp/side$SEG.log || { say "sidecar did not attach:"; head -3 /tmp/side$SEG.log; exit 1; }

if ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  # Fully detached: </dev/null and nohup. A backgrounded child that still holds
  # the caller's stdin keeps an automated caller waiting for EOF forever — this
  # script hung a five-minute tool timeout exactly once, on the one run where the
  # page server was not already up and this branch actually executed.
  ( cd "$WEB/ui-web" && nohup python3 -m http.server "$PORT" --bind 127.0.0.1 \
      > /tmp/page.log 2>&1 < /dev/null & )
  sleep 1
fi

# The pid we STARTED, not whichever one a pattern happens to find first.
say "engine  pid $ENGINE_PID"
# And there is exactly one of them. Everything above is about getting here; if it
# is ever wrong, say so rather than leave a racing pair to be discovered later as
# an inexplicable engine death.
engines=$(our_engines | tr '\n' ' ')
count=$(printf '%s' "$engines" | wc -w | tr -d ' ')
if [ "$count" != "1" ]; then
  say "REFUSING: $count engines on $SHM ($engines) — a single-producer segment with $count writers"
  exit 1
fi
say "sidecar pid $(pgrep -f "daw-sidecar.*$SHM")  ws $WS_STATE state / $WS_CMD commands"
say "page    http://127.0.0.1:$PORT/index.html"
head -2 /tmp/side$SEG.log | sed 's/^/  /'
