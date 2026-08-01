#!/usr/bin/env bash
# THE TREE CONFIGURES FROM A COLD CACHE.
#
# Every other check in this repo runs against a build directory that is already configured, so
# they all answer "does the code work" and none of them answers "can someone else build it at
# all". That gap is not theoretical: f913038 registered two ctest entries named `clip_grid` —
# CMake refuses a duplicate test name at CONFIGURE time — and main stopped configuring. My tree
# built and tested green through it, because CMake only re-runs configure when CMakeLists.txt is
# newer than the cache and mine had been generated before the second add_test existed. The
# frontend agent hit it within a day on a cold checkout. A defect that is invisible to everyone
# who already has the project working, and fatal to everyone who does not, is the worst shape a
# build defect can have.
#
# WHY A REAL CONFIGURE AND NOT A GREP FOR DUPLICATE NAMES. A grep would have caught this one
# instance and nothing else. Configure-time failures are a family: a duplicate test name, an
# add_test pointing at a script that was renamed, a target listed in one place and deleted in
# another, a find_package made mandatory. CMake already detects all of them exactly; the only
# thing missing was anyone running it on a clean directory. So run it.
#
# The build directory is a fresh temp dir every time and is removed on exit — configuring into
# the real build/ would defeat the point (that cache is the thing being distrusted) and would
# also churn it for whatever runs next.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/cold_configure.XXXXXX")"
cleanup() { rm -rf "${TMP}"; }
trap cleanup EXIT

LOG="${TMP}/configure.log"
set +e
cmake -B "${TMP}/build" -S "${ROOT}" >"${LOG}" 2>&1
STATUS=$?
set -e

if [[ ${STATUS} -ne 0 ]]; then
  echo "cold_configure_check: FAIL — cmake could not configure this tree from an empty cache."
  echo "  A warm build/ hides this completely: configure only re-runs when CMakeLists.txt is"
  echo "  newer than the cache, so an existing tree keeps building green while a fresh clone"
  echo "  cannot build at all. The errors, verbatim:"
  echo
  # Only the error blocks, so the failure is readable without the 60 lines of compiler probing
  # that precede it. The whole log is printed after, because a configure error whose cause is
  # three lines above the "CMake Error" line is common enough that truncating is a trap.
  grep -n -A4 "CMake Error" "${LOG}" | sed 's/^/    /' || true
  echo
  echo "  --- full configure log ---"
  sed 's/^/    /' "${LOG}"
  exit 1
fi

# Configure succeeding is the assertion, but a configure that produced no test list would pass it
# while meaning the ctest suite had vanished — so confirm the generated tree can actually be
# enumerated, which is the thing every other check depends on.
TESTS="$(cd "${TMP}/build" && ctest -N 2>/dev/null | grep -c "^  Test" || true)"
if [[ "${TESTS}" -lt 50 ]]; then
  echo "cold_configure_check: FAIL — configured, but the generated tree lists only ${TESTS} tests."
  echo "  Expected the full suite (100+). A configure that succeeds while registering almost"
  echo "  nothing is how a whole block of add_test calls goes missing without a build error."
  exit 1
fi

echo "cold_configure_check: PASS — a fresh cache configures and lists ${TESTS} tests."
