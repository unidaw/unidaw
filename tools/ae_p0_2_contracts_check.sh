#!/usr/bin/env bash
# THE AE-P0.2 GENERATED CONTRACTS ARE VALIDATED BY TESTS THAT NOTHING WAS RUNNING.
#
# AE-P0.2 generates a schema bundle and emits it into four languages
# (`tools/architecture/ae_p0_2/generated/`). Six test files validate that output, all six pass, and
# until this check existed **not one of them was executed by anything**: `ae_p0_2` appeared nowhere
# in CMakeLists.txt, and `contracts_rust_test.rs` had no Cargo.toml above it, so `cargo test` could
# not have found it either. The ledger recorded the ticket COMPLETE, with its test suite never run.
#
# `check_registry_check.sh` could not see this: its glob is `tools/*_check.sh`, and these tests are
# a different population entirely. That is why this file is a `*_check.sh` in `tools/` rather than a
# bare add_test — so the registry ratchet covers the thing that covers them.
#
# WHAT THIS PINS, AND WHY EACH RULE EXISTS
#
# 1. Every `*.test.mjs` under tests/ is RUN. Discovered by glob, not listed, because a hand-written
#    list is exactly how a population decays by addition — the next test is correct on the day it is
#    written and unlisted forever after.
#
# 2. The COUNT is pinned, not a floor. A floor survives the mutation it exists to catch. Growth is
#    refused until a human states the new number, and that refusal is the prompt to check the new
#    member by hand.
#
# 3. The two COMPILED tests are named as exercised by `cross-language.test.mjs`. This is the rule
#    with the least obvious motivation and the most consequence: `contracts_cpp_test.cpp` and
#    `contracts_rust_test.rs` are NOT standalone binaries and have no registration of their own.
#    Their only path to execution is that cross-language.test.mjs compiles them with clang++/rustc
#    and feeds them 23 golden-vector arguments.
#
#    Two mutations were run, and they do NOT behave the same — which is the whole reason this rule
#    is worth its lines, and why the first phrasing of this comment was wrong:
#
#      swap `clang++` for a no-op   suite goes 2 pass / 1 FAIL — the suite catches this itself,
#                                   because the executable it then tries to run does not exist
#      DELETE the whole C++ test    suite goes 2 pass / 0 fail — FULLY GREEN, C++ coverage gone
#
#    So the danger is not a broken invocation, which is loud. It is a tidy deletion, which is
#    silent: remove the test that looks redundant and nothing anywhere reports a loss. That second
#    mutation is what this rule exists for, and it is the one a person doing cleanup would make.
#
#    Separately verified that the compiled test is genuinely exercised rather than merely compiled:
#    altering a constant in generated/contracts.hpp takes cross-language.test.mjs from 3 pass / 0
#    fail to 2 pass / 1 fail, reporting the C++ assertion by name.
#
#   tools/ae_p0_2_contracts_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SUITE="tools/architecture/ae_p0_2/tests"
EXPECTED_TEST_FILES=4          # *.test.mjs; the two compiled tests are driven BY cross-language
CROSS="$SUITE/cross-language.test.mjs"
fail=0

if [ ! -d "$SUITE" ]; then
  echo "  FAIL: $SUITE does not exist — this check has gone blind and proves nothing"
  echo "ae_p0_2_contracts_check: FAILED"
  exit 1
fi

# ---- rule 1 + 2: the population, discovered and pinned ----------------------------------------
# No `mapfile`: the bash on this machine is 3.2 and does not have it. Caught by running the file
# rather than by reading it, which is the only way shell logic gets verified here.
TESTS=()
while IFS= read -r line; do TESTS+=("$line"); done < <(find "$SUITE" -name '*.test.mjs' | sort)
if [ "${#TESTS[@]}" -ne "$EXPECTED_TEST_FILES" ]; then
  echo "  FAIL: ${#TESTS[@]} *.test.mjs under $SUITE, expected exactly $EXPECTED_TEST_FILES"
  printf '        %s\n' "${TESTS[@]}"
  echo "        If a test was ADDED, confirm it runs here and raise the number deliberately."
  echo "        If one was removed, say why the contract it covered no longer needs covering."
  fail=1
fi

# ---- rule 3: the compiled tests have no other way to be executed -------------------------------
# Checked by STRUCTURE — the compiler invocation and the test's own path must both appear — rather
# than by a filename mention, which a comment naming the file would satisfy.
for pair in "clang++:contracts_cpp_test.cpp" "rustc:contracts_rust_test.rs"; do
  tool="${pair%%:*}"; src="${pair##*:}"
  if [ ! -f "$SUITE/$src" ]; then
    echo "  FAIL: $SUITE/$src is missing; the ${tool%%+*} half of the contract is unverified"
    fail=1
    continue
  fi
  if ! grep -q "execFileSync('$tool'" "$CROSS"; then
    echo "  FAIL: cross-language.test.mjs no longer invokes $tool, so $src is compiled by nothing."
    echo "        That file is not a standalone binary and has no registration of its own: with"
    echo "        this call gone the suite stays GREEN and the contract stops being checked."
    fail=1
  fi
done

# ---- run them ----------------------------------------------------------------------------------
# One at a time and by PATH: `node --test <dir>` is not supported by every node here and fails with
# MODULE_NOT_FOUND, which reads exactly like a broken test suite. It cost a wrong diagnosis once.
for t in "${TESTS[@]}"; do
  if out=$(node --test "$t" 2>&1); then
    printf '  ok   %s\n' "${t#$SUITE/}"
  else
    printf '  FAIL %s\n' "${t#$SUITE/}"
    printf '%s\n' "$out" | grep -E 'not ok|Assertion|Error:|AssertionError' | head -6 | sed 's/^/        /'
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "        These validate the generated schema bundle and its C++/Rust/TS emissions."
  echo "        They passed for weeks while nothing ran them; a failure here is real."
  echo "ae_p0_2_contracts_check: FAILED"
  exit 1
fi

echo "ae_p0_2_contracts_check: PASS — ${#TESTS[@]} suites run, both compiled tests still driven"
