#!/usr/bin/env bash
# REHEARSE THE DEMO BY PROMPTING IT, THE WAY IT WILL BE DRIVEN ON THE DAY.
#
# NOT REGISTERED IN ctest, on purpose: it makes a handful of real API calls, and a suite that
# costs money every time somebody runs it will be disabled by whoever is paying. The path itself
# is covered on every run by tools/ask_path_check.sh, which is deliberately one small call. This
# is the wider rehearsal you run before a demo, by hand:
#
#   ./tools/demo_rehearsal.sh
#
# WHAT IT CAN AND CANNOT TELL YOU. It drives the sidecar's command socket — the same socket the
# browser drives — so it proves each step is ACHIEVABLE BY PROMPTING against a real engine. It
# does NOT open a browser, so it says nothing about whether the page renders the agent log, the
# piano roll or the harmony lane. That leg still needs a human at the screen.
#
# Each step asserts the ENGINE CHANGED, never that the model sounded confident. A stubbed
# add_track that sent nothing still produced "Done! I've added a new audio track named Bass
# (track 2)" — the prose is not evidence.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
SIDE="$ROOT/ui/target/release/daw-sidecar"
[ -x "$SIDE" ] || SIDE="$ROOT/ui/target/debug/daw-sidecar"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$SIDE" ] || { echo "build daw-sidecar first"; exit 2; }
node -e "require.resolve('ws')" >/dev/null 2>&1 || { echo "needs node's 'ws' module"; exit 2; }

SIDECAR_CWD=/Users/jak/src/daw-web/ui
[ -d "$SIDECAR_CWD" ] || SIDECAR_CWD="$ROOT/ui"

TMP="$(mktemp -d)"
SHM="/demoreh_$$"
ENG=""; SC=""
cleanup() {
  [ -n "$SC" ]  && kill "$SC"  2>/dev/null
  [ -n "$ENG" ] && stop_engine "$ENG"
  echo "  logs kept in $TMP"
}
trap cleanup EXIT

read -r STATE_PORT CMD_PORT <<EOF
$(python3 -c "
import socket
def p():
    s=socket.socket(); s.bind(('127.0.0.1',0)); n=s.getsockname()[1]; s.close(); return n
print(p(), p())")
EOF

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    ./daw_engine --run-seconds 900 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started" \
  || { echo "engine never came up"; tail -8 "$TMP/eng.log"; exit 1; }

( cd "$SIDECAR_CWD" && exec env DAW_ENV_FILE="${DAW_ENV_FILE:-}" \
    "$SIDE" --shm "$SHM" --port "$STATE_PORT" --cmd-port "$CMD_PORT" >"$TMP/side.log" 2>&1 ) &
SC=$!
port_open() { nc -z 127.0.0.1 "$CMD_PORT" 2>/dev/null; }
wait_until 60 port_open || { echo "sidecar never opened its command port"; tail -8 "$TMP/side.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@" 2>/dev/null; }

# One prompt, one connection, printed as it streams. Returns non-zero if the model reports failure.
ask() {
  node - "$CMD_PORT" "$1" <<'JS'
const WebSocket = require('ws');
const [,, port, prompt] = process.argv;
const ws = new WebSocket(`ws://127.0.0.1:${port}`);
const deadline = setTimeout(() => { console.log('    TIMEOUT'); process.exit(3); }, 180000);
ws.on('open', () => ws.send(JSON.stringify({ type: 'ask', text: prompt })));
ws.on('message', (m) => {
  let j = null; try { j = JSON.parse(m.toString()); } catch (_) { return; }
  const t = j.agent, text = (j.text || '').replace(/\s+/g, ' ');
  if (t === 'did')  console.log('    did: ' + text.slice(0, 90) + '  ok=' + j.ok);
  else if (t === 'done')   { clearTimeout(deadline); process.exit(0); }
  else if (t === 'failed') { console.log('    FAILED: ' + text.slice(0,200)); clearTimeout(deadline); process.exit(1); }
});
ws.on('error', (e) => { console.log('    WS ERROR ' + e.message); process.exit(4); });
JS
}

PASS=0; FAIL=0
step() {  # step "<label>" "<prompt>" "<measure cmd>"  — passes when the measurement CHANGES
  local label="$1" prompt="$2" measure="$3"
  local before after
  before=$(eval "$measure")
  echo "  [$label]"
  ask "$prompt"
  local rc=$?
  after=$(eval "$measure")
  if [ "$rc" -ne 0 ]; then
    echo "    RESULT: FAIL — the ask itself did not complete"; FAIL=$((FAIL+1)); return
  fi
  if [ "$before" = "$after" ]; then
    echo "    RESULT: FAIL — the model answered but the song did not change ($measure: $before)"
    FAIL=$((FAIL+1))
  else
    echo "    RESULT: pass ($before -> $after)"; PASS=$((PASS+1))
  fi
}

# Parse the published fields, never line counts — `grep -c .` on `get tracks` counts JSON lines
# and calls them tracks, which is a wrong number in a failure message.
n_tracks() {
  cli get tracks | sed -n 's/.*"track_count"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1
}
# Each track publishes its head-of-chain device NAME. A sampler landing on a track is therefore
# visible as that name appearing, which is what "put a sampler on it" has to mean to be real.
n_samplers(){ cli get tracks | grep -ci '"device": *"[Ss]ampler"' ; }
n_notes()  { cli get notes 2>/dev/null | grep -c '"pitch"' ; }

echo "REHEARSING — each step is one prompt, judged by whether the ENGINE changed."
echo
step "add a track"        "Add a new track named Bass."                     n_tracks
step "put a sampler on it" "Put a sampler on the track named Bass."         n_samplers
step "write a bassline"   "Write a simple four-bar bassline on the track named Bass, root notes on the beat." n_notes

echo
echo "rehearsed: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
