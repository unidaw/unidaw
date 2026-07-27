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
# The SHARED build, by preference. It is the backend agent's tree, so this can
# put us on an SHM version ahead of the daw-bridge this branch is pinned to —
# which happened, at v13 against v12. That risk is accepted because the attach
# check below catches it immediately and loudly, and because the alternative is
# worse: an engine built in this worktree loses its plugin host a few seconds in
# ("Failed to receive control header") and exits, every time, while the same
# source built in the shared tree runs indefinitely. I have not found why; the
# host binaries behave identically when run alone.
#
# Set ENGINE=... to override, e.g. to test this worktree's own build.
ENGINE=${ENGINE:-$SHARED/build/daw_engine}
HOST=${HOST:-$SHARED/build/juce_host_process}
RUNDIR=$(dirname "$ENGINE")
SHM=${SHM:-/daw_web_ui}
PROJECTS=$WEB/presets/projects
PORT=${PORT:-8173}

say() { printf '  %s\n' "$*"; }

# Two agents share this machine, so kill only what is on OUR segment. The backend
# agent runs `daw_engine --run-seconds N` against its own shm constantly; killing
# by process name would shoot down its test runs, and refusing to start because
# one is up would block on something that is none of our business.
# The engine takes its segment from DAW_UI_SHM_NAME, an env var, so it is not in
# the command line to pattern-match on. Track pids in a file instead.
PIDFILE=/tmp/uni-web-stack.pids

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

if [ -f "$PIDFILE" ]; then
  while read -r pid; do alive "$pid" && kill "$pid" 2>/dev/null || true; done < "$PIDFILE"
  sleep 2
  still=""
  while read -r pid; do alive "$pid" && still="$still $pid"; done < "$PIDFILE"
  if [ -n "$still" ]; then
    say "REFUSING TO START: our own process(es)$still would not die"
    ps -o pid,command -p $(echo "$still" | tr ' ' ',' | sed 's/^,//;s/,$//') 2>/dev/null || true
    exit 1
  fi
  rm -f "$PIDFILE"
fi
# Also clear anything holding OUR ports. A sidecar started by hand is not in the
# pidfile, and "Address already in use" three steps later is a much worse way to
# find out about it than here.
for port in 8174 8175; do
  for pid in $(lsof -nP -iTCP:$port -sTCP:LISTEN -t 2>/dev/null || true); do
    kill "$pid" 2>/dev/null || true
  done
done
# NOT killing juce_host_process. It is tempting — orphaned hosts do linger after
# an engine dies — but the backend agent's engines spawn hosts too, and a global
# pkill would shoot down their test runs. That is the same cross-agent hazard the
# engine kill above is scoped to avoid; leaving a few orphans is the cheaper
# mistake. They are harmless: an orphan holds a socket nobody connects to.
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
    nohup "$ENGINE" "$@" > /tmp/eng.log 2>&1 < /dev/null & echo $! >> "$PIDFILE" )
sleep 6
ENGINE_PID=$(sed -n 1p "$PIDFILE")
alive "$ENGINE_PID" || { say "engine exited during startup:"; tail -5 /tmp/eng.log; exit 1; }

# Build before launching. This script used to run whatever release binary was
# lying around, so an edited sidecar started silently as the previous one and
# answered a brand-new command with "unknown command" — twenty minutes of looking
# for a bug in code that was never running.
say "building the sidecar…"
( cd "$WEB/ui" && cargo build --release -p daw-sidecar ) > /tmp/side-build.log 2>&1 \
  || { say "sidecar build failed:"; tail -20 /tmp/side-build.log; exit 1; }

( cd "$WEB/ui" && DAW_PROJECT_DIR=$PROJECTS \
    nohup ./target/release/daw-sidecar --shm "$SHM" > /tmp/side.log 2>&1 < /dev/null & echo $! >> "$PIDFILE" )
sleep 2
SIDECAR_PID=$(sed -n 2p "$PIDFILE")
grep -q 'attached to' /tmp/side.log || { say "sidecar did not attach:"; head -3 /tmp/side.log; exit 1; }

if ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  # Fully detached: </dev/null and nohup. A backgrounded child that still holds
  # the caller's stdin keeps an automated caller waiting for EOF forever — this
  # script hung a five-minute tool timeout exactly once, on the one run where the
  # page server was not already up and this branch actually executed.
  ( cd "$WEB/ui-web" && nohup python3 -m http.server "$PORT" --bind 127.0.0.1 \
      > /tmp/page.log 2>&1 < /dev/null & )
  sleep 1
fi

say "engine  pid $(pgrep -f daw_engine)"
say "sidecar pid $(pgrep -f "daw-sidecar.*$SHM")  ws 8174 state / 8175 commands"
say "page    http://127.0.0.1:$PORT/index.html"
head -2 /tmp/side.log | sed 's/^/  /'
