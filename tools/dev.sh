#!/usr/bin/env bash
#
# ONE COMMAND: build the latest, start the stack, open the page, and Ctrl-C to stop all of it.
#
# This exists because of a specific failure that cost three wrong conclusions in one day. A
# running stack keeps the binary it STARTED with. Rebuild while it is up and the process on the
# machine is older than the code on disk — so a fix lands, the tests go green, and the app in
# front of you still has the bug. Every time it happened the reaction was to doubt the fix.
#
# `webstack.sh` is deliberately unchanged: it starts a stack and exits, which is what the harness
# and demo-stack-smoke need. This wraps it for a PERSON: build first, always; open the page; and
# hold the terminal so Ctrl-C is a stop button rather than something you have to go hunting for.
#
#   tools/dev.sh              build, start, open, wait
#   tools/dev.sh --no-build   skip the build (when you know nothing changed)
#   tools/dev.sh --no-open    do not open a browser
#
# Arrow-up and run it again to get the latest: the build is inside the command, so "the latest"
# is not something you have to remember to do first.
set -u

WEB=$(cd "$(dirname "$0")/.." && pwd)
cd "$WEB"

PORT=${PORT:-8173}
SHM=${SHM:-/daw_web_ui}
SEG=$(printf '%s' "$SHM" | tr -c 'A-Za-z0-9' '_')
PIDFILE=/tmp/uni-web-stack$SEG.pids
URL="http://127.0.0.1:$PORT/index.html"

BUILD=1
OPEN=1
for a in "$@"; do
  case "$a" in
    --no-build) BUILD=0 ;;
    --no-open)  OPEN=0 ;;
    *) echo "dev.sh: unknown option $a" >&2; exit 2 ;;
  esac
done

say() { printf '\033[36m>\033[0m %s\n' "$*"; }

# ── STOP EVERYTHING, ON Ctrl-C OR ON THE WAY OUT ────────────────────────────────────────────
#
# The pidfile holds the engine and the sidecar. The PAGE SERVER is not in it — webstack.sh starts
# it in a subshell without recording the pid — so it is matched by what it is instead. Without
# this, Ctrl-C leaves a server on the port and the next run of dev.sh finds the port taken.
stop() {
  trap - INT TERM EXIT
  echo
  say "stopping the stack"
  if [ -f "$PIDFILE" ]; then
    while read -r pid; do
      [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done < "$PIDFILE"
  fi
  # The page server, by its own command line. Narrow on purpose: `serve.mjs <this port>` cannot
  # match another agent's stack, a sweep, or this script.
  pkill -f "serve\.mjs $PORT" 2>/dev/null
  # A moment to go quietly, then insist. An engine that ignores TERM holds the shared-memory
  # segment, and the next start refuses rather than joining it — which reads as "the app is
  # broken" when it is only the last one still exiting.
  sleep 1
  if [ -f "$PIDFILE" ]; then
    while read -r pid; do
      [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
    done < "$PIDFILE"
  fi
  say "stopped"
  exit 0
}
trap stop INT TERM

# ── BUILD FIRST, ALWAYS ─────────────────────────────────────────────────────────────────────
#
# RELEASE, because that is what the stack and every suite actually load — `cargo build` and
# `cargo test` default to debug, so the ordinary loop never refreshes the binaries that run.
if [ "$BUILD" = "1" ]; then
  say "building (release)"
  # ONE build, and its OWN exit status. Piping cargo into grep would report GREP's status, so a
  # failed build reads as success — and a failed build leaves the previous binary in place and
  # running, which is the quietest possible way to test code nobody compiled. The output is kept
  # so it can be shown only when it matters.
  out=$(cargo build --release --manifest-path ui/Cargo.toml 2>&1)
  if [ $? -ne 0 ]; then
    say "BUILD FAILED — not starting. The old binaries are still on disk and would have run."
    printf '%s\n' "$out" | tail -30
    exit 1
  fi
  printf '%s\n' "$out" | grep -E "^warning: unused" -A 3 || true
fi

# ── START ───────────────────────────────────────────────────────────────────────────────────
say "starting the stack on $SHM port $PORT"
DAW_ENV_FILE="${DAW_ENV_FILE:-$WEB/.env}" KEEP_ENGINE=1 tools/webstack.sh || {
  say "webstack.sh failed to start"
  exit 1
}

# WHAT IS ACTUALLY RUNNING, stated plainly. The whole reason this script exists is that a stack
# can be older than the source; printing the binary's timestamp makes that visible instead of
# something you deduce after an hour of doubting a fix.
if [ -f ui/target/release/daw-sidecar ]; then
  say "sidecar built $(stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' ui/target/release/daw-sidecar)"
fi

[ "$OPEN" = "1" ] && open "$URL"

say "page $URL"
say "Ctrl-C stops the engine, the sidecar and the page server"

# Hold the terminal. `wait` alone would return immediately — every child is detached by
# webstack.sh — so this parks on a sleep the trap can interrupt.
while true; do sleep 3600 & wait $!; done
