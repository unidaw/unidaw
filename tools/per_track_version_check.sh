#!/usr/bin/env bash
# Checks PER-TRACK CLIP VERSIONS (roadmap M2.17): edit acceptance is scoped to the track
# being edited, not to one global counter.
#
# The property that matters is CONCURRENCY, so the test must model two authors who each
# read a base, and only then write — with the other author's write landing in between.
# A test that reads the version immediately before each send can never fail, because the
# read always sees the other author's edit already applied. That is why `do note` takes
# --base: both bases are captured up front, before either write.
#
#   author A: base0 = track 0's version   author B: base1 = track 1's version
#   A writes track 0 with base0           -> accepted
#   B writes track 1 with base1           -> accepted    (globally, base1 is now stale)
#   A writes track 0 with base0 AGAIN     -> REJECTED    (same track, genuinely stale)
#
# The middle line is the fix; the last line is the guard rail that proves the fix did not
# simply disable version checking. Run it against the pre-fix engine and B is rejected.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/per_track_version_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/ptvchk_$$"
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. This trap removed
# $TMP and left the engine running, so a timed-out check orphaned it and ctest then blocked on the
# orphan — ~1000s per timeout, measured across 18 runs. override was the demonstrated case: 909.87s
# against a TIMEOUT of 600 while passing standalone in 23.2s.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/ptv.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "ptv" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [
    { "id": 1, "name": "a", "length": $((16*Q)), "kind": "midi", "events": [] },
    { "id": 2, "name": "b", "length": $((16*Q)), "kind": "midi", "events": [] } ],
  "tracks": [
    { "track_id": 0, "name": "A",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 1, "at": 0, "length": $((16*Q)),
                        "notes": [], "chords": [], "mutes": [] } ] },
    { "track_id": 1, "name": "B",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 2, "at": 0, "length": $((16*Q)),
                        "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 22 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load ptv --force >/dev/null 2>&1 || true
sleep 1.5

version_of() {  # version_of <track>
  cli get notes --track "$1" 2>/dev/null | sed -n 's/.*"clip_version": \([0-9]*\).*/\1/p'
}

BASE0="$(version_of 0)"
BASE1="$(version_of 1)"
echo "  bases captured before any write: track0=$BASE0 track1=$BASE1"

fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }

# PRECONDITION: both tracks must actually be published, or every later read returns
# nothing and the assertions below pass vacuously.
[ -n "$BASE0" ] && [ -n "$BASE1" ] || fail "engine published no clip region for both tracks"

# --- A writes track 0 against its own base.
cli do note --force --track 0 --base "$BASE0" --nanotick 0 --pitch 60 --duration $Q >/dev/null
sleep 0.6
# --- B writes track 1 against the base it read BEFORE A's write.
cli do note --force --track 1 --base "$BASE1" --nanotick 0 --pitch 64 --duration $Q >/dev/null
sleep 0.6
# --- A writes track 0 again against the SAME (now stale) base: must be refused.
cli do note --force --track 0 --base "$BASE0" --nanotick $((4*Q)) --pitch 67 --duration $Q >/dev/null
sleep 0.8

N0="$(cli get notes --track 0 | sed -n 's/.*"note_count": \([0-9]*\).*/\1/p')"
N1="$(cli get notes --track 1 | sed -n 's/.*"note_count": \([0-9]*\).*/\1/p')"
REJECTED=$(grep -c '"outcome":"rejected:version"' "$TMP/history.jsonl" 2>/dev/null || echo 0)
echo "  track0 notes=$N0  track1 notes=$N1  rejections=$REJECTED"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

[ "$N1" = "1" ] || fail "track 1's edit did not land (notes=$N1) — a concurrent author on
        track 0 invalidated it, which is exactly the collision M2.17 removes"
[ "$N0" = "1" ] || fail "track 0 has $N0 notes, expected exactly 1: the first write must
        land and the stale re-write must not"
[ "$REJECTED" = "1" ] || fail "expected exactly 1 version rejection, saw $REJECTED — a
        stale edit to the SAME track must still be refused, or per-track versioning has
        just turned version checking off"

echo "per_track_version_check: PASS"
