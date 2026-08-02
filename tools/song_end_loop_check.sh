#!/usr/bin/env bash
# Checks that the SONG END and the LOOP are separate things (roadmap M3), and that each
# one's rule holds. Both halves are needed and each is a bug on its own.
#
#   GROW:   material added past the old song end must start playing. The loop was
#           computed once at LOAD, so a placement added at bar 4 of a one-bar project was
#           silent forever — you add a section, press play, and hear nothing, with nothing
#           anywhere saying why.
#   SURVIVE: a loop the USER set must not be quietly taken back by a later edit. That is
#           the same bug pointing the other way, and worse, because the user chose that
#           loop on purpose.
#
# A check that only tested GROW would pass an engine that resets the loop on every
# keystroke, and vice versa — so this asserts both against one running engine.
#
# Also pins that AddPlacement REFUSES Resize's leave-unchanged sentinel as a position:
# it used to accept it and create an invisible placement at tick 2^64-1, which then
# poisoned any song-end computation that added a length to it.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/song_end_loop_check.sh
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
SHM="/selchk_$$"
trap 'rm -rf "$TMP"' EXIT

# One bar of arrangement, so the song end is unambiguous and small.
cat > "$TMP/sel.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "sel" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "c", "length": $BAR, "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 240000, "pitch": 60, "velocity": 100,
        "column": 0, "note_id": 1 } ] } ],
  "tracks": [ { "track_id": 0, "name": "T",
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $BAR,
                      "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load sel >/dev/null 2>&1 || true
sleep 1.5

fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }
loop_end() { cli get transport 2>/dev/null | sed -n 's/.*"loop_end": \([0-9]*\).*/\1/p'; }

AFTER_LOAD="$(loop_end)"
[ -n "$AFTER_LOAD" ] || fail "the engine published no loop range — nothing can be measured"
[ "$AFTER_LOAD" = "$BAR" ] || \
  fail "after loading a one-bar project the loop should be $BAR, got $AFTER_LOAD"
echo "  after load: loop_end=$AFTER_LOAD (one bar)"

# GROW. A placement at bar 4 puts the song end at bar 5.
WANT_GROWN=$((5 * BAR))
cli do add-placement --track 0 --clip 1 --at $((4 * BAR)) --length "$BAR" >/dev/null 2>&1 || true
sleep 1
GROWN="$(loop_end)"
[ "$GROWN" = "$WANT_GROWN" ] || \
  fail "a placement at bar 4 should have moved the loop to $WANT_GROWN, got $GROWN —
        material past the old song end will never play"
echo "  after a placement at bar 4: loop_end=$GROWN (grew)"

# SURVIVE. The user chooses a 2-bar loop; a later placement must not take it back.
USER_LOOP=$((2 * BAR))
cli do loop --start 0 --end "$USER_LOOP" >/dev/null 2>&1 || true
sleep 0.8
SET="$(loop_end)"
[ "$SET" = "$USER_LOOP" ] || fail "setting the loop to $USER_LOOP did not take (got $SET)"
cli do add-placement --track 0 --clip 1 --at $((8 * BAR)) --length "$BAR" >/dev/null 2>&1 || true
sleep 1
SURVIVED="$(loop_end)"
[ "$SURVIVED" = "$USER_LOOP" ] || \
  fail "a placement edit changed the user's loop from $USER_LOOP to $SURVIVED — a loop
        chosen by hand must not be quietly taken back"
echo "  user loop $USER_LOOP survived a later placement edit"

# The song end must still have MOVED under the hood, or "survive" is being satisfied by
# the song end not tracking at all — which would silently break GROW again later.
cli do add-placement --track 0 --clip 1 --at $((12 * BAR)) --length "$BAR" >/dev/null 2>&1 || true
sleep 1

# The sentinel is not a position.
cli do add-placement --track 0 --clip 1 >/dev/null 2>&1 || true
sleep 0.8

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

MOVES="$(grep -c '"event":"song.end_moved"' "$TMP/engine.log" || true)"
[ "${MOVES:-0}" -ge 3 ] || \
  fail "the song end only moved ${MOVES:-0} time(s) across 4 placement edits; it is not
        tracking the arrangement, so GROW is passing by luck"
echo "  song end moved ${MOVES} times, independently of the user's loop"

grep -q '"event":"placement.add_rejected"' "$TMP/engine.log" || \
  fail "AddPlacement accepted the leave-unchanged sentinel as a position — that creates
        an invisible placement at the end of time"
echo "  AddPlacement refuses the leave-unchanged sentinel as a position"

echo "song_end_loop_check: PASS"
