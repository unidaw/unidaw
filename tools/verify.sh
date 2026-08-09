#!/usr/bin/env bash
# Everything that can be checked, in one command.
#
#   tools/verify.sh           fast: rust + js units + goldens + allocation
#   tools/verify.sh --all     also frame work (opens a window) and a 4-min soak
#   tools/verify.sh --engine  ALSO every suite that drives a real engine
#   tools/verify.sh --plan [--engine|--all]  print the same commands without running them
#
# `--engine` is the one to run before believing anything. Each of those suites
# starts its OWN stack on its own ports and shared-memory segment, so they cannot
# disturb a session you have open — that isolation is why they can be in here at
# all. They take about twenty minutes together, which is why they are not the
# default.
#
# This exists because `npm test` does not run the Rust side, and a stale sidecar
# assertion survived several commits on exactly that gap — the clamp had moved
# from 512 to 2048 and only a manual `cargo test` noticed.
#
# Suites that need an engine are grouped under --engine rather than being left to
# be remembered one at a time. `reachable` is the one to read first if something is
# wrong: it asks whether the UI can SEE everything the document holds, which is the
# question the other suites cannot ask because each of them only tests material it
# already knows is there.
set -uo pipefail

if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'verify: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || exit 2
. "$SCRIPT_DIR/lib/repository_root.sh" || exit 2
ROOT="$(daw_repository_root)" || exit 2
unset BASH_ENV ENV NODE_OPTIONS NODE_PATH PYTHONHOME PYTHONPATH CARGO_TARGET_DIR
unset ANTHROPIC_API_KEY
while IFS= read -r daw_variable; do
  unset "$daw_variable"
done < <(compgen -v DAW_)
unset UNI_URL UNI_PROJECTS UNI_PROJECT UNI_HAS_AUDIO

PLAN_ONLY=0
MODE="${1:-}"
if [ "$MODE" = "--plan" ]; then
  PLAN_ONLY=1
  MODE="${2:-}"
fi
case "$MODE" in
  ''|--all|--engine) ;;
  *) printf '%s\n' 'usage: tools/verify.sh [--plan] [--all|--engine]' >&2; exit 2 ;;
esac

fail=0
step() {
  printf '\n== %s ==\n' "$1"; shift
  if [ "$PLAN_ONLY" = "1" ]; then
    printf '  PLAN cwd=%q' "$ROOT"
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  if (cd "$ROOT" && "$@"); then :; else fail=$((fail + 1)); printf '   ^ FAILED\n'; fi
}

TEMP_ROOT="$(daw_os_temp_root)" || exit 2
export TMPDIR="$TEMP_ROOT"
unset TMP TEMP
PATCHER_PRESETS="$(daw_canonical_directory "$ROOT/presets/patcher" 'checkout patcher presets')" || exit 2
daw_require_within_root "$PATCHER_PRESETS" "$ROOT" 'checkout patcher presets' || exit 2
export DAW_PATCHER_PRESET_DIR="$PATCHER_PRESETS"
PLUGIN_CACHE="$ROOT/build/plugin_cache.json"
if [ -L "$PLUGIN_CACHE" ]; then
  printf '%s\n' 'verify: ERROR: checkout plugin cache is a symlink' >&2
  exit 2
fi
if [ -e "$PLUGIN_CACHE" ]; then
  PLUGIN_CACHE="$(daw_canonical_readable_file "$PLUGIN_CACHE" 'checkout plugin cache')" || exit 2
  daw_require_within_root "$PLUGIN_CACHE" "$ROOT" 'checkout plugin cache' || exit 2
fi
export DAW_PLUGIN_CACHE="$PLUGIN_CACHE"

CARGO_TARGET="$ROOT/ui/target"
if [ -L "$CARGO_TARGET" ]; then
  printf '%s\n' 'verify: ERROR: checkout Cargo target is a symlink' >&2
  exit 2
fi
if [ -d "$CARGO_TARGET" ]; then
  CARGO_TARGET="$(daw_canonical_directory "$CARGO_TARGET" 'checkout Cargo target')" || exit 2
  daw_require_within_root "$CARGO_TARGET" "$ROOT" 'checkout Cargo target' || exit 2
  CARGO_SYMLINK="$(find "$CARGO_TARGET" -type l -print -quit 2>/dev/null)" || {
    printf '%s\n' 'verify: ERROR: cannot inspect checkout Cargo target for symlinks' >&2
    exit 2
  }
  if [ -n "$CARGO_SYMLINK" ]; then
    printf '%s\n' 'verify: ERROR: checkout Cargo target contains a symlink' >&2
    exit 2
  fi
fi
export CARGO_TARGET_DIR="$CARGO_TARGET"

NODE_MODULES="$ROOT/ui-web/node_modules"
SOURCE_SHA="$(daw_git -C "$ROOT" rev-parse HEAD)" || exit 2
SOURCE_STATUS="$(daw_git -C "$ROOT" status --porcelain --untracked-files=no)" || exit 2
if [ -n "$SOURCE_STATUS" ]; then SOURCE_STATE=dirty; else SOURCE_STATE=clean; fi
if [ -L "$ROOT/build" ]; then
  printf '%s\n' 'verify: ERROR: checkout build directory is a symlink' >&2
  exit 2
elif [ -d "$ROOT/build" ]; then
  BUILD_PROVENANCE="$(daw_canonical_directory "$ROOT/build" 'checkout build directory')" || exit 2
  daw_require_within_root "$BUILD_PROVENANCE" "$ROOT" 'checkout build directory' || exit 2
  daw_validate_cmake_build_source "$BUILD_PROVENANCE" "$ROOT" 'checkout build directory' || exit 2
else
  BUILD_PROVENANCE="$ROOT/build (missing)"
fi
if [ "$PLAN_ONLY" = "0" ] && [ "$MODE" = "--engine" ]; then
  for artifact_spec in \
    "$ROOT/build/daw_engine|daw_engine" \
    "$ROOT/build/juce_host_process|juce_host_process" \
    "$ROOT/ui/target/release/daw-sidecar|daw-sidecar" \
    "$ROOT/ui/target/release/daw-cli|daw-cli"; do
    artifact_path="${artifact_spec%%|*}"
    artifact_label="${artifact_spec#*|}"
    [ ! -L "$artifact_path" ] || {
      printf 'verify: ERROR: %s is a symlink\n' "$artifact_label" >&2
      exit 2
    }
    artifact_path="$(daw_canonical_executable "$artifact_path" "$artifact_label")" || exit 2
    daw_require_within_root "$artifact_path" "$ROOT" "$artifact_label" || exit 2
    printf 'verification artifact: %s (%s)\n' "$artifact_path" "$artifact_label"
  done
fi
printf 'verification source: %s\n' "$ROOT"
printf 'verification revision: %s (%s)\n' "$SOURCE_SHA" "$SOURCE_STATE"
printf 'verification build: %s\n' "$BUILD_PROVENANCE"
printf 'verification dependencies: %s\n' "$NODE_MODULES"
printf 'verification Cargo target: %s\n' "$CARGO_TARGET"
printf 'verification plugin cache: %s\n' "$PLUGIN_CACHE"
printf 'verification patcher presets: %s\n' "$PATCHER_PRESETS"
printf 'verification temp root: %s\n' "$TEMP_ROOT"

step "repository integrity" bash "$ROOT/tools/repository_integrity_check.sh"
step "webstack free-port startup control" bash "$ROOT/tools/webstack.sh" --self-test-free-port

if [ "$PLAN_ONLY" = "0" ]; then
  [ -d "$NODE_MODULES" ] && [ ! -L "$NODE_MODULES" ] \
    || { printf '%s\n' 'verify: ERROR: checkout-local ui-web dependencies are not installed as a real directory' >&2; exit 2; }
  NODE_MODULES="$(daw_canonical_directory "$NODE_MODULES" 'ui-web dependency directory')" || exit 2
  daw_require_within_root "$NODE_MODULES" "$ROOT" 'ui-web dependency directory' || exit 2
  PLAYWRIGHT_ENTRY="$(cd "$ROOT/ui-web" && node -e "process.stdout.write(require('fs').realpathSync(require.resolve('playwright')))" 2>/dev/null)" \
    || { printf '%s\n' "verify: ERROR: checkout-local 'playwright' module is unavailable" >&2; exit 2; }
  case "$PLAYWRIGHT_ENTRY" in
    "$NODE_MODULES"/*) ;;
    *) printf '%s\n' "verify: ERROR: 'playwright' resolved outside checkout-local dependencies" >&2; exit 2 ;;
  esac
  PLAYWRIGHT_CORE_ENTRY="$(cd "$ROOT/ui-web" && node -e "const fs=require('fs'),{createRequire}=require('module');const entry=fs.realpathSync(require.resolve('playwright'));process.stdout.write(fs.realpathSync(createRequire(entry).resolve('playwright-core')))" 2>/dev/null)" \
    || { printf '%s\n' "verify: ERROR: checkout-local 'playwright-core' module is unavailable" >&2; exit 2; }
  case "$PLAYWRIGHT_CORE_ENTRY" in
    "$NODE_MODULES"/*) ;;
    *) printf '%s\n' "verify: ERROR: 'playwright-core' resolved outside checkout-local dependencies" >&2; exit 2 ;;
  esac
fi

step "rust: daw-bridge + daw-sidecar" \
  cargo test --manifest-path "$ROOT/ui/Cargo.toml" -p daw-bridge -p daw-sidecar --release -q
step "js: pure functions" \
  node --test "$ROOT/ui-web/test/unit.mjs"
step "js: goldens" \
  node "$ROOT/ui-web/test/shot.mjs"
step "js: allocation" \
  node "$ROOT/ui-web/test/alloc.mjs"

if [ "$MODE" = "--all" ]; then
  step "js: frame work" node "$ROOT/ui-web/test/frametime.mjs"
  step "js: heap soak" node "$ROOT/ui-web/test/soak.mjs" 4
fi

if [ "$MODE" = "--engine" ]; then
  # REACHABILITY FIRST, because it is the broadest: it builds a document by editing
  # and asks whether the UI can see all of it. Two data-loss bugs that no fixture
  # could produce are caught by this one file.
  step "engine: reachability"  node "$ROOT/ui-web/test/reachable.mjs"
  step "engine: e2e"           node "$ROOT/ui-web/test/e2e.mjs"
  step "engine: journey"       node "$ROOT/ui-web/test/journey.mjs"
  step "engine: markers "      node "$ROOT/ui-web/test/markers.mjs"
  step "engine: shared"        node "$ROOT/ui-web/test/shared.mjs"
  step "engine: modulation"    node "$ROOT/ui-web/test/mods.mjs"
  step "engine: chrome"        node "$ROOT/ui-web/test/chrome.mjs"
  step "engine: layout"        node "$ROOT/ui-web/test/layout.mjs"
  step "engine: automation"    node "$ROOT/ui-web/test/automation.mjs"
  step "engine: placements"    node "$ROOT/ui-web/test/placement.mjs"
  # The AUDIO suites. Each captures the master output and asserts on the sound, so
  # they are the only checks here that can tell "the value changed" from "the change
  # was audible" — and they take a minute each because they have to play something.
  step "audio: quantize"       node "$ROOT/ui-web/test/quantize.mjs"
  step "audio: patcher knobs"  node "$ROOT/ui-web/test/patchcfg.mjs"
  step "audio: bypass"         node "$ROOT/ui-web/test/bypass.mjs"
  step "audio: panic"          node "$ROOT/ui-web/test/panic.mjs"
fi

if [ "$PLAN_ONLY" = "1" ]; then
  printf '\n%s\n' 'PLAN COMPLETE (no suites run)'
else
  printf '\n%s\n' "$([ $fail -eq 0 ] && echo 'ALL GREEN' || echo "$fail SUITE(S) FAILED")"
fi
exit $((fail > 0))
