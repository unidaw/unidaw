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

if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'webstack: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || exit 2
. "$SCRIPT_DIR/lib/repository_root.sh" || exit 2
ROOT="$(daw_repository_root)" || exit 2
unset BASH_ENV ENV NODE_OPTIONS NODE_PATH PYTHONHOME PYTHONPATH CARGO_TARGET_DIR
unset SIDECAR_API_KEY SIDECAR_ENV_FILE
# Ambient DAW runtime controls can redirect output, sockets, caches, fixtures,
# test modes, or timing in ways this launcher never reported. Keep only the
# three documented namespaced launcher inputs and the explicit credential-file
# channel until each is validated and captured below. Every engine/sidecar DAW
# value is then pinned at its individual exec site.
while IFS= read -r daw_variable; do
  case "$daw_variable" in
    DAW_WEBSTACK_ENGINE|DAW_WEBSTACK_HOST|DAW_WEBSTACK_ALLOW_CREDENTIALS|DAW_ENV_FILE) ;;
    *) unset "$daw_variable" ;;
  esac
done < <(compgen -v DAW_)
say() { printf '  %s\n' "$*"; }

UI_WEB="$(daw_canonical_directory "$ROOT/ui-web" 'checkout web source')" \
  || { say "checkout-local web source is missing"; exit 2; }
daw_require_within_root "$UI_WEB" "$ROOT" 'checkout web source' || exit 2

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }

canonical_port() {
  local value="$1"
  local label="$2"
  local canonical
  case "$value" in
    ''|*[!0-9]*) say "REFUSING TO START: $label must be a decimal TCP port" >&2; return 1 ;;
  esac
  [ "${#value}" -le 5 ] \
    || { say "REFUSING TO START: $label is outside the TCP port range" >&2; return 1; }
  canonical=$((10#$value))
  [ "$canonical" -ge 1 ] && [ "$canonical" -le 65535 ] \
    || { say "REFUSING TO START: $label is outside the TCP port range" >&2; return 1; }
  printf '%s\n' "$canonical"
}

# `lsof` exits 1 when there are no matching listeners. Under `set -e` plus
# `pipefail`, returning that ordinary absence from a command substitution aborts
# the launcher before it can enter the page-server start branch. Empty output is
# a successful answer here; callers decide whether emptiness is expected.
page_listener_pids() {
  lsof -nP -iTCP:"$PORT" -sTCP:LISTEN -t 2>/dev/null | sort -u || true
}

page_server_matches_checkout() {
  local pid="$1"
  local cwd
  cwd="$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | sed -n 's/^n//p' | head -1)"
  [ -n "$cwd" ] || return 1
  cwd="$(daw_canonical_directory "$cwd" 'page-server working directory' 2>/dev/null)" || return 1
  [ "$cwd" = "$UI_WEB" ] || return 1
  ps -o command= -p "$pid" 2>/dev/null | grep -Fq 'test/serve.mjs' || return 1
  curl -fsSI --max-time 3 "http://127.0.0.1:$PORT/index.html" 2>/dev/null \
    | grep -Eiq '^Cache-Control:[[:space:]]*no-store([,;[:space:]]|$)' || return 1
  curl -fsS --max-time 3 "http://127.0.0.1:$PORT/index.html" 2>/dev/null \
    | cmp -s - "$UI_WEB/index.html"
}

sidecar_listener_pids() {
  local port="$1"
  lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null | sort -u || true
}

wait_for_sidecar_listener() {
  local port="$1" pid="$2" attempt listeners
  for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    listeners="$(sidecar_listener_pids "$port")"
    [ "$listeners" = "$pid" ] && return 0
    sleep 0.25
  done
  return 1
}

file_mode() {
  command stat -f '%Lp' "$1" 2>/dev/null || command stat -c '%a' "$1" 2>/dev/null
}

# READY locators intentionally outlive the launcher so the smoke check can read
# the running stack's log. Before replacing a segment, retire only locators that
# satisfy the complete producer contract. This prevents an old numeric PID from
# becoming authoritative again after OS PID reuse. Invalid lookalikes are never
# followed or removed; a valid locator that cannot be unlinked blocks startup.
retire_prior_ready_states() {
  local candidate
  local canonical_dir
  local canonical_state
  local name
  local retired=0
  local state
  local suffix
  for candidate in "$TEMP_ROOT"/daw-webstack-log.*; do
    [ -e "$candidate" ] || continue
    [ -d "$candidate" ] && [ ! -L "$candidate" ] && [ -O "$candidate" ] || continue
    name="$(basename -- "$candidate")"
    suffix="${name#daw-webstack-log.}"
    [ "${#suffix}" = "8" ] || continue
    case "$suffix" in *[!A-Za-z0-9]*) continue ;; esac
    [ "$(file_mode "$candidate")" = "700" ] || continue
    canonical_dir="$(daw_canonical_directory "$candidate" 'prior webstack log directory' 2>/dev/null)" \
      || continue
    [ "$canonical_dir" = "$candidate" ] && [ "$(dirname -- "$canonical_dir")" = "$TEMP_ROOT" ] \
      || continue
    state="$canonical_dir/uni-web-stack$SEG.state"
    [ -f "$state" ] && [ ! -L "$state" ] && [ -O "$state" ] || continue
    [ "$(file_mode "$state")" = "600" ] || continue
    canonical_state="$(daw_canonical_readable_file "$state" 'prior ready locator' 2>/dev/null)" \
      || continue
    [ "$canonical_state" = "$state" ] || continue
    daw_require_within_root "$canonical_state" "$canonical_dir" 'prior ready locator' \
      >/dev/null 2>&1 || continue
    LC_ALL=C awk -v segment="$SEG" -v log_dir="$canonical_dir" \
      -v engine_log="$canonical_dir/engine.log" '
        NR == 1 { ok = ($0 == "DAW_WEBSTACK_STATE=1") }
        NR == 2 { ok = ok && ($0 == "READY=1") }
        NR == 3 { ok = ok && ($0 == "SEG=" segment) }
        NR == 4 { ok = ok && ($0 == "LOG_DIR=" log_dir) }
        NR == 5 { ok = ok && ($0 == "ENGINE_LOG=" engine_log) }
        NR == 6 { ok = ok && ($0 ~ /^ENGINE_PID=[1-9][0-9]*$/) }
        NR > 6 { ok = 0 }
        END { exit !(ok && NR == 6) }
      ' "$canonical_state" || continue
    rm -f -- "$canonical_state" \
      || { say "REFUSING TO START: cannot retire the prior valid READY locator"; return 1; }
    retired=$((retired + 1))
  done
  [ "$retired" = "0" ] || say "retired $retired prior READY locator(s) for $SHM"
}

webstack_ready_retirement_selftest() {
  local first
  local foreign
  local linked
  local second
  local target
  local test_root

  test_root="$(daw_make_temp_directory daw-webstack-retirement-test)" || return 2
  retirement_selftest_cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if [ -n "${test_root:-}" ] && [ -d "$test_root" ] && [ ! -L "$test_root" ]; then
      daw_remove_temp_directory "$test_root" daw-webstack-retirement-test || rc=1
    fi
    exit "$rc"
  }
  trap retirement_selftest_cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  TEMP_ROOT="$(daw_canonical_directory "$test_root" 'retirement self-test root')" || return 2
  SHM='/retirement_test'
  SEG='_retirement_test'
  first="$TEMP_ROOT/daw-webstack-log.Ab12Cd34"
  second="$TEMP_ROOT/daw-webstack-log.Ef56Gh78"
  foreign="$TEMP_ROOT/daw-webstack-log.Ij90Kl12"
  linked="$TEMP_ROOT/daw-webstack-log.Mn34Op56"
  mkdir -- "$first" "$second" "$foreign" "$linked" || return 2
  chmod 700 "$first" "$second" "$foreign" "$linked" || return 2

  write_ready_fixture() {
    local directory="$1"
    local segment="$2"
    local state="$directory/uni-web-stack$SEG.state"
    ( umask 077
      printf 'DAW_WEBSTACK_STATE=1\nREADY=1\nSEG=%s\nLOG_DIR=%s\nENGINE_LOG=%s/engine.log\nENGINE_PID=%s\n' \
        "$segment" "$directory" "$directory" "$3" > "$state"
    ) || return 1
    chmod 600 "$state" || return 1
  }
  write_ready_fixture "$first" "$SEG" 41001 || return 2
  write_ready_fixture "$second" "$SEG" 41002 || return 2
  write_ready_fixture "$foreign" '_foreign_segment' 41003 || return 2
  target="$TEMP_ROOT/do-not-remove"
  ( umask 077; printf '%s\n' 'symlink target must survive' > "$target" ) || return 2
  ln -s -- "$target" "$linked/uni-web-stack$SEG.state" || return 2

  retire_prior_ready_states || return 1
  [ ! -e "$first/uni-web-stack$SEG.state" ] \
    && [ ! -e "$second/uni-web-stack$SEG.state" ] \
    && [ -f "$foreign/uni-web-stack$SEG.state" ] \
    && [ -L "$linked/uni-web-stack$SEG.state" ] \
    && [ -f "$target" ] \
    || { say 'SELF-TEST FAIL: retirement removed the wrong locator or retained prior authority'; return 1; }
  say 'SELF-TEST PASS: prior same-segment READY locators retired; foreign and symlink lookalikes preserved'
}

webstack_free_port_selftest() {
  command -v lsof >/dev/null 2>&1 \
    || { say "SELF-TEST FAIL: lsof is required"; return 2; }
  command -v curl >/dev/null 2>&1 \
    || { say "SELF-TEST FAIL: curl is required"; return 2; }
  command -v node >/dev/null 2>&1 \
    || { say "SELF-TEST FAIL: node is required"; return 2; }

  PORT="$(env -u ANTHROPIC_API_KEY -u DAW_ENV_FILE node -e '
    const net = require("node:net");
    const server = net.createServer();
    server.on("error", (error) => { console.error(error.message); process.exit(1); });
    server.listen(0, "127.0.0.1", () => {
      const port = server.address().port;
      server.close(() => process.stdout.write(String(port)));
    });
  ')" || return 2
  case "$PORT" in
    ''|*[!0-9]*) say "SELF-TEST FAIL: OS did not return a numeric loopback port"; return 1 ;;
  esac

  # This is deliberately the exact production assignment that regressed. The
  # selected port has just been released by the OS and therefore has no listener.
  PAGE_LISTENERS="$(page_listener_pids)"
  [ -z "$PAGE_LISTENERS" ] \
    || { say "SELF-TEST FAIL: selected page port $PORT is already occupied"; return 1; }

  SELFTEST_TMP="$(daw_make_temp_directory daw-webstack-free-port)" || return 2
  SELFTEST_PAGE_PID=''
  selftest_cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if alive "$SELFTEST_PAGE_PID"; then
      kill "$SELFTEST_PAGE_PID" 2>/dev/null || true
    fi
    [ -z "$SELFTEST_PAGE_PID" ] || wait "$SELFTEST_PAGE_PID" 2>/dev/null || true
    if [ -n "${SELFTEST_TMP:-}" ] && [ -d "$SELFTEST_TMP" ]; then
      daw_remove_temp_directory "$SELFTEST_TMP" daw-webstack-free-port || rc=1
    fi
    exit "$rc"
  }
  trap selftest_cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  ( cd "$UI_WEB" && exec env -u ANTHROPIC_API_KEY -u DAW_ENV_FILE node test/serve.mjs "$PORT" ) \
    > "$SELFTEST_TMP/page.log" 2>&1 < /dev/null &
  SELFTEST_PAGE_PID=$!
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    sleep 0.1
    PAGE_LISTENERS="$(page_listener_pids)"
    [ "$PAGE_LISTENERS" = "$SELFTEST_PAGE_PID" ] && break
    alive "$SELFTEST_PAGE_PID" || break
  done
  [ "$PAGE_LISTENERS" = "$SELFTEST_PAGE_PID" ] || {
    say "SELF-TEST FAIL: checkout page server pid $SELFTEST_PAGE_PID did not become the sole listener on $PORT"
    sed -n '1,5p' "$SELFTEST_TMP/page.log" | sed 's/^/  /'
    return 1
  }
  page_server_matches_checkout "$SELFTEST_PAGE_PID" || {
    say "SELF-TEST FAIL: checkout page readiness/provenance check failed"
    return 1
  }
  say "SELF-TEST PASS: free page port $PORT crossed the empty-listener path, checkout server pid $SELFTEST_PAGE_PID bound it, and checkout bytes matched"
}

if [ "${1:-}" = "--self-test-free-port" ]; then
  [ "$#" = "1" ] || { say "usage: tools/webstack.sh --self-test-free-port"; exit 2; }
  webstack_free_port_selftest
  exit $?
fi

if [ "${1:-}" = "--self-test-ready-retirement" ]; then
  [ "$#" = "1" ] || { say "usage: tools/webstack.sh --self-test-ready-retirement"; exit 2; }
  webstack_ready_retirement_selftest
  exit $?
fi

[ -f "$ROOT/ui/Cargo.toml" ] || { say "checkout-local Rust workspace is missing"; exit 2; }
PROJECTS="$(daw_canonical_directory "$ROOT/presets/projects" 'checkout project fixtures')" \
  || { say "checkout-local project fixtures are missing"; exit 2; }
SIDECAR_SOURCE="$(daw_canonical_directory "$ROOT/ui/daw-sidecar" 'checkout sidecar source')" \
  || { say "checkout-local sidecar source is missing"; exit 2; }
daw_require_within_root "$PROJECTS" "$ROOT" 'checkout project fixtures' || exit 2
daw_require_within_root "$SIDECAR_SOURCE" "$ROOT" 'checkout sidecar source' || exit 2
PATCHER_PRESETS="$(daw_canonical_directory "$ROOT/presets/patcher" 'checkout patcher presets')" \
  || { say "checkout-local patcher presets are missing"; exit 2; }
daw_require_within_root "$PATCHER_PRESETS" "$ROOT" 'checkout patcher presets' || exit 2

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
# Legacy names were once the public override. Silently ignoring them would run a
# different binary than the caller requested, which is a provenance failure.
if [ -n "${ENGINE:-}" ] || [ -n "${HOST:-}" ]; then
  say "REFUSING TO START: legacy ENGINE/HOST overrides are unsupported"
  say "use explicit DAW_WEBSTACK_ENGINE/DAW_WEBSTACK_HOST overrides"
  exit 2
fi
unset ENGINE HOST

# Deliberate external artifacts remain supported only through namespaced,
# canonicalized and labeled overrides. Source and fixtures stay in ROOT.
if [ -n "${DAW_WEBSTACK_ENGINE:-}" ]; then
  ENGINE="$DAW_WEBSTACK_ENGINE"
  ENGINE_LABEL='explicit DAW_WEBSTACK_ENGINE override'
else
  ENGINE="$ROOT/build/daw_engine"
  ENGINE_LABEL='checkout default'
fi
ENGINE="$(daw_canonical_executable "$ENGINE" "$ENGINE_LABEL")" || exit 2
if [ "$ENGINE_LABEL" = 'checkout default' ]; then
  daw_require_within_root "$ENGINE" "$ROOT" 'checkout-default engine' || exit 2
  daw_validate_cmake_build_source "$(dirname -- "$ENGINE")" "$ROOT" 'checkout-default build directory' || exit 2
fi
if [ -n "${DAW_WEBSTACK_HOST:-}" ]; then
  HOST="$DAW_WEBSTACK_HOST"
  HOST_LABEL='explicit DAW_WEBSTACK_HOST override'
else
  HOST="$ROOT/build/juce_host_process"
  HOST_LABEL='checkout default'
fi
HOST="$(daw_canonical_executable "$HOST" "$HOST_LABEL")" || exit 2
if [ "$HOST_LABEL" = 'checkout default' ]; then
  daw_require_within_root "$HOST" "$ROOT" 'checkout-default plugin host' || exit 2
  daw_validate_cmake_build_source "$(dirname -- "$HOST")" "$ROOT" 'checkout-default host build directory' || exit 2
fi
RUNDIR="$(dirname -- "$ENGINE")"
SHM=${SHM:-/daw_web_ui}
SHM_LEAF=${SHM#/}
case "$SHM" in
  /*) ;;
  *) say "REFUSING TO START: SHM must have one leading slash"; exit 2 ;;
esac
case "$SHM_LEAF" in
  ''|*[!A-Za-z0-9_]*)
    say "REFUSING TO START: SHM must contain only a leading slash, letters, digits, and underscores"
    exit 2
    ;;
esac
[ "${#SHM_LEAF}" -le 120 ] \
  || { say "REFUSING TO START: SHM name is too long"; exit 2; }
PORT="$(canonical_port "${PORT:-8173}" PORT)" || exit 2
WS_STATE="$(canonical_port "${WS_STATE:-$((PORT + 1))}" WS_STATE)" || exit 2
WS_CMD="$(canonical_port "${WS_CMD:-$((PORT + 2))}" WS_CMD)" || exit 2
[ "$WS_STATE" = "$((PORT + 1))" ] && [ "$WS_CMD" = "$((PORT + 2))" ] \
  || { say "REFUSING TO START: WS_STATE and WS_CMD must be derived from PORT (+1/+2)"; exit 2; }
PLUGIN_CACHE="$RUNDIR/plugin_cache.json"
if [ -L "$PLUGIN_CACHE" ]; then
  say "selected plugin cache is a symlink; refusing ambiguous artifact provenance"
  exit 2
fi
if [ -e "$PLUGIN_CACHE" ]; then
  PLUGIN_CACHE="$(daw_canonical_readable_file "$PLUGIN_CACHE" 'selected plugin cache')" || exit 2
  daw_require_within_root "$PLUGIN_CACHE" "$RUNDIR" 'selected plugin cache' || exit 2
fi

case "${ANTHROPIC_API_KEY:-}" in
  *[![:space:]]*) ;;
  *) unset ANTHROPIC_API_KEY ;;
esac
case "${DAW_WEBSTACK_ALLOW_CREDENTIALS:-0}" in
  0)
    CREDENTIAL_MODE='credential-free default'
    unset ANTHROPIC_API_KEY DAW_ENV_FILE
    ;;
  1)
    CREDENTIAL_MODE='explicit credentialed mode'
    if [ -n "${DAW_ENV_FILE:-}" ]; then
      DAW_ENV_FILE="$(daw_canonical_readable_file "$DAW_ENV_FILE" 'explicit DAW_ENV_FILE')" || exit 2
    fi
    if [ -z "${ANTHROPIC_API_KEY:-}" ]; then
      if [ -z "${DAW_ENV_FILE:-}" ] || ! daw_env_file_has_anthropic_key "$DAW_ENV_FILE"; then
        say "REFUSING TO START: credentialed mode requested but no explicit key resolves"
        exit 2
      fi
    fi
    ;;
  *)
    say "REFUSING TO START: DAW_WEBSTACK_ALLOW_CREDENTIALS must be 0 or 1"
    exit 2
    ;;
esac
SIDECAR_API_KEY="${ANTHROPIC_API_KEY:-}"
SIDECAR_ENV_FILE="${DAW_ENV_FILE:-}"
unset ANTHROPIC_API_KEY DAW_ENV_FILE
unset DAW_WEBSTACK_ENGINE DAW_WEBSTACK_HOST DAW_WEBSTACK_ALLOW_CREDENTIALS
export -n SIDECAR_API_KEY SIDECAR_ENV_FILE 2>/dev/null || true

say "engine  $ENGINE ($ENGINE_LABEL)"
say "host    $HOST ($HOST_LABEL)"
say "project $PROJECTS (checkout fixture root)"
say "cache   $PLUGIN_CACHE (derived from selected engine artifact directory)"
say "presets $PATCHER_PRESETS (checkout source)"
say "ask     $CREDENTIAL_MODE; sidecar cwd cannot discover checkout/home .env files"
SOURCE_SHA="$(daw_git -C "$ROOT" rev-parse HEAD)" || exit 2
SOURCE_STATUS="$(daw_git -C "$ROOT" status --porcelain --untracked-files=all)" || exit 2
if [ -n "$SOURCE_STATUS" ]; then SOURCE_STATE=dirty; else SOURCE_STATE=clean; fi
say "source  $ROOT"
say "revision $SOURCE_SHA ($SOURCE_STATE)"
say "web     $UI_WEB (checkout source)"

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
[ ! -L "$PIDFILE" ] || { say "REFUSING TO START: pidfile is a symlink"; exit 1; }
TEMP_ROOT="$(daw_os_temp_root)" || exit 2
LOG_DIR="$(daw_make_temp_directory daw-webstack-log)" || exit 2
LOG_DIR="$(daw_canonical_directory "$LOG_DIR" 'run-owned webstack log directory')" || exit 2
[ ! -L "$LOG_DIR" ] && [ "$(dirname -- "$LOG_DIR")" = "$TEMP_ROOT" ] \
  || { say "REFUSING TO START: log directory is not a real immediate child of the trusted OS temp root"; exit 2; }
case "$(basename -- "$LOG_DIR")" in
  daw-webstack-log.*) ;;
  *) say "REFUSING TO START: log directory has an unexpected run-owned name"; exit 2 ;;
esac
daw_require_within_root "$LOG_DIR" "$TEMP_ROOT" 'run-owned webstack log directory' || exit 2
chmod 700 "$LOG_DIR" || { say "cannot restrict the run-owned log directory"; exit 2; }
ENGINE_LOG="$LOG_DIR/engine.log"
( umask 077; : > "$ENGINE_LOG" ) || { say "cannot create the run-owned engine log"; exit 2; }
[ -f "$ENGINE_LOG" ] && [ ! -L "$ENGINE_LOG" ] \
  || { say "REFUSING TO START: engine log is not a regular non-symlink file"; exit 2; }
ENGINE_LOG="$(daw_canonical_readable_file "$ENGINE_LOG" 'run-owned engine log')" || exit 2
daw_require_within_root "$ENGINE_LOG" "$LOG_DIR" 'run-owned engine log' || exit 2
say "logs    $LOG_DIR (validated unique temp directory)"
SIDECAR_RUN_CWD="$LOG_DIR/runtime/sidecar/cwd"
mkdir -p -- "$SIDECAR_RUN_CWD" || { say "cannot create run-owned sidecar cwd"; exit 2; }
SIDECAR_RUN_CWD="$(daw_canonical_directory "$SIDECAR_RUN_CWD" 'run-owned sidecar cwd')" || exit 2
daw_require_within_root "$SIDECAR_RUN_CWD" "$LOG_DIR" 'run-owned sidecar cwd' || exit 2
# The engine follows its last client out the door, because a user thinks the
# window is the application. A TEST RUN opens and closes a browser dozens of
# times and would take the engine with the first one, so harnesses set
# KEEP_ENGINE=1 — correct for a person, hostile to a harness.
KEEP=""
[ -n "${KEEP_ENGINE:-}" ] && KEEP="--keep-engine"

command -v lsof >/dev/null 2>&1 || { say "REFUSING TO START: lsof is required for port provenance"; exit 2; }
command -v curl >/dev/null 2>&1 || { say "REFUSING TO START: curl is required for page provenance"; exit 2; }
PAGE_REUSE=0
PAGE_LISTENERS="$(page_listener_pids)"
if [ -n "$PAGE_LISTENERS" ]; then
  [ "$(printf '%s\n' "$PAGE_LISTENERS" | wc -l | tr -d ' ')" = "1" ] \
    || { say "REFUSING TO START: multiple listeners occupy page port $PORT"; exit 1; }
  if page_server_matches_checkout "$PAGE_LISTENERS"; then
    PAGE_REUSE=1
    say "page    reusing checkout-local server pid $PAGE_LISTENERS"
  else
    say "REFUSING TO START: page port $PORT is owned by an unverified server"
    exit 1
  fi
fi

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
    if ps eww -p "$pid" 2>/dev/null | tr ' ' '\n' | grep -Fqx "DAW_UI_SHM_NAME=$SHM"; then
      printf '%s\n' "$pid"
    fi
  done
}

# A failed Cargo build, sidecar attach, or page readiness check must not leave
# the engine (and its audio device), sidecar, or page server behind. Only PIDs
# resolved from this invocation are stopped, plus engines whose environment
# exactly names this already-validated SHM segment. A reused checkout page is
# never ours and is therefore never included.
ROLLBACK_ARMED=0
STARTED_ENGINE_PID=''
STARTED_SIDECAR_PID=''
STARTED_PAGE_PID=''
STATE_FILE=''
stop_resolved_pid() {
  local pid="${1:-}"
  local attempt
  case "$pid" in ''|*[!0-9]*) return 0 ;; esac
  alive "$pid" || return 0
  kill "$pid" 2>/dev/null || return 0
  for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    alive "$pid" || return 0
    sleep 0.1
  done
  alive "$pid" && kill -9 "$pid" 2>/dev/null || true
}
webstack_exit() {
  local rc=$?
  local pid
  trap - EXIT INT TERM
  if [ "$ROLLBACK_ARMED" = "1" ]; then
    say "startup failed; rolling back only this run on $SHM"
    stop_resolved_pid "$STARTED_PAGE_PID"
    stop_resolved_pid "$STARTED_SIDECAR_PID"
    stop_resolved_pid "$STARTED_ENGINE_PID"
    for pid in $(our_engines); do
      [ "$pid" = "$STARTED_ENGINE_PID" ] || stop_resolved_pid "$pid"
    done
    if [ -n "$STATE_FILE" ]; then
      case "$STATE_FILE" in
        "$LOG_DIR"/*)
          if [ -f "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ]; then
            rm -f -- "$STATE_FILE" || say "WARNING: could not discard an unready state locator"
          fi
          ;;
      esac
    fi
    if [ -f "$PIDFILE" ] && [ ! -L "$PIDFILE" ]; then
      : > "$PIDFILE" || say "WARNING: could not clear the rolled-back numeric pidfile"
    fi
    say "rollback complete; diagnostic logs remain in $LOG_DIR"
  fi
  rmdir "$LOCK" 2>/dev/null || true
  exit "$rc"
}
trap webstack_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for pid in $(our_engines); do kill "$pid" 2>/dev/null || true; done
sleep 2
for pid in $(our_engines); do kill -9 "$pid" 2>/dev/null || true; done
sleep 1
still=$(our_engines | tr '\n' ' ')
if [ -n "${still// /}" ]; then
  say "REFUSING TO START: engine(s) on $SHM would not die: $still"
  ps -o pid,command -p "$(printf '%s\n' "$still" | tr ' ' ',' | sed 's/,*$//')" 2>/dev/null || true
  exit 1
fi
rm -f "$PIDFILE"
# Also clear anything holding OUR ports. A sidecar started by hand is not in the
# pidfile, and "Address already in use" three steps later is a much worse way to
# find out about it than here.
for port in $WS_STATE $WS_CMD; do
  for pid in $(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null || true); do
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
retire_prior_ready_states || exit 1

# The plugin cache is MACHINE state — which plugins are installed — but the engine
# reads it relative to its working directory, so a fresh build dir has none and the
# engine comes up able to resolve a plugin's path and unable to load it. That looks
# like a working stack: projects open, tracks appear, and every device query returns
# an empty parameter list from a host that never instantiated anything. Cost half an
# hour of blaming the engine. Say so rather than let it be silent.
if [ ! -s "$PLUGIN_CACHE" ]; then
  say "WARNING: no plugin_cache.json in $RUNDIR — hosts will load no plugins."
  say "         Copy one from another build dir, or scan: the cache is machine state,"
  say "         not tree state, so a copy is legitimate."
fi

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
[ ! -L "$PIDFILE" ] || { say "REFUSING TO START: pidfile became a symlink"; exit 1; }
: > "$PIDFILE"
chmod 600 "$PIDFILE" || { say "cannot restrict the numeric pidfile"; exit 2; }
ROLLBACK_ARMED=1

ENGINE_LAUNCH_PID_FILE="$LOG_DIR/engine.launch.pid"
( umask 077; : > "$ENGINE_LAUNCH_PID_FILE" ) \
  || { say "cannot create the run-owned engine launch pid file"; exit 2; }
(
  cd "$RUNDIR" || exit 1
  DAW_UI_SHM_NAME=$SHM DAW_PROJECT_DIR=$PROJECTS DAW_HOST_BINARY=$HOST \
    DAW_PLUGIN_CACHE=$PLUGIN_CACHE DAW_PATCHER_PRESET_DIR=$PATCHER_PRESETS \
    nohup "$ENGINE" "$@" > "$ENGINE_LOG" 2>&1 < /dev/null &
  printf '%s\n' "$!" > "$ENGINE_LAUNCH_PID_FILE"
)
STARTED_ENGINE_PID="$(sed -n '1p' "$ENGINE_LAUNCH_PID_FILE")"
case "$STARTED_ENGINE_PID" in
  ''|*[!0-9]*) say "engine launch did not return a numeric pid"; exit 1 ;;
esac
sleep 6
# The engine's own PID, cross-checked against the segment rather than trusted
# from process launch alone.
ENGINE_PID=$(our_engines | head -1)
alive "$ENGINE_PID" || { say "engine exited during startup:"; tail -5 "$ENGINE_LOG"; exit 1; }
[ "$ENGINE_PID" = "$STARTED_ENGINE_PID" ] \
  || { say "engine launch pid $STARTED_ENGINE_PID does not match segment pid $ENGINE_PID"; exit 1; }
printf "%s\n" "$ENGINE_PID" > "$PIDFILE"

# Build before launching. This script used to run whatever release binary was
# lying around, so an edited sidecar started silently as the previous one and
# answered a brand-new command with "unknown command" — twenty minutes of looking
# for a bug in code that was never running.
say "building the sidecar…"
if [ -L "$ROOT/ui/target" ]; then
  say "sidecar build target is a symlink; refusing to write outside the checkout"
  exit 2
fi
if [ -d "$ROOT/ui/target" ]; then
  CARGO_TARGET="$(daw_canonical_directory "$ROOT/ui/target" 'sidecar build target')" || exit 2
  daw_require_within_root "$CARGO_TARGET" "$ROOT" 'sidecar build target' || exit 2
  TARGET_SYMLINK="$(find "$CARGO_TARGET" -type l -print -quit 2>/dev/null)" \
    || { say "cannot inspect sidecar build target for symlinks"; exit 2; }
  [ -z "$TARGET_SYMLINK" ] || { say "sidecar build target contains a symlink"; exit 2; }
else
  CARGO_TARGET="$ROOT/ui/target"
fi
( cd "$ROOT/ui" && CARGO_TARGET_DIR="$CARGO_TARGET" cargo build --release -p daw-sidecar ) > "$LOG_DIR/sidecar-build.log" 2>&1 \
  || { say "sidecar build failed:"; tail -20 "$LOG_DIR/sidecar-build.log"; exit 1; }
SIDECAR="$(daw_canonical_executable "$CARGO_TARGET/release/daw-sidecar" 'built checkout sidecar')" || exit 2
daw_require_within_root "$SIDECAR" "$ROOT" 'built checkout sidecar' || exit 2
say "sidecar $SIDECAR (built from checkout source)"

# A run-owned cwd makes the sidecar's relative .env/../.env/../../.env search end
# inside this run. Both credential channels were stripped from the launcher
# environment above and are passed only to this sidecar child; paid mode was
# validated above and is opt-in.
SIDECAR_LAUNCH_PID_FILE="$LOG_DIR/sidecar.launch.pid"
( umask 077; : > "$SIDECAR_LAUNCH_PID_FILE" ) \
  || { say "cannot create the run-owned sidecar launch pid file"; exit 2; }
(
  cd "$SIDECAR_RUN_CWD"
  ANTHROPIC_API_KEY="$SIDECAR_API_KEY" DAW_ENV_FILE="$SIDECAR_ENV_FILE" \
    DAW_PROJECT_DIR=$PROJECTS \
    nohup "$SIDECAR" --shm "$SHM" --port "$WS_STATE" --cmd-port "$WS_CMD" $KEEP \
      --plugin-cache "$PLUGIN_CACHE" \
      > "$LOG_DIR/sidecar.log" 2>&1 < /dev/null &
  printf '%s\n' "$!" > "$SIDECAR_LAUNCH_PID_FILE"
)
STARTED_SIDECAR_PID="$(sed -n '1p' "$SIDECAR_LAUNCH_PID_FILE")"
case "$STARTED_SIDECAR_PID" in
  ''|*[!0-9]*) say "sidecar launch did not return a numeric pid"; exit 1 ;;
esac
SIDECAR_PID="$STARTED_SIDECAR_PID"
alive "$SIDECAR_PID" || { say "sidecar exited during startup:"; head -3 "$LOG_DIR/sidecar.log"; exit 1; }
printf '%s\n%s\n' "$ENGINE_PID" "$SIDECAR_PID" > "$PIDFILE"
wait_for_sidecar_listener "$WS_STATE" "$SIDECAR_PID" && wait_for_sidecar_listener "$WS_CMD" "$SIDECAR_PID" \
  || { SIDECAR_STATE_LISTENERS="$(sidecar_listener_pids "$WS_STATE")"; SIDECAR_CMD_LISTENERS="$(sidecar_listener_pids "$WS_CMD")"; \
    say "sidecar listener ownership/readiness failed for pid $SIDECAR_PID (state=$SIDECAR_STATE_LISTENERS cmd=$SIDECAR_CMD_LISTENERS)"; exit 1; }
[ "$(sidecar_listener_pids "$WS_STATE")" = "$SIDECAR_PID" ] && [ "$(sidecar_listener_pids "$WS_CMD")" = "$SIDECAR_PID" ] \
  || { say "sidecar listener ownership does not match sidecar pid $SIDECAR_PID (state=$SIDECAR_STATE_LISTENERS cmd=$SIDECAR_CMD_LISTENERS)"; exit 1; }
for _ in 1 2 3 4 5 6 7 8 9 10; do
  grep -q 'attached to' "$LOG_DIR/sidecar.log" && break
  sleep 0.25
done
grep -q 'attached to' "$LOG_DIR/sidecar.log" || { say "sidecar did not attach after listeners became ready:"; head -3 "$LOG_DIR/sidecar.log"; exit 1; }

if [ "$PAGE_REUSE" = "0" ]; then
  # Fully detached: </dev/null and nohup. A backgrounded child that still holds
  # the caller's stdin keeps an automated caller waiting for EOF forever — this
  # script hung a five-minute tool timeout exactly once, on the one run where the
  # page server was not already up and this branch actually executed.
  # test/serve.mjs, not `python3 -m http.server`. The python one sends
  # Last-Modified and NO Cache-Control, and a response with a validator but no
  # freshness directive lets the browser invent a lifetime of its own (RFC 9111
  # 4.2.2) — so a tab left open across a work session keeps serving the copy it
  # already has and never asks whether index.html changed.
  #
  # That cost a whole afternoon: space-to-play, wheel scrolling and the track
  # buttons were each reported dead while passing in a fresh browser, because the
  # report and the test were looking at different builds of the page. serve.mjs
  # sends no-store and exists for that one line.
  PAGE_LAUNCH_PID_FILE="$LOG_DIR/page.launch.pid"
  ( umask 077; : > "$PAGE_LAUNCH_PID_FILE" ) \
    || { say "cannot create the run-owned page launch pid file"; exit 2; }
  (
    cd "$UI_WEB" || exit 1
    nohup node test/serve.mjs "$PORT" > "$LOG_DIR/page.log" 2>&1 < /dev/null &
    printf '%s\n' "$!" > "$PAGE_LAUNCH_PID_FILE"
  )
  STARTED_PAGE_PID="$(sed -n '1p' "$PAGE_LAUNCH_PID_FILE")"
  case "$STARTED_PAGE_PID" in
    ''|*[!0-9]*) say "page launch did not return a numeric pid"; exit 1 ;;
  esac
  for _ in 1 2 3 4 5; do
    sleep 1
    PAGE_LISTENERS="$(page_listener_pids)"
    [ -n "$PAGE_LISTENERS" ] && break
  done
fi
PAGE_LISTENERS="$(page_listener_pids)"
[ -n "$PAGE_LISTENERS" ] && [ "$(printf '%s\n' "$PAGE_LISTENERS" | wc -l | tr -d ' ')" = "1" ] \
  || { say "page server did not bind port $PORT"; exit 1; }
if [ "$PAGE_REUSE" = "0" ] && [ "$PAGE_LISTENERS" != "$STARTED_PAGE_PID" ]; then
  say "page listener pid $PAGE_LISTENERS does not match launched pid $STARTED_PAGE_PID"
  exit 1
fi
page_server_matches_checkout "$PAGE_LISTENERS" \
  || { say "page server provenance/readiness check failed"; exit 1; }
say "page    source verified against $UI_WEB"

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

# Publish the ready locator only after every runtime component and provenance
# check above has passed. It lives inside the unique run-owned log directory;
# the segment PID file remains numeric-only. Consumers scan these unique state
# files, validate their canonical containment, and match ENGINE_PID to the first
# PID in the segment pidfile. A failed or partial start never publishes READY=1.
case "$ENGINE_PID" in
  ''|*[!0-9]*) say "REFUSING: resolved engine pid is not numeric"; exit 1 ;;
esac
STATE_FILE="$LOG_DIR/uni-web-stack$SEG.state"
[ ! -e "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ] \
  || { say "REFUSING: run-owned ready locator already exists"; exit 1; }
STATE_TMP="$(mktemp "$LOG_DIR/.uni-web-stack$SEG.state.XXXXXXXX")" \
  || { say "cannot create the run-owned ready locator temporary file"; exit 2; }
[ -f "$STATE_TMP" ] && [ ! -L "$STATE_TMP" ] \
  || { say "REFUSING: ready locator temporary path is not a regular non-symlink file"; exit 2; }
STATE_TMP="$(daw_canonical_readable_file "$STATE_TMP" 'run-owned ready locator temporary file')" || exit 2
daw_require_within_root "$STATE_TMP" "$LOG_DIR" 'run-owned ready locator temporary file' || exit 2
chmod 600 "$STATE_TMP" || { say "cannot restrict the run-owned ready locator"; exit 2; }
printf 'DAW_WEBSTACK_STATE=1\nREADY=1\nSEG=%s\nLOG_DIR=%s\nENGINE_LOG=%s\nENGINE_PID=%s\n' \
  "$SEG" "$LOG_DIR" "$ENGINE_LOG" "$ENGINE_PID" > "$STATE_TMP" \
  || { say "cannot write the run-owned ready locator"; exit 2; }
mv "$STATE_TMP" "$STATE_FILE" || { say "cannot atomically publish the run-owned ready locator"; exit 2; }
[ -f "$STATE_FILE" ] && [ ! -L "$STATE_FILE" ] \
  || { say "REFUSING: published ready locator is not a regular non-symlink file"; exit 2; }
STATE_FILE="$(daw_canonical_readable_file "$STATE_FILE" 'published run-owned ready locator')" || exit 2
daw_require_within_root "$STATE_FILE" "$LOG_DIR" 'published run-owned ready locator' || exit 2
ROLLBACK_ARMED=0
say "state   $STATE_FILE (validated ready locator)"
say "sidecar pid $SIDECAR_PID  ws $WS_STATE state / $WS_CMD commands"
say "page    http://127.0.0.1:$PORT/index.html"
head -2 "$LOG_DIR/sidecar.log" | sed 's/^/  /'
