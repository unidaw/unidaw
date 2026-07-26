#!/usr/bin/env bash
# Everything that can be checked without a running engine, in one command.
#
#   tools/verify.sh          fast: rust + js units + goldens + allocation
#   tools/verify.sh --all    also frame work (opens a window) and a 4-min soak
#
# This exists because `npm test` does not run the Rust side, and a stale sidecar
# assertion survived several commits on exactly that gap — the clamp had moved
# from 512 to 2048 and only a manual `cargo test` noticed.
#
# The engine-dependent suite is separate on purpose:
#   tools/webstack.sh && (cd ui-web && npm run e2e)
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

printf '\n%s\n' "$([ $fail -eq 0 ] && echo 'ALL GREEN' || echo "$fail SUITE(S) FAILED")"
exit $((fail > 0))
