#!/usr/bin/env bash
# Checks the SECTION ops end to end (roadmap M3.23): the spine, the RIPPLE, and the
# refusal.
#
# A section stores a name and a length in BARS and never a start position, so the only
# way to move one is to change a length or the order — and changing a length has to carry
# every placement after it, or the edit silently overwrites the material it was supposed
# to push aside. That ripple is the part worth testing end to end; the arithmetic is
# already pinned by section_list_tests.
#
# THREE PROPERTIES, and the third is the one a naive implementation gets wrong:
#   GROW      lengthening a section moves what follows, by exactly the delta
#   ROUND TRIP  growing then shrinking returns everything to where it started (the bars
#             vacated by the grow are empty, so the shrink is legal)
#   REFUSE    shrinking INTO OCCUPIED bars is refused WHOLE — the spine does not change,
#             the material does not move, and the refusal names the placement in the way.
#             The hazard is not that the material would be destroyed (the ripple only
#             touches what is at or after the boundary, so it would stay put); it is that
#             every LATER SECTION BOUNDARY would slide over it, silently moving a
#             placement from the intro into the verse with no note changed and nothing to
#             see. A check that only asserted "the placement did not move" would pass an
#             engine that re-sectioned the song, because the placement does not move
#             either way — so this asserts the SPINE is unchanged.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/section_ops_check.sh
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
SHM="/socchk_$$"
trap 'rm -rf "$TMP"' EXIT

# <name> <sections-json> <placements-json>
mk() {
  cat > "$TMP/$1.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "$1" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "sections": $2,
  "clips": [ { "id": 1, "name": "c", "length": $BAR, "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 240000, "pitch": 60, "velocity": 100,
        "column": 0, "note_id": 1 } ] } ],
  "tracks": [ { "track_id": 0, "name": "T",
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": $3 } ] }
EOF
}
mk ripple \
  '[ { "id": 1, "name": "intro", "bars": 4 }, { "id": 2, "name": "verse", "bars": 8 } ]' \
  "[ { \"clip_id\": 1, \"id\": 1, \"at\": 0, \"length\": $BAR, \"notes\": [], \"chords\": [], \"mutes\": [] },
     { \"clip_id\": 1, \"id\": 2, \"at\": $((4 * BAR)), \"length\": $BAR, \"notes\": [], \"chords\": [], \"mutes\": [] } ]"
mk blocked \
  '[ { "id": 1, "name": "intro", "bars": 8 } ]' \
  "[ { \"clip_id\": 1, \"id\": 9, \"at\": $((6 * BAR)), \"length\": $BAR, \"notes\": [], \"chords\": [], \"mutes\": [] } ]"

( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }

# Where is placement N right now, per the engine's published extents?
at_of() {
  cli get extents 2>/dev/null \
    | tr '{' '\n' \
    | grep "\"placement\": $1," \
    | sed -n 's/.*"start": \([0-9]*\).*/\1/p' | head -1
}

cli do load ripple >/dev/null 2>&1 || true
sleep 1.5
P2="$(at_of 2)"
[ -n "$P2" ] || fail "the engine published no extent for placement 2 — nothing to measure"
[ "$P2" = "$((4 * BAR))" ] || fail "placement 2 should load at $((4 * BAR)), got $P2"
echo "  loaded: placement 2 at bar 4 ($P2)"

# GROW the intro 4 -> 8 bars. Placement 2 must move by exactly 4 bars.
cli do section length --id 1 --bars 8 >/dev/null 2>&1 || true
sleep 1.2
GROWN="$(at_of 2)"
WANT=$((8 * BAR))
[ "$GROWN" = "$WANT" ] || \
  fail "after lengthening the intro by 4 bars, placement 2 should be at $WANT, got
        $GROWN — the ripple did not carry it, so the section edit overwrote it"
echo "  grow: placement 2 moved to bar 8 ($GROWN)"

# ROUND TRIP. The bars the grow vacated are empty, so shrinking back is legal and must
# put everything exactly where it was.
cli do section length --id 1 --bars 4 >/dev/null 2>&1 || true
sleep 1.2
BACK="$(at_of 2)"
[ "$BACK" = "$P2" ] || \
  fail "grow then shrink should be a round trip: placement 2 started at $P2 and came
        back to $BACK"
echo "  round trip: placement 2 back at $BACK"

# REFUSE. A placement INSIDE the bars being removed blocks the shrink.
cli do load blocked >/dev/null 2>&1 || true
sleep 1.5
BEFORE9="$(at_of 9)"
[ "$BEFORE9" = "$((6 * BAR))" ] || fail "placement 9 should load at $((6 * BAR)), got $BEFORE9"
cli do section length --id 1 --bars 4 >/dev/null 2>&1 || true
sleep 1.2
AFTER9="$(at_of 9)"
[ "$AFTER9" = "$BEFORE9" ] || \
  fail "a shrink into occupied bars moved placement 9 from $BEFORE9 to $AFTER9 — it must
        be refused whole, not applied partially"

# THE ASSERTION THAT ACTUALLY DISTINGUISHES. The placement does not move either way, so
# what must be checked is that the SPINE did not change — otherwise the intro would now
# be 4 bars, every later boundary would have slid 4 bars earlier, and placement 9 would
# have been silently re-sectioned. Saving is the only way to read the engine's live spine.
cli do save blockedout >/dev/null 2>&1 || true
sleep 1.3
SPINE_BARS="$(python3 - "$TMP/blockedout.uniproj.json" <<'PYS'
import json, sys
doc = json.load(open(sys.argv[1]))
print(next((s["bars"] for s in doc.get("sections", []) if s["id"] == 1), -1))
PYS
)"
[ "$SPINE_BARS" = "8" ] || \
  fail "the refused shrink still changed the spine: the intro is $SPINE_BARS bars, not 8.
        Every later section boundary has moved over material that did not, so a placement
        has been silently re-sectioned"
echo "  refuse: placement 9 unmoved at $AFTER9, and the spine is still 8 bars"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

# The refusal must NAME what is in the way, or the user is told "no" with no way forward.
grep -q '"reason":"content_in_removed_bars"' "$TMP/engine.log" || \
  fail "the shrink was refused without saying why"
grep -q '"blocking_placement":9' "$TMP/engine.log" || \
  fail "the refusal did not name the placement in the way — 'something is in the way' is
        not actionable"
echo "  the refusal names placement 9 as the blocker"

# And the successful ripples reported how much they moved, so a caller can tell a no-op
# from a ripple that did work.
MOVED="$(grep -c '"event":"section.length_set"' "$TMP/engine.log" || true)"
[ "${MOVED:-0}" -ge 2 ] || fail "expected 2 successful length changes, saw ${MOVED:-0}"
grep -q '"placements_moved":1' "$TMP/engine.log" || \
  fail "a successful ripple did not report how many placements it moved"
echo "  successful ripples report their moved count"

echo "section_ops_check: PASS"
