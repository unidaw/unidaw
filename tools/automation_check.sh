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
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/autochk_$$"
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

# One 4-bar intro section, and a placement inside it plus one after it.
cat > "$TMP/auto.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "auto" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "c", "length": $BAR, "kind": "symbolic", "notes": [] } ],
  "tracks": [ { "track_id": 0, "name": "T",
    "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
    "device_chain": [], "mod_links": [],
    "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $BAR,
                      "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 26 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
# These engines start with NO project — the check loads one by command afterwards — so
# wait_for_boot's default pattern (a project.load) would never appear. Wait for the UI
# command thread instead: that is the thread that reads the ring these commands go into,
# so it is the marker that actually means "ready to be told something".
wait_for_boot "$TMP/engine.log" "$ENG" 80 "UI: command thread started"
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
wait_for_event "$TMP/engine.log" '"event":"project.load"' 80 "the project to load" "$ENG"

# AUTHOR: three points inside the intro, and one AFTER it so the ripple has something to
# carry that the earlier ones do not share.
cli do automation --track 0 --param index:0 --nanotick 0 --value 0.0 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $Q --value 0.5 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $((2 * Q)) --value 1.0 >/dev/null 2>&1 || true
cli do automation --track 0 --param index:0 --nanotick $((5 * BAR)) --value 0.25 >/dev/null 2>&1 || true
wait_for_event_count "$TMP/engine.log" '"event":"automation.point"' 4 80 \
                     "four automation writes" "$ENG"

grep -q '"created_clip":true' "$TMP/engine.log" || \
  fail "no automation clip was created — the write path did not reach the engine"
LAST="$(grep '"event":"automation.point"' "$TMP/engine.log" | tail -1)"
echo "$LAST" | grep -q '"points":4' || \
  fail "the clip should hold 4 points after 4 writes; the engine reported: $LAST"
echo "  author: 4 points written into one clip"

# CORRECTING A POINT REPLACES IT. addPoint used to insert unconditionally, so writing a new
# value at a tick that already had one left BOTH — the value could never be fixed, and the
# file grew by a point on every attempt. Rewriting the point at 1 quarter (0.5 above) must
# leave the count at 4, not take it to 5.
cli do automation --track 0 --param index:0 --nanotick $Q --value 0.125 >/dev/null 2>&1 || true
# THE FIFTH EVENT, not '"points":4' — that string is already in the log from the four
# writes above, so waiting on it would match instantly and wait for nothing.
wait_for_event_count "$TMP/engine.log" '"event":"automation.point"' 5 80 \
                     "the corrected point" "$ENG"
LAST="$(grep '"event":"automation.point"' "$TMP/engine.log" | tail -1)"
echo "$LAST" | grep -q '"points":4' || \
  fail "rewriting the point at one quarter should still leave 4 points, so a value can be
        corrected rather than doubled; the engine reported: $LAST"
echo "  correct: rewriting a point at an existing tick replaces it (still 4)"

# PERSIST: ticks AND values. A save that kept the ticks and lost the values would look
# right in a tick-only assertion and play silence.
cli do save autoout >/dev/null 2>&1 || true
# Wait for the document to PARSE rather than for a duration: a half-written file is the
# race the fixed sleep was covering, and points_of would throw on it.
wait_until 20 python3 -c "import json;json.load(open('$TMP/autoout.uniproj.json'))"
SAVED="$(points_of "$TMP/autoout.uniproj.json")"
WANT="0:0 ${Q}:0.125 $((2 * Q)):1 $((5 * BAR)):0.25"
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
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 22 >"$TMP/engine2.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/engine2.log" "$ENG" 80 "UI: command thread started"
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load autoout >/dev/null 2>&1 || true
wait_for_event "$TMP/engine2.log" '"event":"project.load"' 80 "the project to load" "$ENG"
cli do save autoagain >/dev/null 2>&1 || true
wait_until 20 python3 -c "import json;json.load(open('$TMP/autoagain.uniproj.json'))"
AGAIN="$(points_of "$TMP/autoagain.uniproj.json")"
[ "$AGAIN" = "$WANT" ] || \
  fail "in a FRESH engine, load -> save lost the automation: got [$AGAIN]. Parsed at load
        and never installed means the next save deletes it, which is exactly how the mod
        links went."
echo "  reload (fresh engine): load -> save is faithful, so the load installed it"

# RIPPLE: lengthen the intro by 2 bars. The point at bar 5 moves; the three inside the
# intro do not.
# v29: the ripple is its own command now. "grow the intro from 4 bars to 6" is
# "insert 2 bars at bar 5" — the same edit, named for what it does.
cli do time insert --nanotick 15360000 --bars 2 >/dev/null 2>&1 || true
wait_for_event "$TMP/engine2.log" '"event":"time.edited"' 80 "the ripple" "$ENG"
cli do save autorip >/dev/null 2>&1 || true
wait_until 20 python3 -c "import json;json.load(open('$TMP/autorip.uniproj.json'))"
RIPPLED="$(points_of "$TMP/autorip.uniproj.json")"
WANT_RIP="0:0 ${Q}:0.125 $((2 * Q)):1 $((5 * BAR + 2 * BAR)):0.25"
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
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 14 >"$TMP/engine3.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/engine3.log" "$ENG" 80 "UI: command thread started"
cli() { DAW_UI_SHM_NAME="$SHM3" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load autoout >/dev/null 2>&1 || true
wait_for_event "$TMP/engine3.log" '"event":"project.load"' 80 "the project to load" "$ENG"
cli do add-track --force >/dev/null 2>&1 || true      # -> track 1
# STILL A SLEEP, and labelled rather than hidden: add-track and remove-track emit no
# event this check can wait on. Inventing one to make a test faster would be the test
# dictating the product's log.
sleep 0.6
cli do remove-track --track 1 --force >/dev/null 2>&1 || true
# STILL A SLEEP, and labelled rather than hidden: add-track and remove-track emit no
# event this check can wait on. Inventing one to make a test faster would be the test
# dictating the product's log.
sleep 0.8
cli do automation --force --track 1 --param index:0 --nanotick 0 --value 0.9 \
  >/dev/null 2>&1 || true
wait_for_event "$TMP/engine3.log" '"event":"automation.rejected"' 80 \
               "the refusal the assertion below greps for" "$ENG"
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

# ---- A REUSED SLOT MUST NOT INHERIT THE DEAD TRACK'S AUTOMATION.
#
# `automationClips` was cleared nowhere in the engine — only ever ASSIGNED, at load, for
# tracks the document names. The three paths that repurpose an existing runtime (AddTrack
# refilling a tombstone, the load blanking a slot past the new document, and a slot recycled
# as a multi-out stem) each cleared the chain, placements, owned clips and editable ids by
# hand, and each forgot automation and mod links. So: automate a filter on track 1, delete
# track 1, add a track — and the new track carries the deleted one's sweep, which the next
# save writes to disk as if the user had drawn it there.
#
# AddTrack refills the LOWEST tombstone, so removing track 1 and adding one lands back in
# slot 1. That is what makes this reproducible rather than incidental.
SHM4="/autochk4_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM4" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 18 >"$TMP/engine4.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/engine4.log" "$ENG" 80 "UI: command thread started"
cli() { DAW_UI_SHM_NAME="$SHM4" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load auto >/dev/null 2>&1 || true
wait_for_event "$TMP/engine4.log" '"event":"project.load"' 80 "the project to load" "$ENG"
cli do add-track --force >/dev/null 2>&1 || true          # -> track 1
# STILL A SLEEP, and labelled rather than hidden: add-track and remove-track emit no
# event this check can wait on. Inventing one to make a test faster would be the test
# dictating the product's log.
sleep 0.7
cli do automation --force --track 1 --param index:0 --nanotick 0 --value 0.75 \
  >/dev/null 2>&1 || true
wait_for_event "$TMP/engine4.log" '"event":"automation.point"' 80 \
               "the write to the added track" "$ENG"
# It must have LANDED, or this proves nothing about clearing it afterwards.
grep '"event":"automation.point"' "$TMP/engine4.log" | grep -q '"track":1' || \
  fail "the setup failed: automation was never written to the added track, so the reuse
        assertion below would pass for the wrong reason"
cli do remove-track --track 1 --force >/dev/null 2>&1 || true
# STILL A SLEEP, and labelled rather than hidden: add-track and remove-track emit no
# event this check can wait on. Inventing one to make a test faster would be the test
# dictating the product's log.
sleep 0.7
cli do add-track --force >/dev/null 2>&1 || true          # refills slot 1
# STILL A SLEEP, and labelled rather than hidden: add-track and remove-track emit no
# event this check can wait on. Inventing one to make a test faster would be the test
# dictating the product's log.
sleep 0.9
cli do save autoreuse >/dev/null 2>&1 || true
wait_until 20 python3 -c "import json;json.load(open('$TMP/autoreuse.uniproj.json'))"
kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

REUSED="$(python3 - "$TMP/autoreuse.uniproj.json" <<'PYR'
import json, sys
doc = json.load(open(sys.argv[1]))
for t in doc.get("tracks", []):
    if t.get("track_id") == 1 and not t.get("is_master"):
        print(len(t.get("automation", [])))
        break
else:
    print("no-track-1")
PYR
)"
[ "$REUSED" = "0" ] || \
  fail "the re-added track 1 carries $REUSED automation clip(s) from the track that was
        removed — a sweep the user deleted is back on a new lane and now saved to disk"
echo "  slot reuse: a re-added track does not inherit the removed track's automation"

echo "automation_check: PASS"
