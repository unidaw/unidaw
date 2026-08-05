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
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/ovrchk_$$"
# THE ENGINE GOES WITH THE CHECK, including when ctest KILLS it on a timeout. This trap removed
# $TMP and left the engine running, so a timed-out check orphaned it and ctest then blocked on the
# orphan — ~1000s per timeout, measured across 18 runs. override was the demonstrated case: 909.87s
# against a TIMEOUT of 600 while passing standalone in 23.2s.
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

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 30 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/engine.log" "$ENG" 80 "UI: command thread started"
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
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 2
wait_for_published 20 "3" count_pitch 0 38
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
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 3
wait_for_published 20 "1" count_pitch 1 46
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
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 4
wait_for_published 20 "3" count_pitch 0 40
FIX2="$(count_pitch 0 40)"
STILL="$(count_pitch 1 46)"
[ "$FIX2" = "3" ] || fail "the second clip-scope fix reached $FIX2 choruses, not 3"
[ "$STILL" = "1" ] || fail "the local hat did not survive a later clip edit (now $STILL)"
echo "  both: a second clip fix still reaches all 3, and the local hat survives it"

# ---- A LOCAL DELETE mutes a base note in ONE appearance only.
cli do delete-note --track 0 --local --nanotick $((8 * BAR)) --pitch 36 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 5
wait_for_published 20 "2" count_pitch 0 36
AFTER_MUTE="$(count_pitch 0 36)"
[ "$AFTER_MUTE" = "2" ] || \
  fail "muting the bass in chorus 2 should leave 2 of 3 sounding, got $AFTER_MUTE — a
        local delete must not reach the clip"
echo "  local delete: the bass is silenced in chorus 2 only (2 of 3 remain)"

# ---- A LOCAL ADD WITH NO EXPLICIT LENGTH MUST STILL SOUND, and an OFF must be refused.
#
# The scheduler skips a zero-duration event outright, so a note stored with length 0 can never
# sound — while still being saved and counted in the override badge. Clip scope already handled
# this by computing a span to at least the end of the bar; local scope stored the 0. daw-cli
# defaults --duration to 0, so `do note --local --pitch N` was exactly that gesture: accepted,
# reported applied, permanently silent.
cli do note --track 1 --local --nanotick $((17 * BAR + 1 * Q)) --pitch 51 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 6
wait_for_published 20 "1" count_pitch 1 51
NODUR="$(count_pitch 1 51)"
[ "$NODUR" = "1" ] || fail "the local add of pitch 51 did not land at all (count $NODUR)"
# ASSERT THE DURATION, not the existence. A zero-length note is still published, so counting it
# passes against the broken engine — the first version of this check did exactly that and the
# negative control is what caught it. The scheduler skips a zero-duration event, so a non-zero
# length is the property that means "this will sound".
NODUR_LEN="$(cli get notes --track 1 2>/dev/null | tr '{' '\n' | grep '"pitch": 51,' \
  | sed -n 's/.*"duration": \([0-9]*\).*/\1/p' | head -1)"
[ "${NODUR_LEN:-0}" -gt 0 ] || \
  fail "a local add with no --duration was stored with length ${NODUR_LEN:-?} — the scheduler
        skips a zero-duration event, so it is saved and badge-counted and permanently silent.
        Clip scope computes a span for the same gesture; local scope stored the 0."
echo "  no length: a local add without --duration gets a real length (${NODUR_LEN}) and sounds"

# Velocity 0 AND length 0 is the tracker OFF gesture — it ends the note sounding in a column
# and stores no event. As an additive override that is meaningless, and routing it here stored a
# phantom note-shaped nothing. It must be refused with a reason, not accepted.
cli do note --track 1 --local --nanotick $((17 * BAR + 3 * Q)) --pitch 53 --velocity 0 \
  >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 7
wait_for_published 20 "0" count_pitch 1 53
[ "$(count_pitch 1 53)" = "0" ] || \
  fail "an OFF gesture in local scope stored a phantom event ($(count_pitch 1 53) of pitch 53)"
grep -q '"reason":"note_off_needs_clip_scope"' "$TMP/engine.log" || \
  fail "the OFF gesture in local scope was dropped without saying why — a command accepted and
        silently absent is the shape this whole feature keeps failing in"
echo "  off gesture: refused in local scope, with a reason"

# ---- A LOCAL DELETE ON A LOOP REPEAT. The hat clip is 1 bar across 4, so bars 2-4 of the
# appearance are ITERATIONS of the same clip note — and the base notes only exist once, at
# offsets inside the clip. The lookup compared the PLACEMENT-relative tick against those
# offsets, so a delete anywhere past the first bar matched nothing, muted nothing, and returned
# without a word. Everything above missed it because the only local DELETE it does is on the
# bass, whose clip fills its placement exactly and therefore never loops.
#
# Deleting the hat in the THIRD iteration mutes that clip note in this appearance — all four
# iterations, which is what an additive-only override can express (the override belongs to the
# appearance; within it the note recurs). The point of the assertion is that it does SOMETHING:
# 4 was the broken answer.
cli do delete-note --track 1 --local --nanotick $((18 * BAR)) --pitch 42 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 8
wait_for_published 20 "0" count_pitch 1 42
LOOPDEL="$(count_pitch 1 42)"
[ "$LOOPDEL" = "0" ] || \
  fail "a local delete in the third iteration of a looping clip left $LOOPDEL hat(s) — 4 means
        the base-note lookup used the placement-relative tick against clip-relative offsets,
        matched nothing, and silently did nothing"
grep '"event":"local_edit.noop"' "$TMP/engine.log" | grep -q '"pitch":42' && \
  fail "the local delete of pitch 42 reported itself as a no-op, so it found no note to mute" \
  || true
echo "  loop delete: a local delete inside a loop repeat mutes the clip note in this appearance"

# Put it back so the revert assertions below still measure what they were written to measure.
cli do revert-overrides --track 1 --placement 21 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 9
wait_for_published 20 "4" count_pitch 1 42
[ "$(count_pitch 1 42)" = "4" ] || fail "the revert did not restore the base hats"
cli do note --track 1 --local --nanotick $((18 * BAR + 2 * Q)) --pitch 46 \
  --duration 60000 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 10
wait_for_published 20 "1" count_pitch 1 46
[ "$(count_pitch 1 46)" = "1" ] || fail "could not re-add the local hat for the revert test"

# ---- ONE-CLICK REVERT clears the overrides on one appearance and leaves the clip alone.
cli do revert-overrides --track 1 --placement 21 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 11
wait_for_published 20 "0" count_pitch 1 46
REVERTED="$(count_pitch 1 46)"
BASE_LEFT="$(count_pitch 1 42)"
[ "$REVERTED" = "0" ] || fail "revert left $REVERTED added hats"
[ "$BASE_LEFT" = "4" ] || \
  fail "revert removed base notes too ($BASE_LEFT of 4 left) — it must clear only the
        overrides"
echo "  revert: the added hat is gone, the clip's own 4 hats are untouched"

# ---- THE PLACEMENT'S OWN EDIT SCOPE (owner's call): mark ONE appearance, then type NORMALLY.
#
# Everything above passes --local per edit. That works and is what daw-cli does, but it is not an
# interface: something has to decide, per keystroke, which scope was meant. The answer chosen is a
# per-placement toggle rather than a global mode, on failure asymmetry — forget the toggle and the
# note appears in all three choruses (loud, one undo away), whereas being in the wrong global mode
# makes "fix the bass in chorus 1" silently NOT propagate (quiet, and easy to miss for an hour).
#
# THE DISCRIMINATING ASSERTION is that the edit carries NO --local flag. If the placement's flag
# were ignored, the note would reach the clip and sound in all three choruses; if scope were being
# inferred from occupancy rather than from the flag, it would depend on whether the cell was
# empty. Only honouring the placement gives exactly one.
cli do placement-scope --track 0 --placement 13 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 12
local_flag() { { cli get extents 2>/dev/null | tr '{' '\n' | grep '"placement": 13,' \
  | sed -n 's/.*"local_edits": \([a-z]*\).*/\1/p' | head -1; } || true; }
wait_for_published 20 "true" local_flag
LOCALFLAG="$(local_flag)"
[ "$LOCALFLAG" = "true" ] || \
  fail "after marking placement 13 the published extent says local_edits=$LOCALFLAG — a toggle
        whose state cannot be read is one the interface has to guess at, and the extents rebuild
        on the clip version so the command must bump it"

# Chorus 3 is placement 13 (bar 17). Type WITHOUT --local.
cli do note --track 0 --nanotick $((17 * BAR)) --pitch 44 --duration 240000 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 13
wait_for_published 20 "1" count_pitch 0 44
SCOPED="$(count_pitch 0 44)"
[ "$SCOPED" = "1" ] || \
  fail "a note typed into a placement marked for local edits, with NO --local flag, sounded
        $SCOPED times — 3 means the placement's flag was ignored and the edit went to the clip,
        which is the whole feature failing"
echo "  placement scope: an edit with no --local flag stayed local because the placement says so"

# And clearing it puts the behaviour back — otherwise the toggle is one-way and the state is a
# trap rather than a control.
cli do placement-scope --track 0 --placement 13 --on 0 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 14
cli do note --track 0 --nanotick $((17 * BAR + 1 * Q)) --pitch 45 --duration 240000 \
  >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 15
wait_for_published 20 "3" count_pitch 0 45
UNSCOPED="$(count_pitch 0 45)"
[ "$UNSCOPED" = "3" ] || \
  fail "after clearing the placement's scope a normal edit reached $UNSCOPED choruses, not 3 —
        the toggle must be reversible or it is a trap, not a control"
echo "  and clearing it restores clip scope (the next edit reaches all 3)"

# ---- A LOCAL EDIT IS ON THE UNDO STACK.
#
# Undo here is a whole-store SWAP, not a per-edit inverse — and applyLocalNoteEdit pushed
# nothing. So the entry a Ctrl-Z popped was some EARLIER edit's, and restoring its store
# wholesale deleted the override as a side effect; redo restored that edit's after-state,
# which also predates the override, so it was unrecoverable. Nothing logged, nothing refused.
#
# The two assertions that separate a recorded local edit from an unrecorded one:
#   UNDO must take back THE LOCAL EDIT (and leave the older clip-scope fix alone). Without
#   the push, undo instead rolls back the clip-scope note on track 0 and the local note on
#   track 1 stays exactly where it was — the opposite of both expectations.
#   REDO must bring it back. Without the push there is no entry that carries it.
cli do note --track 1 --local --nanotick $((17 * BAR + 1 * Q)) --pitch 50 \
  --duration 60000 >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 16
wait_for_published 20 "1" count_pitch 1 50
[ "$(count_pitch 1 50)" = "1" ] || \
  fail "the setup failed: the local note was not added, so the undo assertions below would
        pass for the wrong reason"
BEFORE_UNDO_CLIP="$(count_pitch 0 40)"
[ "$BEFORE_UNDO_CLIP" = "3" ] || fail "expected the clip-scope fix on 3 choruses, got $BEFORE_UNDO_CLIP"

cli do undo >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 17
wait_for_published 20 "0" count_pitch 1 50
AFTER_UNDO="$(count_pitch 1 50)"
STILL_CLIP="$(count_pitch 0 40)"
[ "$AFTER_UNDO" = "0" ] || \
  fail "undo did not take back the local edit (pitch 50 count $AFTER_UNDO) — the local edit
        is not on the undo stack, so Ctrl-Z reached past it to an older edit"
[ "$STILL_CLIP" = "3" ] || \
  fail "undo took back the LOCAL edit and also the older clip-scope fix (pitch 40 now on
        $STILL_CLIP choruses, was 3) — one Ctrl-Z undid two edits"
# AND it must be THIS edit that was undone, not the revert just before it. Without the local
# edit on the stack, the entry Ctrl-Z pops is the revert's — which restores the store from
# before the revert, bringing the reverted hat back and leaving the pitch-50 note untouched.
# That satisfies both assertions above by accident, which is exactly how a control passes a
# broken engine, so the resurrected hat is what actually distinguishes the two.
UNDO_RESURRECTED="$(count_pitch 1 46)"
[ "$UNDO_RESURRECTED" = "0" ] || \
  fail "undo restored the REVERTED hat ($UNDO_RESURRECTED back) instead of taking back the
        local edit — Ctrl-Z reached past the local edit to the previous entry"
cli do redo >/dev/null 2>&1 || true
# BOTH WAITS, in this order, and the order is the point. The journal says the
# engine ACTED; the published poll says the UI can SEE it. Neither alone is
# enough: a poll for a value the system ALREADY HAS matches on its first read
# and waits for nothing — which is what a refusal assertion ("this must still
# be 0") always is, and it let the next command race.
wait_for_history "$TMP" 18
wait_for_published 20 "1" count_pitch 1 50
AFTER_REDO="$(count_pitch 1 50)"
[ "$AFTER_REDO" = "1" ] || \
  fail "redo did not restore the local edit (pitch 50 count $AFTER_REDO) — it was
        destroyed rather than undone, which is data loss on a single keystroke"
echo "  undo: a local edit is undone and redone on its own, without disturbing the clip fix"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

grep -q '"event":"overrides.reverted"' "$TMP/engine.log" || \
  fail "the revert did not report what it cleared"
grep -q '"event":"local_edit.applied"' "$TMP/engine.log" || \
  fail "no local edit was reported, so the --local flag may not have reached the engine"
echo "  the engine reported the local edits and the revert"

echo "override_check: PASS — both halves of the Movement 3 sentence hold at once"
