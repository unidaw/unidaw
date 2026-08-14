#!/usr/bin/env bash
# COMMAND TRAFFIC DURING PLAYBACK, UNDER THREADSANITIZER.
#
# AE-P1.4's gate is "concurrency hammer and TSan-supported tests show no mixed atomic/plain access
# or lifetime violation". `tools/tsan_render.sh` is the other half and does not reach this one: it
# drives `daw_engine --render`, an OFFLINE render with no UI commands at all, so it exercises the
# producer, consumer, render pool and master render thread — and never the command thread.
#
# EVERY WRITE P1.4 FIXED IS ON THE COMMAND THREAD. The five plain writes to `trackSnapshot` were in
# patcher edits, aux reconciliation, track tombstone/reuse and the command-side reuse path; the
# watchdog use-after-free was in the restart worker. A clean render says nothing about any of them,
# which is why the render passing was reported as NO-REGRESSION rather than as evidence.
#
# So this boots a real engine, starts playback, and drives the command paths that write the pointers
# the producer reads — while the producer is reading them. That is the interleaving the fix is about
# and the only one TSan can judge it on.
#
# WHAT A CLEAN RUN DOES AND DOES NOT ESTABLISH, said here because a green sanitizer is the easiest
# result in this repository to over-read:
#   * TSan reports races it OBSERVES. It cannot report an interleaving that did not occur, so a
#     clean run is evidence proportional to how hard the commands and the producer actually
#     collided — which is why this hammers rather than sends one command.
#   * It is a strictly better instrument than a passing suite, which cannot see a race at all: the
#     suite compares outputs, and a data race that happens to produce the expected bytes on the run
#     you measured passes it every time.
#
#   tools/tsan_command_hammer.sh [seconds]
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-tsan"
CLI="$ROOT/ui/target/debug/daw-cli"
SECONDS_TO_RUN="${1:-20}"
SHM="daw_tsan_hammer_$$"

[ -x "$CLI" ] || { echo "build daw-cli first (cargo build --manifest-path ui/Cargo.toml -p daw-cli)"; exit 2; }

echo "== configuring $BUILD"
cmake -S "$ROOT" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -Wl,-no_compact_unwind" \
  -DDAW_BUILD_PATCHER_RUST=ON >/dev/null || { echo "configure failed"; exit 2; }

echo "== building instrumented engine"
cmake --build "$BUILD" --target daw_engine juce_host_process -j8 >/dev/null || {
  echo "build failed — rerun without >/dev/null to see why"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
# ALWAYS REAP. A TSan engine is slow to exit and this script kills it; leaving one behind holds a
# shared segment and an audio device, and this project has lost days to orphaned engines.
cleanup() {
  [ -n "$ENG" ] && kill "$ENG" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

echo "== booting engine under TSan (shm $SHM)"
( cd "$BUILD" && DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$ROOT/presets/projects" \
    TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" \
    ./daw_engine --run-seconds "$((SECONDS_TO_RUN + 10))" >"$TMP/eng.log" 2>&1 ) &
ENG=$!

# COUNT WHAT LANDED, NOT WHAT WAS ATTEMPTED. The first version of this swallowed the exit code with
# `|| true` and counted loop ITERATIONS, so 108 silently-refused commands and 108 applied ones
# printed the same "18 rounds ... PASS". A stale daw-cli against a live engine is refused SILENTLY in
# this project, which is exactly the case that would have been reported as evidence.
LANDED=0
REFUSED=0
cli() {
  if env DAW_UI_SHM_NAME="$SHM" "$CLI" "$@" >>"$TMP/cli.log" 2>&1; then
    LANDED=$((LANDED + 1))
  else
    REFUSED=$((REFUSED + 1))
  fi
}

# Wait for the command thread rather than sleeping a guessed interval: a TSan build boots several
# times slower than a normal one, so any fixed wait is either wrong now or wrong on a slower machine.
for _ in $(seq 1 120); do
  grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null && break
  kill -0 "$ENG" 2>/dev/null || { echo "engine died during boot"; tail -20 "$TMP/eng.log"; exit 1; }
  sleep 1
done
grep -q "UI: command thread started" "$TMP/eng.log" 2>/dev/null || {
  echo "engine never came up"; tail -20 "$TMP/eng.log"; exit 1; }

cli do load maximal --force
cli do play --force
if [ "$LANDED" -lt 2 ]; then
  echo "  FAIL: load/play did not land ($LANDED ok, $REFUSED refused) — nothing to hammer against."
  tail -20 "$TMP/cli.log" 2>/dev/null
  echo "tsan_command_hammer: FAILED"; exit 1
fi
# Setup is not traffic. Zero the counters so the numbers below describe the hammer only.
LANDED=0
REFUSED=0

echo "== hammering the command paths that write what the producer reads (${SECONDS_TO_RUN}s)"
END=$(( $(date +%s) + SECONDS_TO_RUN ))
rounds=0
while [ "$(date +%s)" -lt "$END" ]; do
  # Each of these writes trackSnapshot on the command thread while the producer loads it.
  cli do add-device --track 0 --kind sampler        # chain edit -> snapshot rebuild
  cli do set-bypass --track 0 --device 0 --on 1     # chain edit
  cli do set-bypass --track 0 --device 0 --on 0
  cli do add-track                                  # tombstone reuse on a recycled slot
  cli do remove-track --track 3
  cli do undo                                       # whole-document swap
  rounds=$((rounds + 1))
done
echo "   $rounds round(s): $LANDED command(s) landed, $REFUSED refused"
# Frozen BEFORE the shutdown command, so the number the verdict quotes is the number reported above.
# The two disagreed by one on the first run — small, but a figure in a PASS line that contradicts the
# figure above it is the kind of thing that makes the whole artifact worth less than it is.
HAMMER_LANDED="$LANDED"

cli do stop
sleep 2
kill "$ENG" 2>/dev/null
wait "$ENG" 2>/dev/null
ENG=""

# NOT `grep -c ... || echo 0`. grep -c PRINTS "0" and EXITS 1 when there is no match — the count is
# its answer and the exit status is the same answer again — so the `||` fires on the clean case and
# appends a SECOND "0". RACES becomes "0\n0", `[ -gt ]` errors on it, and the error path is skipped
# by accident: the clean run and the broken-count run are then indistinguishable.
RACES="$(grep -c 'WARNING: ThreadSanitizer' "$TMP/eng.log" 2>/dev/null)"
[ -n "$RACES" ] || RACES=0
echo "== ThreadSanitizer warnings: $RACES"
if [ "$RACES" -gt 0 ]; then
  echo
  grep -A22 'WARNING: ThreadSanitizer' "$TMP/eng.log" | head -60
  cp "$TMP/eng.log" "${DAW_CHECK_EVIDENCE:-/tmp}/tsan_command_hammer.log" 2>/dev/null && \
    echo "  full log kept at ${DAW_CHECK_EVIDENCE:-/tmp}/tsan_command_hammer.log"
  echo "tsan_command_hammer: FAILED"
  exit 1
fi

# A CLEAN RUN THAT NEVER RAN IS THE FAILURE MODE THIS GUARDS. If the commands did not land, the
# producer never collided with anything and TSan correctly reports nothing — indistinguishable in
# the exit code from a genuinely clean interleaving.
#
# ROUNDS ALONE CANNOT SEE THAT, which is how the first green run of this script was obtained: the
# loop spun 18 times whether or not a single command reached the engine. The predicate has to be
# what LANDED, measured from the CLI's exit status, because that is the thing the property depends
# on. A guard one level above the fact it protects is a proxy, and this one proxied.
if [ "$rounds" -lt 2 ]; then
  echo "  FAIL: only $rounds round(s) of command traffic — the hammer did not hammer, so a clean"
  echo "        result says nothing about the paths it exists to exercise."
  echo "tsan_command_hammer: FAILED"
  exit 1
fi
if [ "$LANDED" -lt 12 ]; then
  echo "  FAIL: only $LANDED command(s) landed ($REFUSED refused) across $rounds rounds. The"
  echo "        producer had nothing to collide with, so TSan reporting nothing is expected and"
  echo "        says nothing about the command thread. Last CLI output:"
  tail -20 "$TMP/cli.log" 2>/dev/null
  echo "tsan_command_hammer: FAILED"
  exit 1
fi
# A refusal is not a race, but a run that is mostly refusals exercised mostly the refusal path.
[ "$REFUSED" -gt 0 ] && echo "   note: $REFUSED command(s) refused — refusals exercise the reject path, not the write path"
echo "tsan_command_hammer: PASS — $HAMMER_LANDED commands landed against a live producer over $rounds rounds, no races"
