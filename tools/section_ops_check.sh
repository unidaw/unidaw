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

# <name> <sections-json> <placements-json> [tempo-json] [harmony-json]
mk() {
  set +u
  # NOT `${4:-[ { ... } ]}`: a `}` inside a ${var:-default} closes the expansion, so that
  # default produced `[ { "nanotick": 0, "bpm": 120 ` plus a stray `]}` — malformed JSON, a
  # load that silently failed, and a check that died with no output at all.
  local tempo="$4"
  local harmony="$5"
  [ -n "$tempo" ] || tempo='[ { "nanotick": 0, "bpm": 120 } ]'
  [ -n "$harmony" ] || harmony='[]'
  set -u
  cat > "$TMP/$1.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "$1" }, "nanoticks_per_quarter": $Q,
  "tempo_map": $tempo, "harmony_timeline": $harmony,
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
# A SECOND ripple fixture carrying a tempo change and a key change on BOTH sides of the
# boundary, so the ripple has something to carry and something it must leave alone.
mk timelines \
  '[ { "id": 1, "name": "intro", "bars": 4 }, { "id": 2, "name": "verse", "bars": 8 } ]' \
  "[ { \"clip_id\": 1, \"id\": 1, \"at\": 0, \"length\": $BAR, \"notes\": [], \"chords\": [], \"mutes\": [] },
     { \"clip_id\": 1, \"id\": 2, \"at\": $((4 * BAR)), \"length\": $BAR, \"notes\": [], \"chords\": [], \"mutes\": [] } ]" \
  "[ { \"nanotick\": 0, \"bpm\": 120 },
     { \"nanotick\": $((2 * BAR)), \"bpm\": 90 },
     { \"nanotick\": $((4 * BAR)), \"bpm\": 140 } ]" \
  "[ { \"nanotick\": $((1 * BAR)), \"root\": 0, \"scale_id\": 0, \"flags\": 0 },
     { \"nanotick\": $((4 * BAR)), \"root\": 7, \"scale_id\": 0, \"flags\": 0 } ]"
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
  # `|| true`: with no matching extent the grep exits 1, and under `set -o pipefail` inside
  # `P2="$(at_of 2)"` that killed the whole script before it printed a single line — so a load
  # that had failed looked identical to a crash.
  { cli get extents 2>/dev/null \
    | tr '{' '\n' \
    | grep "\"placement\": $1," \
    | sed -n 's/.*"start": \([0-9]*\).*/\1/p' | head -1; } || true
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

# ---- THE SONG-LEVEL TIMELINES RIPPLE TOO.
#
# The ripple moved placements and automation and left a tempo change and a key change sitting
# at their absolute ticks — so inserting bars into the intro slid the material later and left
# the tempo change and the modulation firing in the middle of what used to follow them. The
# comment on the automation ripple makes exactly that argument; it was never applied to these.
#
# The fixture straddles the boundary DELIBERATELY: a tempo point at bar 2 and a key change at
# bar 1 are inside the intro and must not move, while the tempo point and key change at bar 4
# are at the boundary and must. A test with only later-side entries would pass an engine that
# moved everything, which is just as wrong and much harder to notice.
SHM2="/socchk2_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 20 >"$TMP/engine2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/engine2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load timelines >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  if grep -q '"event":"project.load"' "$TMP/engine2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
sleep 1
# Grow the intro 4 -> 8 bars: everything at or after bar 4 moves by 4 bars.
cli do section length --id 1 --bars 8 >/dev/null 2>&1 || true
sleep 1.3
cli do save timeout >/dev/null 2>&1 || true
sleep 1.6
kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true

TL="$(python3 - "$TMP/timeout.uniproj.json" <<'PYT'
import json, sys
doc = json.load(open(sys.argv[1]))
t = " ".join("%d:%g" % (p["nanotick"], p["bpm"]) for p in doc.get("tempo_map", []))
h = " ".join("%d:%d" % (e["nanotick"], e["root"]) for e in doc.get("harmony_timeline", []))
print("TEMPO[%s] HARMONY[%s]" % (t, h))
PYT
)"
WANT_TL="TEMPO[0:120 $((2 * BAR)):90 $((8 * BAR)):140] HARMONY[$((1 * BAR)):0 $((8 * BAR)):7]"
[ "$TL" = "$WANT_TL" ] || fail "after growing the intro by 4 bars the song timelines should be
        $WANT_TL
        got
        $TL
        — a tempo or key change at the boundary must move with the material, and one inside
        the section must not"
echo "  timelines: the tempo change and key change at the boundary moved 4 bars, the earlier ones did not"

# The engine says how much it carried, so a no-op is distinguishable from a ripple that worked.
grep -q '"tempo_points_moved":1' "$TMP/engine2.log" || \
  fail "the ripple did not report moving exactly 1 tempo point"
grep -q '"harmony_events_moved":1' "$TMP/engine2.log" || \
  fail "the ripple did not report moving exactly 1 harmony event"
echo "  and reports what it carried (1 tempo point, 1 key change)"

# ---- A GROW THAT WOULD SPLIT A PLACEMENT IS REFUSED. The shrink already refuses anything
# OVERLAPPING the bars it removes, on the argument that a straddler would be silently truncated.
# The grow did not, and left the straddler exactly where it was while everything after it moved —
# so the inserted bars landed INSIDE it: its notes go on sounding across bars that are supposed
# to be new and empty, and it now overlaps whatever slid away.
#
# There is no answer to pick silently. Split it? Stretch it? Accept the overlap? Each is a
# different musical intention and the command carries none of them.
python3 - "$TMP/straddle.uniproj.json" "$Q" <<'PYS'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1}]}
# Section 1 ends at bar 5 (4 * BAR). Placement 1 spans bars 4-6, straddling it; placement 2 sits
# wholly after, so a grow would move it while the straddler stayed.
tr = {"track_id": 0, "name": "A", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 3 * BAR, "length": 2 * BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 1, "id": 2, "at": 6 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "straddle"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "sections": [{"id": 1, "name": "intro", "bars": 4},
                        {"id": 2, "name": "verse", "bars": 8}],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PYS

SHM3="/socchk3_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 18 >"$TMP/engine3.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/engine3.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load straddle >/dev/null 2>&1 || true
for _ in $(seq 1 80); do
  if grep -q '"event":"project.load"' "$TMP/engine3.log" 2>/dev/null; then break; fi
  sleep 0.25
done
sleep 1
cli do section length --id 1 --bars 6 >/dev/null 2>&1 || true
sleep 1.3
cli do save straddleout --force >/dev/null 2>&1 || true
sleep 1.6
kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true

grep '"event":"section.rejected"' "$TMP/engine3.log" | grep -q '"reason":"straddling_placement"' || \
  fail "growing a section whose end falls INSIDE a placement was not refused. What happens
        instead is invisible: the straddler keeps its start and length while everything after it
        moves away, so the inserted bars end up inside it and it overlaps the material that
        slid: $(grep -o '"event":"section[a-z._]*"[^}]*' "$TMP/engine3.log" | tail -1)"
grep '"event":"section.rejected"' "$TMP/engine3.log" | tail -1 | grep -q '"blocking_placement":1' || \
  fail "the refusal did not name placement 1 as the straddler"
ST="$(python3 - "$TMP/straddleout.uniproj.json" <<'PYST'
import json, sys
doc = json.load(open(sys.argv[1]))
pl = " ".join("%d@%d" % (p["id"], p["at"]) for p in doc["tracks"][0]["placements"])
sec = " ".join("%d:%d" % (s["id"], s["bars"]) for s in doc.get("sections", []))
print("PL[%s] SEC[%s]" % (pl, sec))
PYST
)"
WANT_ST="PL[1@$((3 * BAR)) 2@$((6 * BAR))] SEC[1:4 2:8]"
[ "$ST" = "$WANT_ST" ] || \
  fail "the refused grow still changed something: expected $WANT_ST, got $ST. A refusal has to be
        whole — a half-applied ripple across tracks is a corrupted arrangement with no undo entry"
echo "  refuse: a grow whose boundary falls inside placement 1 is refused, and nothing moved"

echo "section_ops_check: PASS"
