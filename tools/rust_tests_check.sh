#!/usr/bin/env bash
# THE RUST TESTS COMPILE, AND THE ONES THAT GUARD THE WIRE RUN.
#
# ctest builds and runs C++ and shell. It has never built the Rust test binaries, so when
# `WaveformSlotView` gained a `sampler_addr` field and one test's initialiser was not updated,
# the daw-sidecar test binary STOPPED COMPILING — and 75 tests went absent from every run with
# nothing reporting it. `cargo build` stayed green throughout, because a test-only compile error
# is invisible to it.
#
# WHAT THAT HID. Two of the missing tests guard the sidecar's wire offsets. The `lpb` block had
# gone 8 -> 16 -> 64 bytes and every offset after it moved by 48; the encoder's trailing comments,
# its three checkpoint assertions and the test's literals all still named the old numbers, while
# ui-web/src/wire.js — the only side that has to spell offsets out — had the right ones. So the
# encoder's own `debug_assert_eq!(out.len(), 132, "the song meter starts at 132")` was false on
# every frame, and a debug build of the sidecar would have panicked immediately. Nobody could
# find that out, because finding it out required the test binary to build.
#
# THE COMPILE GATE IS THE POINT. --no-run over the whole workspace is what would have caught the
# original defect on the commit that introduced it; running the suites is the second-order check.
#
# WHAT IS NOT RUN HERE, said out loud rather than quietly skipped:
#   daw-agent's engine_e2e — it starts real engines and loads vendor plugins, which is what the
#   C++ checks in this suite already do under proper isolation, and running two engine fleets
#   from one ctest invocation is how the shared-segment collisions in this project started. It is
#   still COMPILED by the gate above. It currently has one genuine failure
#   (multi_bundle_selects_named_subplugin: a project naming Zebralette inside the Zebra2 bundle
#   gets "Identity"), tracked separately — this check does not paper over it, it declines to run
#   it here.
#
#   tools/rust_tests_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UI="$ROOT/ui"
# The sidecar's suite is the one this check exists for; a floor stops it passing on a suite that
# has quietly stopped registering tests, which is the same blindness in a different coat.
SIDECAR_FLOOR=70

command -v cargo >/dev/null 2>&1 || { echo "cargo not on PATH"; exit 2; }
[ -f "$UI/Cargo.toml" ] || { echo "no Rust workspace at $UI"; exit 2; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# 1. EVERY TEST BINARY IN THE WORKSPACE COMPILES.
if ! ( cd "$UI" && cargo test --workspace --no-run ) >"$TMP/build.log" 2>&1; then
  echo
  echo "  FAIL: a Rust test binary does not compile, so its tests are absent from every run"
  echo "        while \`cargo build\` stays green. The compiler's reason:"
  echo
  grep -E "^error(\[|:)" -A6 "$TMP/build.log" | head -30 | sed 's/^/          /'
  echo
  echo "        This is the exact shape that hid the sidecar's 48-byte wire drift: the test that"
  echo "        would have caught it had not built since a struct gained a field."
  exit 1
fi

# 2. THE SUITES THAT DO NOT NEED AN ENGINE ACTUALLY PASS.
if ! ( cd "$UI" && cargo test -p daw-sidecar -p daw-bridge -p daw-cli -p daw-agent ) >"$TMP/test.log" 2>&1; then
  echo
  echo "  FAIL: a Rust test failed."
  echo
  grep -E "^failures:|^---- |panicked at|assertion|^test result:" "$TMP/test.log" \
    | head -25 | sed 's/^/          /'
  exit 1
fi

# 3. BLINDNESS FLOOR. A suite that registers nothing passes step 2 by running nothing.
PASSED=$(grep -oE "test result: ok\. [0-9]+ passed" "$TMP/test.log" \
         | grep -oE "[0-9]+" | awk '{s+=$1} END {print s+0}')
if [ "${PASSED:-0}" -lt "$SIDECAR_FLOOR" ]; then
  fail "only ${PASSED:-0} Rust test(s) ran across daw-sidecar, daw-bridge and daw-cli, and there
        should be at least $SIDECAR_FLOOR. Either the suites moved or they stopped being
        registered — repoint this check rather than leaving it green over tests it can no
        longer see."
fi

echo "rust_tests_check: PASS — every workspace test binary compiles and $PASSED tests pass" \
     "(daw-agent's engine_e2e is compiled but not run here; see the header)"
