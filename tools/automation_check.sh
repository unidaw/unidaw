#!/usr/bin/env bash
# Checks AUTOMATION (roadmap M3.27): it can be written, it survives a save, and it moves
# with the material it belongs to.
#
# Automation PLAYBACK has existed and been unit-tested since Movement 3 phase 1 — but
# nothing in the engine ever CREATED a clip to play, and nothing persisted one. So the
# feature was unreachable: there was no command to write a point, and had there been, the
# point would have vanished on reload. "Persist the existing automation" turned out to be
# "build it".
#
# THREE PROPERTIES:
#   AUTHOR   a point can be written and lands in the clip (the engine reports the count)
#   PERSIST  it survives save -> reload with its ticks AND its values
#   RIPPLE   a section-length edit carries automation at or after the boundary, and leaves
#            earlier points alone. Without this, inserting bars into the intro slid every
#            note later and left the filter sweep where it was — notes and automation
#            drifting apart by exactly the size of the edit, silently.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/automation_check.sh
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
SHM="/autochk_$$"
trap 'rm -rf "$TMP"' EXIT

# One 4-bar intro section, and a placement inside it plus one after it.
cat > "$TMP/auto.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "auto" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "sections": [ { "id": 1, "name": "intro", "bars": 4 } ],
  "clips": [ { "id": 1, "name": "c", "length": $BAR, "kind": "symbolic", "notes": [] } ],
  "tracks": [ { "track_id": 0, "name": "T",
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $BAR,
                      "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 26 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }
points_of() {  # points_of <file> -> "tick:value tick:value ..."
  python3 - "$1" <<'PYA'
import json, sys
doc = json.load(open(sys.argv[1]))
out = []
for clip in doc["tracks"][0].get("automation", []):
    for p in clip["points"]:
        out.append("%d:%g" % (p["nanotick"], p["value"]))
print(" ".join(out))
PYA
}

cli do load auto >/dev/null 2>&1 || true
sleep 1.6

# AUTHOR: three points inside the intro, and one AFTER it so the ripple has something to
# carry that the earlier ones do not share.
cli do automation --track 0 --param index:0 --nanotick 0 --value 0.0 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $Q --value 0.5 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $((2 * Q)) --value 1.0 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $((5 * BAR)) --value 0.25 >/dev/null 2>&1 || true
sleep 1

grep -q '"created_clip":true' "$TMP/engine.log" || \
  fail "no automation clip was created — the write path did not reach the engine"
LAST="$(grep '"event":"automation.point"' "$TMP/engine.log" | tail -1)"
echo "$LAST" | grep -q '"points":4' || \
  fail "the clip should hold 4 points after 4 writes; the engine reported: $LAST"
echo "  author: 4 points written into one clip"

# PERSIST: ticks AND values. A save that kept the ticks and lost the values would look
# right in a tick-only assertion and play silence.
cli do save autoout >/dev/null 2>&1 || true
sleep 1.4
SAVED="$(points_of "$TMP/autoout.uniproj.json")"
WANT="0:0 ${Q}:0.5 $((2 * Q)):1 $((5 * BAR)):0.25"
[ "$SAVED" = "$WANT" ] || fail "saved automation is
        [$SAVED]
        expected
        [$WANT]"
echo "  persist: ticks and values survive the save"

# A RELOAD must INSTALL it — parsed-but-not-installed is how the mod links were lost (the
# next save wrote an empty list and deleted them from disk).
#
# THIS NEEDS A FRESH ENGINE. Reloading in the same process passed even with the install
# line deleted, because the runtime still held the automation from the writes above —
# nothing had cleared it, so the save looked correct and the test proved nothing. A new
# process starts with empty automation, so a save that comes back full can only have come
# from the load.
kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true
SHM2="/autochk2_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 22 >"$TMP/engine2.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load autoout >/dev/null 2>&1 || true
sleep 1.8
cli do save autoagain >/dev/null 2>&1 || true
sleep 1.4
AGAIN="$(points_of "$TMP/autoagain.uniproj.json")"
[ "$AGAIN" = "$WANT" ] || \
  fail "in a FRESH engine, load -> save lost the automation: got [$AGAIN]. Parsed at load
        and never installed means the next save deletes it, which is exactly how the mod
        links went."
echo "  reload (fresh engine): load -> save is faithful, so the load installed it"

# RIPPLE: lengthen the intro by 2 bars. The point at bar 5 moves; the three inside the
# intro do not.
cli do section length --id 1 --bars 6 >/dev/null 2>&1 || true
sleep 1.3
cli do save autorip >/dev/null 2>&1 || true
sleep 1.4
RIPPLED="$(points_of "$TMP/autorip.uniproj.json")"
WANT_RIP="0:0 ${Q}:0.5 $((2 * Q)):1 $((5 * BAR + 2 * BAR)):0.25"
[ "$RIPPLED" = "$WANT_RIP" ] || \
  fail "after lengthening the intro by 2 bars the automation should be
        [$WANT_RIP]
        got
        [$RIPPLED]
        — a point after the boundary must move with the notes, and one before it must not"
echo "  ripple: the point after the boundary moved 2 bars, the three before it did not"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

# An empty param id is not an automation write.
grep -q '"event":"automation.rejected"' "$TMP/engine.log" && \
  fail "something was rejected that should not have been" || true

# A WRITE THE SAVE WOULD THROW AWAY MUST BE REFUSED, not accepted.
#
# The handler's only check used to be `trackId < tracks.size()`, which is true for three
# kinds of runtime the save deliberately skips: a tombstone (a hole kept to reserve an id),
# a slot past the live count (a leftover of a larger project), and an aux child (derived
# from a plugin's bus layout at load). Automation written to any of them was applied,
# reported with created_clip:true, and then simply absent after the next reload — the same
# silent-loss shape as the mod links that were parsed and never installed.
#
# A TOMBSTONE is the case this can build from nothing: add a track, remove it, write to it.
SHM3="/autochk3_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 14 >"$TMP/engine3.log" 2>&1 ) &
ENG=$!
sleep 2.5
cli() { DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load autoout >/dev/null 2>&1 || true
sleep 1.6
cli do add-track --force >/dev/null 2>&1 || true      # -> track 1
sleep 0.6
cli do remove-track --track 1 --force >/dev/null 2>&1 || true
sleep 0.8
cli do automation --force --track 1 --param index:0 --nanotick 0 --value 0.9 \
  >/dev/null 2>&1 || true
sleep 1
kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

grep -q '"event":"automation.rejected".*"reason":"track_not_persisted"' "$TMP/engine3.log" || \
  fail "automation written to a REMOVED track was not refused. The save skips tombstoned
        tracks, so the points would be reported as written and then be gone after a
        reload, with nothing saying no. The engine's log says:
        $(grep -o '"event":"automation[a-z._]*"[^}]*' "$TMP/engine3.log" | tail -3)"
# And it must not ALSO have been applied — a refusal that still writes is worse than either.
grep '"event":"automation.point"' "$TMP/engine3.log" | grep -q '"track":1' && \
  fail "the write to the removed track was refused AND applied" || true
echo "  refusal: automation on a track the save would discard is refused, not silently lost"

echo "automation_check: PASS"
