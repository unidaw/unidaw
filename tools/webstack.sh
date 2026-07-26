#!/usr/bin/env bash
# Start exactly one engine + one sidecar + one page server, or report why not.
#
# Written because "is the engine running?" has been answered wrongly three times
# in this project: once by a `ps` pattern that matched nothing (the engine runs as
# ./daw_engine, not build/daw_engine), and twice by two engines racing on the same
# shared-memory segment, which produces frozen frames that look exactly like a
# healthy stopped transport.
set -euo pipefail

# This worktree's own build, NOT /Users/jak/src/daw/build. That one is the backend
# agent's working tree: building there compiles whatever they have uncommitted, so
# the engine's SHM version can jump ahead of the daw-bridge this branch is pinned
# to and the sidecar refuses to attach. Build what we have merged.
WEB=/Users/jak/src/daw-web
REPO=$WEB
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
pkill -f 'juce_host_process' 2>/dev/null || true

cd "$REPO/build"
DAW_UI_SHM_NAME=$SHM DAW_PROJECT_DIR=$PROJECTS \
  DAW_HOST_BINARY=$REPO/build/juce_host_process \
  ./daw_engine "$@" > /tmp/eng.log 2>&1 &
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
