#!/usr/bin/env bash
# Everything that can be checked, in one command.
#
#   tools/verify.sh           fast: rust + js units + goldens + allocation
#   tools/verify.sh --all     also frame work (opens a window) and a 4-min soak
#   tools/verify.sh --engine  ALSO every suite that drives a real engine
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

WEB=/Users/jak/src/daw-web
fail=0
step() {
  printf '\n== %s ==\n' "$1"; shift
  if "$@"; then :; else fail=$((fail + 1)); printf '   ^ FAILED\n'; fi
}

step "rust: daw-bridge + daw-sidecar" \
  cargo test --manifest-path "$WEB/ui/Cargo.toml" -p daw-bridge -p daw-sidecar --release -q
step "js: pure functions" \
  node --test "$WEB/ui-web/test/unit.mjs"
step "js: goldens" \
  node "$WEB/ui-web/test/shot.mjs"
step "js: allocation" \
  node "$WEB/ui-web/test/alloc.mjs"

if [ "${1:-}" = "--all" ]; then
  step "js: frame work" node "$WEB/ui-web/test/frametime.mjs"
  step "js: heap soak" node "$WEB/ui-web/test/soak.mjs" 4
fi

if [ "${1:-}" = "--engine" ]; then
  # REACHABILITY FIRST, because it is the broadest: it builds a document by editing
  # and asks whether the UI can see all of it. Two data-loss bugs that no fixture
  # could produce are caught by this one file.
  step "engine: reachability"  node "$WEB/ui-web/test/reachable.mjs"
  step "engine: e2e"           node "$WEB/ui-web/test/e2e.mjs"
  step "engine: journey"       node "$WEB/ui-web/test/journey.mjs"
  step "engine: markers "      node "$WEB/ui-web/test/markers.mjs"
  step "engine: modulation"    node "$WEB/ui-web/test/mods.mjs"
  step "engine: chrome"        node "$WEB/ui-web/test/chrome.mjs"
  step "engine: layout"        node "$WEB/ui-web/test/layout.mjs"
  step "engine: automation"    node "$WEB/ui-web/test/automation.mjs"
  step "engine: placements"    node "$WEB/ui-web/test/placement.mjs"
  # The AUDIO suites. Each captures the master output and asserts on the sound, so
  # they are the only checks here that can tell "the value changed" from "the change
  # was audible" — and they take a minute each because they have to play something.
  step "audio: quantize"       node "$WEB/ui-web/test/quantize.mjs"
  step "audio: patcher knobs"  node "$WEB/ui-web/test/patchcfg.mjs"
  step "audio: bypass"         node "$WEB/ui-web/test/bypass.mjs"
  step "audio: panic"          node "$WEB/ui-web/test/panic.mjs"
fi

printf '\n%s\n' "$([ $fail -eq 0 ] && echo 'ALL GREEN' || echo "$fail SUITE(S) FAILED")"
exit $((fail > 0))
