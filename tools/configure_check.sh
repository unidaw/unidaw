#!/usr/bin/env bash
# CMAKE CAN STILL PROCESS THIS TREE.
#
# Every other test in this repo runs against a build directory that is already configured, so the
# whole suite answers "does the code work" and not one of them answers "can this still be built".
# That gap is not theoretical: f913038 registered two ctest entries named `clip_grid` — CMake
# refuses a duplicate test name at CONFIGURE time — and main stopped configuring entirely. My tree
# built and tested green through it for a day, because CMake only re-runs configure when
# CMakeLists.txt is newer than the cache and mine had been generated before the second add_test
# existed. The frontend agent hit it on a cold checkout. A defect invisible to everyone who
# already has the project working, and fatal to everyone who does not, is the worst shape a build
# defect can have.
#
# WHY A REAL CONFIGURE AND NOT A GREP FOR DUPLICATE NAMES. A grep would have caught that one
# instance and nothing else. Configure-time failures are a family, and CMake already detects every
# member of it exactly; the only missing ingredient was anyone running it.
#
# The two members verified against this check, by breaking each and watching it fail: a DUPLICATE
# TEST NAME, and a SET_TESTS_PROPERTIES naming a test that no longer exists (which is what a
# half-finished rename leaves behind). A third was tried and does NOT fail — an `add_test` whose
# COMMAND names a script that is not there configures cleanly, because CMake never resolves the
# command until the test runs. That is not a hole worth closing here: it surfaces as that one test
# failing loudly, which is visible, rather than as the whole tree refusing to configure, which is
# not. Written down because the first version of this comment claimed it was covered.
#
# WHY A PERSISTENT PROBE DIRECTORY AND NOT A FRESH ONE EVERY TIME, which is what this check did
# first and is worth writing down. Configuring from an empty directory re-adds JUCE and re-probes
# the compiler: measured at 76 seconds idle and 953 seconds inside a full ctest run, where it blew
# a ten-minute timeout while PASSING. Re-configuring a directory that already exists takes 1.4
# seconds and still processes every add_test, so it still fails on all of the above — verified by
# putting the duplicate name back, which produced both the duplicate add_test error and a
# set_tests_properties error naming the test that had vanished.
#
# WHAT THE CHEAP FORM DOES NOT COVER, said plainly rather than left to be discovered: a dependency
# that is newly missing on a clean machine, or a cached variable masking a change to how something
# is found. Those need a genuinely empty cache — which is what a fresh clone or a CI job is, and
# what this check pays for the first time it runs, since the probe directory does not exist yet.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# A SIBLING of build/, not a child: nesting a build tree inside CMAKE_BINARY_DIR confuses enough
# tooling to be worth avoiding, and a sibling is obvious to find and safe to delete. Listed in
# .gitignore beside build/ and build-tsan/, for the reason the comment there already gives.
PROBE="${ROOT}/build-configure-probe"
LOG="$(mktemp "${TMPDIR:-/tmp}/configure_check.XXXXXX")"
trap 'rm -f "${LOG}"' EXIT

FIRST=""
if [[ ! -f "${PROBE}/CMakeCache.txt" ]]; then
  FIRST=" (first run: configuring from empty, so this one is slow)"
fi

set +e
cmake -B "${PROBE}" -S "${ROOT}" >"${LOG}" 2>&1
STATUS=$?
set -e

if [[ ${STATUS} -ne 0 ]]; then
  echo "configure_check: FAIL — cmake could not process this tree."
  echo "  A warm build/ hides this completely: configure only re-runs when CMakeLists.txt is"
  echo "  newer than the cache, so an existing tree keeps building green while a fresh clone"
  echo "  cannot build at all. The errors, verbatim:"
  echo
  # The error blocks first, so the failure is readable without scrolling; the whole log after,
  # because a configure error whose real cause is three lines above the "CMake Error" line is
  # common enough that truncating is a trap.
  grep -n -A4 "CMake Error" "${LOG}" | sed 's/^/    /' || true
  echo
  echo "  --- full configure log ---"
  sed 's/^/    /' "${LOG}"
  echo
  echo "  The probe directory is ${PROBE}; delete it to retry from an empty cache."
  exit 1
fi

# Configure succeeding is the assertion, but a configure that produced no test list would pass it
# while meaning the ctest suite had vanished — so confirm the generated tree can be enumerated,
# which is the thing every other check depends on.
TESTS="$(cd "${PROBE}" && ctest -N 2>/dev/null | grep -c "^  Test" || true)"
if [[ "${TESTS}" -lt 50 ]]; then
  echo "configure_check: FAIL — configured, but the generated tree lists only ${TESTS} tests."
  echo "  Expected the full suite (100+). A configure that succeeds while registering almost"
  echo "  nothing is how a whole block of add_test calls goes missing without a build error."
  exit 1
fi

echo "configure_check: PASS — cmake processes this tree and lists ${TESTS} tests${FIRST}."
