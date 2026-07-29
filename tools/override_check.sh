#!/usr/bin/env bash
# THE ACCEPTANCE CRITERION FOR MOVEMENT 3, both halves, in one test:
#
#   "fix the bass in chorus 1, all three choruses change, and the hat you added to
#    chorus 3 survives. Both halves of that sentence are true in no shipping DAW."
#
# The two halves pull in opposite directions, which is why they belong in one test. A
# design that routes every edit to the CLIP satisfies the first and destroys the second
# (the hat appears in all three choruses). One that routes every edit to the PLACEMENT
# satisfies the second and destroys the first (the bass fix reaches only chorus 1). So
# the engine takes an explicit scope, and this asserts both outcomes from one fixture.
#
# The fixture is built so that a wrong answer is unmistakable: THREE placements of ONE
# bass clip (the three choruses) and a 1-BAR hat clip placed across 4 bars, so the hat
# clip LOOPS. That loop is what broke the second half before `adds` were re-ruled — an
# add merged into the clip's events either vanished past the clip length or repeated on
# every iteration.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/override_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/ovrchk_$$"
trap 'rm -rf "$TMP"' EXIT

# Track 0: a 4-bar BASS clip placed three times — chorus 1 at bar 1, chorus 2 at bar 9,
#          chorus 3 at bar 17. One clip, three appearances.
# Track 1: a 1-BAR HAT clip placed across 4 bars at chorus 3, so it loops four times.
cat > "$TMP/ovr.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "ovr" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [
    { "id": 1, "name": "bass", "length": $((4 * BAR)), "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 240000, "pitch": 36, "velocity": 100,
        "column": 0, "note_id": 1 } ] },
    { "id": 2, "name": "hat", "length": $BAR, "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 60000, "pitch": 42, "velocity": 90,
        "column": 0, "note_id": 2 } ] } ],
  "tracks": [
    { "track_id": 0, "name": "Bass",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [
        { "clip_id": 1, "id": 11, "at": 0, "length": $((4 * BAR)), "notes": [], "chords": [], "mutes": [] },
        { "clip_id": 1, "id": 12, "at": $((8 * BAR)), "length": $((4 * BAR)), "notes": [], "chords": [], "mutes": [] },
        { "clip_id": 1, "id": 13, "at": $((16 * BAR)), "length": $((4 * BAR)), "notes": [], "chords": [], "mutes": [] } ] },
    { "track_id": 1, "name": "Hat",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [
        { "clip_id": 2, "id": 21, "at": $((16 * BAR)), "length": $((4 * BAR)), "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }
# How many notes of this pitch does the track play?
count_pitch() {
  cli get notes --track "$1" 2>/dev/null | grep -c "\"pitch\": $2," || true
}

cli do load ovr >/dev/null 2>&1 || true
# WAIT for the load rather than sleeping a guessed amount. A fixed sleep read 0 notes on
# a busy machine once, which looks exactly like the feature being broken — the worst kind
# of flake, because it accuses the code under test.
for _ in $(seq 1 40); do
  [ "$(count_pitch 0 36)" = "3" ] && break
  sleep 0.25
done

BASS0="$(count_pitch 0 36)"
HAT0="$(count_pitch 1 42)"
[ "$BASS0" = "3" ] || fail "the bass clip should sound 3 times (one per chorus), got $BASS0"
[ "$HAT0" = "4" ] || fail "the 1-bar hat clip across 4 bars should LOOP 4 times, got $HAT0"
echo "  loaded: bass 3 (one per chorus), hat 4 (the 1-bar clip looping)"

# ---- HALF ONE: fix the bass in chorus 1, and ALL THREE choruses change.
# A CLIP-scope edit (the default, no --local) writes to the clip, so every appearance of
# it gains the note. Chorus 1 is at bar 1; the new note goes at bar 2 of it.
cli do note --track 0 --nanotick $((1 * BAR)) --pitch 38 --duration 240000 >/dev/null 2>&1 || true
sleep 1.2
FIX="$(count_pitch 0 38)"
[ "$FIX" = "3" ] || \
  fail "a clip-scope edit in chorus 1 should appear in all THREE choruses, got $FIX —
        half one of the acceptance sentence fails"
echo "  half 1: a clip-scope fix in chorus 1 appears in all 3 choruses"

# ---- HALF TWO: add a hat to chorus 3 ONLY, and it stays there.
# A LOCAL edit records an `add` on that placement. The hat clip is 1 bar and loops four
# times across the placement, so a wrong implementation gives 4 (merged into the clip and
# repeated per iteration) or 0 (dropped past the clip length) — never 1.
cli do note --track 1 --local --nanotick $((18 * BAR + 2 * Q)) --pitch 46 \
  --duration 60000 >/dev/null 2>&1 || true
sleep 1.2
ADDED="$(count_pitch 1 46)"
[ "$ADDED" = "1" ] || \
  fail "the hat added to chorus 3 should sound EXACTLY ONCE, got $ADDED — 4 means it was
        merged into the clip and looped with it, 0 means it was dropped past the clip's
        length. Half two of the acceptance sentence fails."
echo "  half 2: the hat added to chorus 3 sounds exactly once"

# The base hat is untouched — a local ADD must not disturb what was already there.
HAT1="$(count_pitch 1 42)"
[ "$HAT1" = "4" ] || fail "the base hat should still loop 4 times, got $HAT1"

# ---- BOTH AT ONCE. Another clip-scope bass fix must STILL reach all three, with the
# local hat in place. This is the sentence's "and" — a design that satisfies the halves
# only separately fails here.
cli do note --track 0 --nanotick $((2 * BAR)) --pitch 40 --duration 240000 >/dev/null 2>&1 || true
sleep 1.2
FIX2="$(count_pitch 0 40)"
STILL="$(count_pitch 1 46)"
[ "$FIX2" = "3" ] || fail "the second clip-scope fix reached $FIX2 choruses, not 3"
[ "$STILL" = "1" ] || fail "the local hat did not survive a later clip edit (now $STILL)"
echo "  both: a second clip fix still reaches all 3, and the local hat survives it"

# ---- A LOCAL DELETE mutes a base note in ONE appearance only.
cli do delete-note --track 0 --local --nanotick $((8 * BAR)) --pitch 36 >/dev/null 2>&1 || true
sleep 1.2
AFTER_MUTE="$(count_pitch 0 36)"
[ "$AFTER_MUTE" = "2" ] || \
  fail "muting the bass in chorus 2 should leave 2 of 3 sounding, got $AFTER_MUTE — a
        local delete must not reach the clip"
echo "  local delete: the bass is silenced in chorus 2 only (2 of 3 remain)"

# ---- ONE-CLICK REVERT clears the overrides on one appearance and leaves the clip alone.
cli do revert-overrides --track 1 --placement 21 >/dev/null 2>&1 || true
sleep 1.2
REVERTED="$(count_pitch 1 46)"
BASE_LEFT="$(count_pitch 1 42)"
[ "$REVERTED" = "0" ] || fail "revert left $REVERTED added hats"
[ "$BASE_LEFT" = "4" ] || \
  fail "revert removed base notes too ($BASE_LEFT of 4 left) — it must clear only the
        overrides"
echo "  revert: the added hat is gone, the clip's own 4 hats are untouched"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

grep -q '"event":"overrides.reverted"' "$TMP/engine.log" || \
  fail "the revert did not report what it cleared"
grep -q '"event":"local_edit.applied"' "$TMP/engine.log" || \
  fail "no local edit was reported, so the --local flag may not have reached the engine"
echo "  the engine reported the local edits and the revert"

echo "override_check: PASS — both halves of the Movement 3 sentence hold at once"
