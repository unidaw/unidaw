#!/usr/bin/env bash
# ASKING THE MODEL TO EDIT THE SONG MUST ACTUALLY EDIT THE SONG.
#
# This is the feature Jaakko demos first — "ask the AI to generate a bassline, or add a track,
# make the music by prompting" — and until this file it had NO automated coverage at all. Every
# other part of the AI path is checked (the agent tools have unit tests, the wire has offset
# tests); the one thing nobody checked was whether asking works.
#
# WHAT IT ASSERTS, AND WHY IT IS NOT "THE MODEL REPLIED":
# The failure that matters is not silence from the API — that is loud and obvious. It is the model
# answering fluently while nothing reaches the engine: a renamed tool, a rejected command, a
# capability the agent no longer has. So the verdict here is the ENGINE'S TRACK COUNT, read back
# through daw-cli after the exchange finishes. `done` alone is not a pass.
#
# THE REPLY FIELD IS `agent`, NOT `type`. The sidecar's json_line() emits
# {"agent":"say"|"did"|"done"|"failed","text":...,"ok":...}. The first version of this probe read
# `type`, matched none of its own replies, ran out its deadline and reported the product broken
# when the product was fine. If you extend this, print the raw frame before believing a verdict.
#
# COST AND SKIPPING: one real API call per run, deliberately tiny. With no key resolvable this
# SKIPS rather than fails, so a machine without one still gets a green suite. A transport failure
# (DNS, refused, offline) also skips — that is the environment, not the product. An API-level
# refusal (401 bad key, 404 unknown model) FAILS, because those are exactly the breakages that
# would end the demo and they are indistinguishable from working until you ask.
#
#   tools/ask_path_check.sh
set -uo pipefail
if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'ask_path_check: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || exit 2
. "$SCRIPT_DIR/lib/repository_root.sh" || exit 2
ROOT="$(daw_repository_root)" || exit 2
unset BASH_ENV ENV NODE_OPTIONS NODE_PATH
unset PYTHONHOME PYTHONPATH
unset ASK_BUILD_DIR_INPUT ASK_ENV_FILE_INPUT ASK_EVIDENCE_ROOT ASK_API_KEY_INPUT
unset SIDECAR_API_KEY SIDECAR_ENV_FILE
ASK_BUILD_DIR_INPUT="${DAW_BUILD_DIR:-}"
ASK_ENV_FILE_INPUT="${DAW_ENV_FILE:-}"
ASK_EVIDENCE_ROOT="${DAW_CHECK_EVIDENCE:-}"
ASK_API_KEY_INPUT="${ANTHROPIC_API_KEY:-}"
while IFS= read -r daw_variable; do
  unset "$daw_variable"
done < <(compgen -v DAW_)
unset ANTHROPIC_API_KEY
export -n ASK_BUILD_DIR_INPUT ASK_ENV_FILE_INPUT ASK_EVIDENCE_ROOT ASK_API_KEY_INPUT 2>/dev/null || true
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3 for isolated port selection"; exit 0; }
python3() { command python3 -I "$@"; }
. "$ROOT/tools/lib/engine_wait.sh"
if [ -n "$ASK_BUILD_DIR_INPUT" ]; then
  BUILD="$(daw_canonical_directory "$ASK_BUILD_DIR_INPUT" 'explicit DAW_BUILD_DIR')" || exit 2
  printf 'build directory: %s (explicit DAW_BUILD_DIR)\n' "$BUILD" >&2
else
  BUILD="$(daw_canonical_directory "$ROOT/build" 'checkout build directory')" || exit 2
  daw_require_within_root "$BUILD" "$ROOT" 'checkout build directory' || exit 2
  daw_validate_cmake_build_source "$BUILD" "$ROOT" 'checkout build directory' || exit 2
  printf 'build directory: %s (checkout default)\n' "$BUILD" >&2
fi
SIDE="$ROOT/ui/target/release/daw-sidecar"
[ -x "$SIDE" ] || SIDE="$ROOT/ui/target/debug/daw-sidecar"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

ENGINE="$(daw_canonical_executable "$BUILD/daw_engine" 'selected daw_engine')" || { echo "build daw_engine first"; exit 2; }
HOST="$(daw_canonical_executable "$BUILD/juce_host_process" 'selected juce_host_process')" || { echo "build juce_host_process first"; exit 2; }
daw_require_within_root "$ENGINE" "$BUILD" 'selected daw_engine' || exit 2
daw_require_within_root "$HOST" "$BUILD" 'selected juce_host_process' || exit 2
SIDE="$(daw_canonical_executable "$SIDE" 'checkout daw-sidecar')" || { echo "build daw-sidecar first"; exit 2; }
CLI="$(daw_canonical_executable "$CLI" 'checkout daw-cli')" || { echo "build daw-cli first"; exit 2; }
daw_require_within_root "$SIDE" "$ROOT" 'checkout daw-sidecar' || exit 2
daw_require_within_root "$CLI" "$ROOT" 'checkout daw-cli' || exit 2
PLUGIN_CACHE="$BUILD/plugin_cache.json"
if [ -L "$PLUGIN_CACHE" ]; then
  echo "selected plugin cache is a symlink; refusing ambiguous artifact provenance"
  exit 2
fi
if [ -e "$PLUGIN_CACHE" ]; then
  PLUGIN_CACHE="$(daw_canonical_readable_file "$PLUGIN_CACHE" 'selected plugin cache')" || exit 2
  daw_require_within_root "$PLUGIN_CACHE" "$BUILD" 'selected plugin cache' || exit 2
fi
PROJECTS="$(daw_canonical_directory "$ROOT/presets/projects" 'checkout project fixtures')" || exit 2
daw_require_within_root "$PROJECTS" "$ROOT" 'checkout project fixtures' || exit 2
PATCHER_PRESETS="$(daw_canonical_directory "$ROOT/presets/patcher" 'checkout patcher presets')" || exit 2
daw_require_within_root "$PATCHER_PRESETS" "$ROOT" 'checkout patcher presets' || exit 2
command -v node >/dev/null 2>&1 || { echo "SKIP: no node, and the command socket is a websocket"; exit 0; }
NODE_MODULES="$ROOT/ui-web/node_modules"
[ -d "$NODE_MODULES" ] && [ ! -L "$NODE_MODULES" ] \
  || { echo "SKIP: checkout-local ui-web dependencies are not installed as a real directory"; exit 0; }
NODE_MODULES="$(daw_canonical_directory "$NODE_MODULES" 'ui-web dependency directory')" || exit 2
daw_require_within_root "$NODE_MODULES" "$ROOT" 'ui-web dependency directory' || exit 2
WS_ENTRY="$(cd "$ROOT/ui-web" && node -e "process.stdout.write(require('fs').realpathSync(require.resolve('ws')))" 2>/dev/null)" \
  || { echo "SKIP: checkout-local node 'ws' module not available"; exit 0; }
case "$WS_ENTRY" in
  "$NODE_MODULES"/*) ;;
  *) echo "FAIL: node 'ws' resolved outside checkout-local dependencies"; exit 2 ;;
esac

# Paid execution is explicit: environment key or named file only. The sidecar is
# launched from a run-owned cwd below, so its relative .env search cannot discover
# a checkout or home credential file accidentally.
SIDECAR_ENV_FILE=''
if [ -n "$ASK_ENV_FILE_INPUT" ]; then
  SIDECAR_ENV_FILE="$(daw_canonical_readable_file "$ASK_ENV_FILE_INPUT" 'explicit DAW_ENV_FILE')" || exit 2
fi
case "$ASK_API_KEY_INPUT" in
  *[![:space:]]*) ;;
  *) ASK_API_KEY_INPUT='' ;;
esac
key_present() {
  [ -n "$ASK_API_KEY_INPUT" ] && return 0
  [ -n "$SIDECAR_ENV_FILE" ] && daw_env_file_has_anthropic_key "$SIDECAR_ENV_FILE" && return 0
  return 1
}
key_present || { echo "SKIP: paid ask check requires explicit ANTHROPIC_API_KEY or DAW_ENV_FILE"; exit 0; }
SIDECAR_API_KEY="$ASK_API_KEY_INPUT"
ASK_API_KEY_INPUT=''
export -n SIDECAR_API_KEY SIDECAR_ENV_FILE 2>/dev/null || true

TMP="$(daw_make_temp_directory daw-ask-path)" || exit 2
SIDECAR_CWD="$TMP/runtime/sidecar/cwd"
mkdir -p -- "$SIDECAR_CWD" || exit 2
SIDECAR_CWD="$(daw_canonical_directory "$SIDECAR_CWD" 'run-owned sidecar cwd')" || exit 2
daw_require_within_root "$SIDECAR_CWD" "$TMP" 'run-owned sidecar cwd' || exit 2
SHM="/askpath_$$"
ENG=""; SC=""
cleanup() {
  [ -n "$SC" ]  && kill "$SC"  2>/dev/null
  [ -n "$ENG" ] && stop_engine "$ENG"
  daw_remove_temp_directory "$TMP" daw-ask-path
}
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local evidence_root
    local dest
    if [ -n "$ASK_EVIDENCE_ROOT" ]; then
      [ -d "$ASK_EVIDENCE_ROOT" ] && [ ! -L "$ASK_EVIDENCE_ROOT" ] || {
        echo "  evidence root override is not a real directory"
        "$@"
        exit "$rc"
      }
      evidence_root="$(daw_canonical_directory "$ASK_EVIDENCE_ROOT" 'explicit evidence root')" || {
        "$@"
        exit "$rc"
      }
      dest="$(mktemp -d "$evidence_root/ask-path-check.XXXXXXXX")" || dest=''
    else
      dest="$(daw_make_temp_directory daw-check-evidence)" || dest=''
    fi
    if [ -n "$dest" ] && cp -R "$TMP"/. "$dest"/ 2>/dev/null; then
      echo "  evidence kept in $dest"
    fi
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# Two free ports from the OS rather than fixed numbers, so a running web stack does not collide.
read -r STATE_PORT CMD_PORT <<EOF
$(python3 -c "
import socket
def p():
    s=socket.socket(); s.bind(('127.0.0.1',0)); n=s.getsockname()[1]; s.close(); return n
print(p(), p())")
EOF
[ -n "$CMD_PORT" ] || fail "could not pick a free port"

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$PROJECTS" \
    DAW_HOST_BINARY="$HOST" DAW_PLUGIN_CACHE="$PLUGIN_CACHE" \
    DAW_PATCHER_PRESET_DIR="$PATCHER_PRESETS" \
    "$ENGINE" --run-seconds 240 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 "UI: command thread started" \
  || fail "engine never came up:
$(tail -8 "$TMP/eng.log" | sed 's/^/          /')"

( cd "$SIDECAR_CWD" && exec env ANTHROPIC_API_KEY="$SIDECAR_API_KEY" \
    DAW_ENV_FILE="$SIDECAR_ENV_FILE" DAW_PROJECT_DIR="$PROJECTS" \
    "$SIDE" --shm "$SHM" --port "$STATE_PORT" --cmd-port "$CMD_PORT" \
      --plugin-cache "$PLUGIN_CACHE" >"$TMP/side.log" 2>&1 ) &
SC=$!
port_open() { nc -z 127.0.0.1 "$CMD_PORT" 2>/dev/null; }
wait_until 60 port_open || fail "the sidecar never opened its command port:
$(tail -8 "$TMP/side.log" | sed 's/^/          /')"

# PARSE track_count, do not count lines. A first version piped `get tracks` through `grep -c .`,
# which counts LINES OF JSON: it reported "8 tracks" for a two-track song and only worked as a
# change detector by the accident that one added track prints one added line. A number in a
# failure message that is not the number it claims to be sends the next person somewhere else.
count_tracks() {
  env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$PROJECTS" "$CLI" get tracks 2>/dev/null \
    | sed -n 's/.*"track_count"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1
}
BEFORE=$(count_tracks); BEFORE=${BEFORE:-0}
[ "$BEFORE" -gt 0 ] || fail "read no track_count before asking; the engine is not published yet,
        so any change measured after the ask would be meaningless."
echo "  tracks before: $BEFORE"

( cd "$ROOT/ui-web" && node - "$CMD_PORT" ) >"$TMP/ask.out" 2>&1 <<'JS'
const WebSocket = require('ws');
const ws = new WebSocket(`ws://127.0.0.1:${process.argv[2]}`);
// A hard deadline, because the sidecar puts NO timeout on its HTTP call: a hung API request
// would otherwise hang this check exactly as it hangs the ask box in front of an audience.
const deadline = setTimeout(() => { console.log('VERDICT:TIMEOUT'); process.exit(3); }, 120000);
ws.on('open', () => ws.send(JSON.stringify({
  type: 'ask', text: 'Add one new audio track named Bass. Do nothing else.' })));
ws.on('message', (m) => {
  const s = m.toString();
  let j = null; try { j = JSON.parse(s); } catch (_) {}
  const t = j && j.agent;                       // `agent`, not `type` — see the header
  if (!t) { console.log('RAW ' + s.slice(0, 200)); return; }
  const text = (j.text || '').replace(/\s+/g, ' ').slice(0, 240);
  if (t === 'done')       { console.log('VERDICT:DONE ' + text); clearTimeout(deadline); process.exit(0); }
  else if (t === 'failed'){ console.log('VERDICT:FAILED ' + text); clearTimeout(deadline); process.exit(1); }
  else console.log(t.toUpperCase() + ' ' + text);
});
ws.on('error', (e) => { console.log('VERDICT:WSERROR ' + e.message); process.exit(4); });
JS
RC=$?
sed 's/^/    /' "$TMP/ask.out"

# A TRANSPORT failure is the environment; an API REFUSAL is the product. Only one of them is red.
if grep -q 'VERDICT:FAILED' "$TMP/ask.out"; then
  MSG=$(grep 'VERDICT:FAILED' "$TMP/ask.out" | head -1)
  case "$MSG" in
    *"dns"*|*"Dns"*|*"connect"*|*"Connect"*|*"transport"*|*"network"*|*"timed out"*|*"io error"*)
      echo "ask_path_check: SKIP — the request never reached the API (environment, not the product):"
      echo "    $MSG"
      exit 0 ;;
  esac
  fail "asking the model failed at the API:
        $MSG

        401/permission means the key is wrong or expired; 404 means MODEL in
        ui/daw-sidecar/src/ask.rs names a model this key cannot reach. Both look exactly like a
        working system until somebody asks, which is why this check exists."
fi
[ "$RC" -eq 3 ] && fail "no verdict within 120s. ask.rs makes its HTTP call with no timeout, so a
        hung request hangs the ask box with no feedback at all."
[ "$RC" -eq 4 ] && fail "could not talk to the sidecar's command socket at all."
[ "$RC" -eq 0 ] || fail "the ask ended with no 'done' (rc=$RC)."

AFTER=$(count_tracks); AFTER=${AFTER:-0}
echo "  tracks after:  $AFTER"
if [ "$AFTER" -le "$BEFORE" ]; then
  fail "the model answered but THE SONG DID NOT CHANGE ($BEFORE tracks before, $AFTER after).

        This is the failure this check exists for. The exchange reads perfectly — 'say', 'did',
        'done' all arrive — while the edit never reaches the engine, because a tool was renamed,
        the command was refused, or the agent lost the capability. Read the DID lines above: an
        ok=false there names the tool that was refused. On the demo this looks like the DAW
        ignoring Jaakko while the model cheerfully agrees with him."
fi

echo "ask_path_check: PASS — asking the model to add a track added a track ($BEFORE -> $AFTER)"
