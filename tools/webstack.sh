#!/usr/bin/env bash
# Start exactly one engine + one sidecar + one page server, or report why not.
#
# Written because "is the engine running?" has been answered wrongly three times
# in this project: once by a `ps` pattern that matched nothing (the engine runs as
# ./daw_engine, not build/daw_engine), and twice by two engines racing on the same
# shared-memory segment, which produces frozen frames that look exactly like a
# healthy stopped transport.
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
pkill -f 'juce_host_process' 2>/dev/null || true
sleep 1

[ -x "$ENGINE" ] || { say "no engine at $ENGINE — run: cmake --build build --target daw_engine"; exit 1; }
[ -x "$HOST" ] || { say "no host at $HOST — run: cmake --build build --target juce_host_process"; exit 1; }
cd "$RUNDIR"
DAW_UI_SHM_NAME=$SHM DAW_PROJECT_DIR=$PROJECTS \
  DAW_HOST_BINARY=$HOST \
  "$ENGINE" "$@" > /tmp/eng.log 2>&1 &
ENGINE_PID=$!
echo "$ENGINE_PID" > "$PIDFILE"
sleep 6
alive "$ENGINE_PID" || { say "engine exited during startup:"; tail -5 /tmp/eng.log; exit 1; }

cd "$WEB/ui"
DAW_PROJECT_DIR=$PROJECTS ./target/release/daw-sidecar --shm "$SHM" > /tmp/side.log 2>&1 &
SIDECAR_PID=$!
echo "$SIDECAR_PID" >> "$PIDFILE"
sleep 2
grep -q 'attached to' /tmp/side.log || { say "sidecar did not attach:"; head -3 /tmp/side.log; exit 1; }

if ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  cd "$WEB/ui-web" && python3 -m http.server "$PORT" --bind 127.0.0.1 > /tmp/page.log 2>&1 &
  sleep 1
fi

say "engine  pid $(pgrep -f daw_engine)"
say "sidecar pid $(pgrep -f "daw-sidecar.*$SHM")  ws 8174 state / 8175 commands"
say "page    http://127.0.0.1:$PORT/index.html"
head -2 /tmp/side.log | sed 's/^/  /'
