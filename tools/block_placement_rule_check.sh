#!/usr/bin/env bash
# EVERY EVENT PLACED IN A BLOCK GOES THROUGH placeInBlock. No site converts a tick to a sample
# and hands it straight to an event.
#
# WHY THIS IS A TEXT CHECK AND NOT A BEHAVIOUR ONE. The RULE is already pinned by behaviour:
# tools/block_edge_note_check.sh proves a note-on on a boundary tick sounds where it is written,
# tools/note_off_edge_check.sh proves a note-off on one releases the note, and placeInBlock's own
# unit tests pin the floor/round/clamp arithmetic. What none of them can see is a NEW SITE that
# does the conversion by hand — and that is exactly how this defect kept coming back:
#
#   placeInBlock was written for the note-ON path, and SEVEN sites computed the same thing.
#   Three note-OFF sites kept the old arithmetic for a day (stuck notes, fixed 3a20da6).
#   The chord column-cut kept it for longer still, and its comment said "four sites" when it
#   was seven — a count from memory, which is how a rule keeps a divergent twin.
#
# WHAT THE BAD SHAPE COSTS, so the next person does not reintroduce it thinking it is cosmetic:
# tickDeltaToSamples ROUNDS, so a tick INSIDE this block can land at exactly blockSize. The host
# windows a block on [blockStart, blockEnd) and BREAKS out of its collect loop at the first entry
# at or past blockEnd — so one such entry stops every event queued behind it from being collected,
# and on the next block those are < blockStart and get popped and discarded. An aux child's MIDI
# is appended to the parent's ring after the parent's own, so it is the child that loses a whole
# block of notes.
#
# THE ALLOWED SHAPES are placeInBlock (which returns nullopt for a genuinely-later event and
# clamps the position of an in-window one) and the caller's own bounded arithmetic where the
# window test already guarantees the range — those are listed below by file and line reason
# rather than pattern-matched, so adding one is a decision rather than an accident.
#
#   tools/block_placement_rule_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ok=1

# The producer-path sources that place events into a block.
SOURCES="$ROOT/apps/engine_emit_notes.cpp $ROOT/apps/engine_resolve_events.cpp \
         $ROOT/apps/engine_render_track.cpp $ROOT/apps/engine_produce_block.cpp \
         $ROOT/apps/engine_run_patcher_node.cpp"
for f in $SOURCES; do
  [ -f "$f" ] || { echo "block_placement_rule_check: FAIL — $f is missing; a check that cannot"; \
                   echo "        find its subject is not a passing check."; exit 1; }
done

# THE FORBIDDEN SHAPE: a sample built by adding a converted tick delta to the block start, which
# is placeInBlock's body written out by hand and without its floor/clamp.
BAD="$(grep -nE 'blockSampleStart \+ tickDeltaToSamples\(' $SOURCES || true)"
if [ -n "$BAD" ]; then
  echo "block_placement_rule_check: FAIL — $(printf '%s\n' "$BAD" | wc -l | tr -d ' ') site(s) convert a"
  echo "        tick to a sample by hand instead of calling placeInBlock:"
  printf '%s\n' "$BAD" | sed "s|$ROOT/||" | sed 's/^/          /'
  echo
  echo "        tickDeltaToSamples ROUNDS, so a tick inside this block can land at exactly"
  echo "        blockSize. The host breaks its collect loop at the first entry >= blockEnd and"
  echo "        DISCARDS everything queued behind it on the next block — for a track with an aux"
  echo "        child that is a whole block of the child's notes, silently."
  echo
  echo "        Use daw::engine::placeInBlock(tickDelta, blockSampleStart, samplesPerTick,"
  echo "        engineConfig.blockSize) and act on nullopt at the call site, as the other sites do."
  ok=0
fi

# BLINDNESS FLOOR. If placeInBlock stops being called at all, the grep above passes by finding
# nothing — the same failure shape as the scope bug in ui_diff_accounting_check. The rule is only
# meaningful while the good shape is actually in use.
USES="$(grep -c 'placeInBlock(' $SOURCES | awk -F: '{s+=$2} END {print s+0}')"
if [ "${USES:-0}" -lt 5 ]; then
  echo "block_placement_rule_check: FAIL — only ${USES:-0} call(s) to placeInBlock across the"
  echo "        producer path, and there should be several. Either the placements moved to"
  echo "        another file or the helper was renamed; repoint this check rather than leaving"
  echo "        it green over a rule it can no longer see."
  ok=0
fi

[ "$ok" = "1" ] || exit 1
echo "block_placement_rule_check: PASS — $USES placement(s) go through placeInBlock and none" \
     "converts a tick to a sample by hand"
