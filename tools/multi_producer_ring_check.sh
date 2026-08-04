#!/usr/bin/env bash
# Checks the MULTI-PRODUCER COMMAND RING (roadmap M2.18): several processes can write
# the UI command ring at once without losing each other's commands.
#
# The old ring was single-producer — a writer read writeIndex, filled that slot, and
# stored writeIndex back. Two writers that interleave between the read and the store
# both claim the same slot, both write it, and exactly one command disappears with no
# error anywhere. That silent loss is why `daw-cli do` demanded --force.
#
# A test with one command per process would almost never catch it: the race window is a
# few instructions and process startup dwarfs it. So each producer writes a long phrase
# (many ring writes back to back) and they all run at once — hundreds of writes densely
# overlapped. Each producer owns its own track, so per-track versioning (M2.17) accepts
# them all and the ONLY thing that can lose a note is the ring itself.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/multi_producer_ring_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
PRODUCERS=4
NOTES=48   # per producer; 4x48 = 192 ring writes crammed into a few milliseconds

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/mprchk_$$"
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. Without this the
# engine was orphaned and ctest then blocked on it — ~1000s per timeout, measured across 18 runs.
# stop_engine escalates to SIGKILL after 10s and says so.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT

# One track per producer, each with a placement long enough to hold the phrase.
LEN=$((NOTES * Q + 4 * Q))
{
  printf '{ "schema_version": 4, "meta": { "name": "mpr" }, "nanoticks_per_quarter": %d,\n' "$Q"
  printf '  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],\n'
  printf '  "clips": ['
  for t in $(seq 0 $((PRODUCERS - 1))); do
    [ "$t" = "0" ] || printf ','
    printf '\n    { "id": %d, "name": "c%d", "length": %d, "kind": "midi", "events": [] }' \
      "$((t + 1))" "$t" "$LEN"
  done
  printf ' ],\n  "tracks": ['
  for t in $(seq 0 $((PRODUCERS - 1))); do
    [ "$t" = "0" ] || printf ','
    printf '\n    { "track_id": %d, "name": "T%d",' "$t" "$t"
    printf ' "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },'
    printf ' "device_chain": [], "mod_links": [],'
    printf ' "placements": [ { "clip_id": %d, "at": 0, "length": %d,' "$((t + 1))" "$LEN"
    printf ' "notes": [], "chords": [], "mutes": [] } ] }'
  done
  printf ' ] }\n'
} > "$TMP/mpr.uniproj.json"

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
# Wait for the engine to be READY rather than sleeping a guessed amount. The pattern is
# "UI: command thread started" because this engine boots with no project, so wait_for_boot's
# default (a project.load) would never appear; that thread reads the command ring, so it is
# the marker that means "ready to be told something".
wait_for_boot "$TMP/engine.log" "$ENG" 80 "UI: command thread started"
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load mpr >/dev/null 2>&1 || true
sleep 1.5

fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }

# PRECONDITION: every track must be published before the writers start, or a producer
# writes into nothing and the run proves only that the fixture failed to load.
for t in $(seq 0 $((PRODUCERS - 1))); do
  cli get notes --track "$t" >/dev/null 2>&1 || fail "track $t was never published"
done

# Build each producer's pitch list, then launch them all at once. `do notes` issues one
# ring write per pitch with no pause between them, which is the density this needs.
pitches_for() {
  local t="$1" out="" p
  for i in $(seq 0 $((NOTES - 1))); do
    p=$((36 + t * 12 + i % 12))
    out="${out:+$out,}$p"
  done
  printf '%s' "$out"
}

echo "  launching $PRODUCERS concurrent producers x $NOTES notes each"
for t in $(seq 0 $((PRODUCERS - 1))); do
  cli do notes --track "$t" --pitches "$(pitches_for "$t")" --start 0 --step "$Q" \
    --duration $((Q / 2)) >"$TMP/producer_$t.out" 2>&1 &
done
wait $(jobs -p | grep -v "$ENG" 2>/dev/null) 2>/dev/null || true
sleep 2.5

ok=1
total=0
for t in $(seq 0 $((PRODUCERS - 1))); do
  n="$(cli get notes --track "$t" | sed -n 's/.*"note_count": \([0-9]*\).*/\1/p')"
  n="${n:-0}"
  total=$((total + n))
  if [ "$n" != "$NOTES" ]; then
    echo "  track $t: $n/$NOTES notes  <-- LOST $((NOTES - n))"
    ok=0
  else
    echo "  track $t: $n/$NOTES notes"
  fi
done

# The engine logs a retired slot only when a producer died mid-write; none did here, so
# seeing one means the recovery path fired against a live producer.
if grep -q "ring.abandoned_slot" "$TMP/engine.log" 2>/dev/null; then
  echo "  a ring slot was retired as abandoned, but every producer exited normally"
  ok=0
fi

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

[ "$ok" = "1" ] || fail "$total of $((PRODUCERS * NOTES)) commands survived — concurrent
        writers are still overwriting each other's ring slots"

echo "  $total/$((PRODUCERS * NOTES)) commands survived $PRODUCERS-way concurrent writing"
echo "multi_producer_ring_check: PASS"
