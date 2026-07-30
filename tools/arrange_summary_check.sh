#!/usr/bin/env bash
# Checks the ARRANGEMENT SUMMARY (roadmap M3.25, kShmVersion 27): the section spine and
# the meter map, published RESOLVED, in one region with one version.
#
# "Build this before drawing a single rectangle" is the roadmap's own instruction, and the
# reason is that the model stores only bar COUNTS. A client deriving positions from those
# would be reimplementing SectionList::resolve, and the first time the two implementations
# disagreed the UI would draw chorus 2 in the wrong place with nothing reporting an error.
# So the engine publishes startBar AND startTick, already prefix-summed through the meter.
#
# The expectations here are computed BY HAND from the fixture, not read back from the
# engine, and the fixture deliberately puts a METER CHANGE INSIDE a section so that the
# derivation has to compose the two — a summary that ignored the meter would pass a
# same-meter fixture.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/arrange_summary_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR44=$((4 * Q))
BAR34=$((3 * Q))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/arrsum_$$"
trap 'rm -rf "$TMP"' EXIT

# intro 4 bars, verse 8, chorus 8 — with the meter dropping to 3/4 at bar 5, which is
# exactly where the verse begins. So the verse and chorus are in 3/4 and the intro is not.
cat > "$TMP/arr.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "arr" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "sections": [ { "id": 1, "name": "intro", "bars": 4 },
                { "id": 2, "name": "verse", "bars": 8 },
                { "id": 3, "name": "chorus", "bars": 8 } ],
  "time_sig_map": [ { "nanotick": 0, "numerator": 4, "denominator": 4 },
                    { "nanotick": $((4 * BAR44)), "numerator": 3, "denominator": 4 } ],
  "clips": [ { "id": 1, "name": "c", "length": $BAR44, "kind": "symbolic", "notes": [] } ],
  "tracks": [ { "track_id": 0, "name": "T",
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $BAR44,
                      "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }

cli do load arr >/dev/null 2>&1 || true
sleep 1.8

SUMMARY="$(cli get arrangement 2>/dev/null || true)"
[ -n "$SUMMARY" ] || fail "the engine published no arrangement summary"

# Pull one field of one section by id.
field() { echo "$SUMMARY" | tr '{' '\n' | grep "\"id\": $1," | sed -n "s/.*\"$2\": \([0-9]*\).*/\1/p" | head -1; }

# BY HAND: the intro is 4 bars of 4/4.
WANT_INTRO_END=$((4 * BAR44))
# The verse begins there and is 8 bars of 3/4.
WANT_VERSE_END=$((4 * BAR44 + 8 * BAR34))
[ "$(field 1 start_tick)" = "0" ] || fail "intro should start at 0"
[ "$(field 1 end_tick)" = "$WANT_INTRO_END" ] || \
  fail "intro should end at $WANT_INTRO_END (4 bars of 4/4), got $(field 1 end_tick)"
[ "$(field 2 start_bar)" = "5" ] || fail "verse should start at bar 5, got $(field 2 start_bar)"
[ "$(field 2 end_tick)" = "$WANT_VERSE_END" ] || \
  fail "verse should end at $WANT_VERSE_END (8 bars of 3/4 after the intro), got
        $(field 2 end_tick) — the summary is not composing the meter map with the spine"
[ "$(field 3 start_bar)" = "13" ] || fail "chorus should start at bar 13"
echo "  resolved: intro ends $WANT_INTRO_END, the 3/4 verse ends $WANT_VERSE_END, chorus at bar 13"

# The meter points come through too, so a ruler can draw accents without a second source.
echo "$SUMMARY" | grep -q '"sig": "3/4"' || fail "the meter map was not published"
echo "  the meter map is published alongside the spine"

# Truncation is reported. It must be ZERO here — a non-zero count on a 3-section project
# would mean the counter is wrong, which is as bad as not having it.
echo "$SUMMARY" | grep -q '"sections_truncated": 0' || \
  fail "a 3-section project reports truncation, so the counter is wrong"

# THE VERSION MOVES ON A SPINE EDIT, and every position with it. This is the property a
# client caches on, so a version that did not move would leave a stale arrangement drawn.
V1="$(echo "$SUMMARY" | sed -n 's/.*"version": \([0-9]*\).*/\1/p' | head -1)"
cli do section length --id 1 --bars 6 >/dev/null 2>&1 || true
sleep 1.2
SUMMARY="$(cli get arrangement 2>/dev/null || true)"
V2="$(echo "$SUMMARY" | sed -n 's/.*"version": \([0-9]*\).*/\1/p' | head -1)"
[ "$V2" != "$V1" ] || fail "the summary version did not move after a section edit ($V1)"

# 6 bars of intro now SPANS the meter change: 4 bars of 4/4 plus 2 of 3/4. A summary that
# multiplied one bar length by the count would get this wrong, which is why the fixture is
# built this way.
WANT_SPANNING=$((4 * BAR44 + 2 * BAR34))
[ "$(field 1 end_tick)" = "$WANT_SPANNING" ] || \
  fail "an intro spanning the meter change should end at $WANT_SPANNING (4 bars of 4/4 +
        2 of 3/4), got $(field 1 end_tick) — the derivation is using one bar length for a
        section whose bars are two different lengths"
echo "  after the edit: version $V1 -> $V2, and the intro correctly spans the meter change"

# A note edit must NOT move the summary version, or a section rename and a typed note
# would invalidate each other's caches.
V3_BEFORE="$V2"
cli do note --track 0 --nanotick 0 --pitch 60 --duration 240000 >/dev/null 2>&1 || true
sleep 1
V3="$(cli get arrangement 2>/dev/null | sed -n 's/.*"version": \([0-9]*\).*/\1/p' | head -1)"
[ "$V3" = "$V3_BEFORE" ] || \
  fail "a NOTE edit moved the arrangement version ($V3_BEFORE -> $V3) — the spine version
        must be independent of the clip version, or the two invalidate each other"
echo "  a note edit leaves the summary version alone ($V3)"

# ---- THE SONG END MUST NOT GO STALE. It rides this region, and it changes on a PLACEMENT
# edit — which moves no section. The rebuild was gated on the section version alone, so a client
# drawing the song end from here kept the value from the last section edit, and could not tell:
# the version it caches on had not moved either. Two facts in one region under one version, and
# only one of the two inputs opening the gate.
END0="$(echo "$SUMMARY" | sed -n 's/.*"song_end_tick": \([0-9]*\).*/\1/p' | head -1)"
VER0="$(echo "$SUMMARY" | sed -n 's/.*"version": \([0-9]*\).*/\1/p' | head -1)"
# Add a placement well past the current end. No section changes.
cli do add-placement --track 0 --clip 1 --at $((40 * BAR44)) --length $BAR44 \
  >/dev/null 2>&1 || true
sleep 1.3
SUMMARY="$(cli get arrangement 2>/dev/null || true)"
END1="$(echo "$SUMMARY" | sed -n 's/.*"song_end_tick": \([0-9]*\).*/\1/p' | head -1)"
VER1="$(echo "$SUMMARY" | sed -n 's/.*"version": \([0-9]*\).*/\1/p' | head -1)"
[ "${END1:-0}" -gt "${END0:-0}" ] || \
  fail "a placement past the song end left song_end_tick at ${END1:-?} (was ${END0:-?}) — the
        region is gated on the section version and a placement edit moves no section, so the
        published song end was stale with nothing to say so"
[ "$VER1" != "$VER0" ] || \
  fail "song_end_tick changed but the region version did not ($VER0), so a client caching on the
        version never re-reads it — a fresh value nobody fetches is still a stale value"
echo "  song end: a placement edit updates song_end_tick AND moves the version ($VER0 -> $VER1)"

# ---- THE METER MAP MUST SURVIVE A SAVE. Everything above reads the meter from the FIXTURE,
# so it all passed while the save wrote no time_sig_map at all: `document` is default
# constructed in the save, timeSigMap was only ever READ (at load, into songMeter), and the
# save emitted the single song-wide numerator/denominator and nothing else. A project with a
# meter change therefore loaded, played and published correctly and then came back from disk
# flattened to one time signature — which moves every section boundary after the first meter
# change, silently. A FRESH ENGINE is required: a same-process reload still holds the meter
# from the load above, so it would pass with the save line deleted.
cli do save arrout >/dev/null 2>&1 || true
sleep 1.4
kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

SAVED_SIGS="$(python3 - "$TMP/arrout.uniproj.json" <<'PYS'
import json, sys
doc = json.load(open(sys.argv[1]))
pts = doc.get("time_sig_map", [])
print(" ".join("%d:%d/%d" % (p["nanotick"], p["numerator"], p["denominator"]) for p in pts))
PYS
)"
WANT_SIGS="0:4/4 $((4 * BAR44)):3/4"
[ "$SAVED_SIGS" = "$WANT_SIGS" ] || \
  fail "the save wrote time_sig_map
        [$SAVED_SIGS]
        expected
        [$WANT_SIGS]
        — without it the song reloads in one time signature and every section boundary
        after the meter change moves"
echo "  save: the meter map is written [$SAVED_SIGS]"

SHM2="/arrsum2_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 16 >"$TMP/engine2.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load arrout >/dev/null 2>&1 || true
sleep 1.8
SUMMARY="$(cli get arrangement 2>/dev/null || true)"
[ -n "$SUMMARY" ] || fail "the reloaded project published no arrangement summary"
# The intro was grown to 6 bars above, so after the reload it must STILL span the meter
# change: 4 bars of 4/4 + 2 of 3/4. A flattened meter would make it 6 bars of 4/4 and put
# every later boundary in the wrong place.
[ "$(field 1 end_tick)" = "$WANT_SPANNING" ] || \
  fail "after save+reload the 6-bar intro ends at $(field 1 end_tick), not $WANT_SPANNING —
        the meter map did not survive, so the song came back in 4/4 throughout"
echo "$SUMMARY" | grep -q '"sig": "3/4"' || fail "the reloaded meter map has no 3/4 point"
echo "  reload (fresh engine): the 3/4 point is back and the intro still spans it"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true
echo "arrange_summary_check: PASS"
