#!/usr/bin/env bash
# Checks NON-DESTRUCTIVE LANE QUANTIZE end to end (roadmap M1.13): turning quantize on
# changes what the engine PLAYS, and changes nothing about what it STORES.
#
# Both halves matter and neither alone is the feature. A quantize that only moved the
# audio would be untestable from the outside; one that also moved the notes would be the
# ordinary destructive quantize every tracker already has, and would throw away the
# performance on the first pass.
#
# The stimulus is arithmetic rather than musical: eight notes written deliberately OFF a
# 16th grid, then quantized hard onto it.
#
# The audible half is asserted through the engine's own count rather than through a
# capture. There is no honest audio assertion available here: the fixture instrument is
# silent by construction, and a real synth makes this a timing-flaky test of the audio
# device rather than of quantize. So the engine reports how many events its SCHEDULING
# copy moved — the very snapshot the producer plays from — and the check requires all
# eight. That number is 0 if quantize is stored but not wired, which is the failure this
# has to catch, and it cannot be faked by the notes having been on the grid already
# (the check also asserts they are not).
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/lane_quantize_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
SIXTEENTH=$((Q / 4))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
SHM="/lqchk_$$"
# THE ENGINE MUST DIE WHEN THIS CHECK DOES, including when ctest KILLS the check on a timeout.
# This trap used to remove $TMP and leave the engine running: it was only stopped on the normal
# path and inside fail(). A timed-out check therefore orphaned a possibly-hung engine, and ctest
# then blocked on it — measured at about 1000s per timeout across 18 runs, perfectly correlated
# with the timeout count. override showed it plainly: 909.87s against a TIMEOUT of 600, passing
# standalone in 23.2s.
#
# stop_engine escalates to SIGKILL after 10s and SAYS SO, so a hang stops being something to
# infer from a sample stack and becomes a line in the run.
cleanup() { [ -n "${ENG:-}" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT

# Eight notes, each pushed off the 16th grid by a different amount. Nothing lands on a
# grid line, so "the onsets are on a grid" cannot be true by accident.
OFFSETS=(37000 -41000 52000 -28000 61000 -19000 44000 -55000)
NOTES=""
for i in $(seq 0 7); do
  base=$((SIXTEENTH * (i + 1) * 2))
  t=$((base + ${OFFSETS[$i]}))
  [ -z "$NOTES" ] || NOTES="$NOTES,"
  NOTES="$NOTES
        { \"nanotick\": $t, \"duration\": 40000, \"pitch\": $((60 + i)),
          \"velocity\": 100, \"column\": 0, \"note_id\": $((100 + i)) }"
done

cat > "$TMP/lq.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "lq" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "c", "length": $((16 * Q)), "kind": "symbolic",
    "notes": [ $NOTES ] } ],
  "tracks": [
    { "track_id": 0, "name": "L",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "quantize": { "grid_nanoticks": 0, "strength_milli": 0, "swing_milli": 0 },
      "placements": [ { "clip_id": 1, "at": 0, "length": $((16 * Q)),
                        "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

# BOOTED WITH THE PROJECT, AND WAITED FOR. This check used to launch a bare engine, `sleep 2.5`,
# send `do load`, and `sleep 1.5` — two bets on a busy machine, and it failed roughly one run in
# three with "no published notes for track 0 — the fixture did not load". A true observation and
# the wrong conclusion: the fixture is fine, the read just arrives too early.
#
# The repo has had wait_for_boot for this since the boot-wait sweep (task #106) and the sweep
# missed this file — at the time it was one of the 43 checks nobody had registered in ctest, so
# nothing it did could be observed to be wrong. Registering it is what exposed this.
#
# BUT WAIT_FOR_BOOT ALONE DID NOT FIX IT — 4 failures in 12 runs with the sleeps replaced, which
# is the only reason the real cause was found rather than assumed. `project.load` firing means the
# engine has LOADED; it does not mean the notes region has been PUBLISHED, and `get notes` reads
# the region. The boot event is a proxy for what this check actually needs, and a proxy that is
# usually close enough is exactly the kind of wait that fails under load.
#
# So the assertion below POLLS FOR THE THING IT NEEDS instead. General rule, paid for twice here:
# wait on the condition you are about to assert on, not on an event that normally precedes it.
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project lq --run-seconds 26 >"$TMP/engine.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/engine.log" "$ENG" 120
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

fail() { echo "  FAIL: $*"; kill "$ENG" 2>/dev/null || true; wait "$ENG" 2>/dev/null || true; exit 1; }

# What is STORED is what a SAVE writes — not what the published region happens to be
# showing. That distinction is the whole test here: the published clip region is only
# rebuilt when the clip version moves, and quantize deliberately does not move it, so
# comparing published-before against published-after passes even for a quantize that
# rewrites every note. (It did: the first version of this check called a destructive
# quantize non-destructive.) Saving forces the engine to serialise the notes it holds.
# Parse the JSON properly rather than grepping "nanotick": the tempo map has one too,
# and the saved file puts each field on its own line, so a regex over the whole file
# either miscounts or matches across records.
ticks_of() {
  python3 - "$1" <<'PYX'
import json, sys
doc = json.load(open(sys.argv[1]))
ticks = []
for clip in doc.get("clips", []):
    for note in clip.get("notes", []):
        ticks.append(int(note["nanotick"]))
print(" ".join(str(t) for t in sorted(ticks)))
PYX
}
BEFORE="$(ticks_of "$TMP/lq.uniproj.json")"
COUNT="$(echo "$BEFORE" | wc -w | tr -d ' ')"
[ "$COUNT" = "8" ] || fail "the fixture should have 8 notes, parsed $COUNT"

# BOUNDED POLL, not a single read: the notes region is published shortly AFTER project.load, so
# a one-shot read here races the publish. Still fails if the fixture genuinely did not load —
# it just no longer fails when the engine was merely slow.
notes_ready() { cli get notes --track 0 2>/dev/null | grep -q '"nanotick"'; }
wait_until 20 notes_ready || \
  fail "no published notes for track 0 after 20s — the fixture did not load"
echo "  authored notes: $COUNT"

# Turn quantize on: hard onto the 16th grid, then save and read back what was stored.
cli do quantize --track 0 --grid "$SIXTEENTH" --strength 1000 >/dev/null 2>&1 || true
sleep 1
cli do save lqout >/dev/null 2>&1 || true
sleep 1.5
# Read the PUBLISHED deviations while the engine is still up.
DEVS="$(cli get notes --track 0 | sed -n 's/.*"dev": \(-\{0,1\}[0-9]*\).*/\1/p' | tr '\n' ' ')"

kill "$ENG" 2>/dev/null || true
wait "$ENG" 2>/dev/null || true

[ -f "$TMP/lqout.uniproj.json" ] || \
  fail "the engine did not write the save — nothing to compare"
AFTER="$(ticks_of "$TMP/lqout.uniproj.json")"

# PRECONDITION: the engine must have actually accepted the command. Without this, a
# no-op engine passes the "notes did not change" half trivially.
QLINE="$(grep '"event":"lane.quantize"' "$TMP/engine.log" | tail -1)"
[ -n "$QLINE" ] || fail "the engine never applied SetLaneQuantize — nothing was tested"

# HALF ONE, non-destructive: the SAVED notes are identical to the ones authored.
if [ "$BEFORE" != "$AFTER" ]; then
  echo "  authored: $BEFORE"
  echo "  saved   : $AFTER"
  fail "quantizing the lane CHANGED the stored notes — this is meant to be
        non-destructive, and a destructive quantize throws the performance away"
fi
echo "  saved notes identical to authored (non-destructive)"

# HALF TWO, audible: the SCHEDULING copy really moved. 0 here means quantize was stored
# and never reached the snapshot the producer plays from.
MOVED="$(echo "$QLINE" | sed -n 's/.*"moved":\([0-9]*\).*/\1/p')"
[ "${MOVED:-0}" = "8" ] || \
  fail "the scheduling copy moved ${MOVED:-0} of 8 notes — quantize is stored but is not
        reaching the snapshot the producer schedules from"
echo "  scheduling copy moved all 8 notes onto the grid (audible half)"

# And the authored ticks must NOT be on the grid, so "quantize moved them" is falsifiable
# rather than true of notes that were already there.
OFFGRID=0
for t in $AFTER; do
  if [ $((t % SIXTEENTH)) -ne 0 ]; then
    OFFGRID=$((OFFGRID + 1))
  fi
done
[ "$OFFGRID" -ge 8 ] || \
  fail "only $OFFGRID authored notes are off the grid; the fixture is supposed to place
        all 8 off it, so 'quantize moved them' would be unfalsifiable"
echo "  all $OFFGRID saved notes remain off the grid, as written"

# HALF THREE: the PUBLISHED deviation must be the same number the scheduler used. It is
# published (UiClipNote.devNanoticks) precisely so a client never has to re-implement
# quantizeTick — so if it disagreed with the scheduling copy, the drawn deviation bar
# would be an instrument that lies in the last digit, which is the digit it exists for.
#
# The engine is gone by now, so this re-derives from the SAVED authored ticks: at full
# strength every note sounds exactly on the grid, so authored + dev must be a multiple of
# the grid, for every note.
python3 - "$SIXTEENTH" <<PYD
import sys
grid = int(sys.argv[1])
authored = [int(x) for x in "$AFTER".split()]
devs = [int(x) for x in "$DEVS".split()]
if len(devs) != len(authored):
    print("  FAIL: %d published deviations for %d notes" % (len(devs), len(authored)))
    raise SystemExit(1)
bad = [(a, d) for a, d in zip(sorted(authored), devs) if (a + d) % grid != 0]
if bad:
    for a, d in bad[:4]:
        print("  FAIL: note at %d publishes dev %+d, which sounds at %d — not on the grid"
              % (a, d, a + d))
    raise SystemExit(1)
if all(d == 0 for d in devs):
    print("  FAIL: every published deviation is 0, so the field is not being written")
    raise SystemExit(1)
print("  published deviations put every note exactly on the grid: %s"
      % ", ".join("%+d" % d for d in devs))
PYD
[ $? = 0 ] || fail "the published deviation does not agree with the quantize the engine ran"

echo "lane_quantize_check: PASS"
