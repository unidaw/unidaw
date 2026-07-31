#!/usr/bin/env bash
# AN AGENT SHOULD NOT WRITE INTO YOUR CLIP.
#
# An agent that edits the document directly leaves you undoing its work: its notes are interleaved
# with yours in one undo stack, there is no way to hear the two versions side by side, and "I liked
# the second bar of that" is not expressible at all. The frontend agent asked for a scratch target
# instead, and shipped writing direct-and-flagged in the meantime.
#
# THE MODEL ALREADY HAD THE PRIMITIVE. A clip is CONTENT and a placement is an APPEARANCE, so "the
# agent's version" is just another clip and comparing is retargeting the appearance:
#
#   fork   copy what this appearance plays, point it at the copy, keep the original as the ALTERNATE
#   swap   exchange the two — that IS the A/B
#   keep   drop the other once you have decided (keeping is doing nothing, which is the right
#          default for the case where the agent was useful)
#
# WHAT PLAYS IS ALWAYS THE PLACEMENT'S clipId. There is deliberately no "auditioning" flag: a second
# fact about which clip you are hearing is a second fact that can disagree with the first, and this
# codebase has spent most of its debugging time on exactly that shape.
#
# SIX PROPERTIES:
#   FORKS      only the named appearance retargets — the clip's other appearances keep playing the
#              original, which is the whole point of forking rather than editing. "Fix the bass in
#              chorus 1" must still reach all three choruses; a DRAFT of chorus 1 must not.
#   ISOLATES   a write after the fork lands in the draft and nowhere else
#   SWAPS      the A/B is instant and restores exactly
#   VISIBLE    the extent publishes has_alternate, so a UI can offer the toggle at all
#   UNDOABLE-  the fork and the swap DO NOT consume the undo stack. This is the property the whole
#   -NESS      feature exists for: undo must still undo your last musical edit, not the audition.
#   SURVIVES   the draft and the alternate link come back after a save and a reload
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/scratch_clip_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((Q * 4))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

# THREE appearances of ONE clip. Three is the minimum that can show the fork is scoped: with one
# placement, "retargeted the appearance" and "edited the clip" are indistinguishable.
python3 - "$TMP/sc.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
clip = {"id": 1, "name": "bass", "length": BAR, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 40, "velocity": 100,
                   "column": 0, "note_id": 1},
                  {"nanotick": Q, "duration": Q, "pitch": 43, "velocity": 100,
                   "column": 0, "note_id": 2}]}
tr = {"track_id": 0, "name": "B", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 1, "id": 2, "at": BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 1, "id": 3, "at": 2 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "sc"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/scchk_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load sc --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.5

# "placement:clip" per appearance, and whether it has an alternate — read through python so a
# grep that matches the wrong line cannot pass the check.
spread() {
  cli get extents 2>/dev/null | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("BADJSON"); raise SystemExit
print(" ".join("%d:%d%s" % (e["placement"], e["clip"], "*" if e["has_alternate"] else "")
               for e in sorted(d, key=lambda x: x["placement"])))
'
}
notes() {
  cli get notes --track 0 2>/dev/null | sed -n 's/.*"note_count": \([0-9]*\).*/\1/p' | head -1
}

BEFORE="$(spread)"
[ "$BEFORE" = "1:1 2:1 3:1" ] || \
  fail "the fixture should start with three appearances of clip 1, got [$BEFORE]"
N_BEFORE="$(notes)"
echo "  loaded: $BEFORE, $N_BEFORE notes on the track"

# ---- FORKS, and only the one named. The other two appearances must keep playing the original,
# because that is the difference between a draft and an edit.
cli do scratch fork --track 0 --placement 2 >/dev/null 2>&1 || true
sleep 1.3
grep '"event":"scratch.applied"' "$TMP/eng.log" | grep -q '"op":"fork_placement_clip"' || \
  fail "the fork was not applied: $(grep -o '"event":"scratch[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
FORKED="$(spread)"
case "$FORKED" in
  "1:1 2:"*"* 3:1") : ;;
  *) fail "after forking placement 2 the appearances read [$FORKED]. Expected 1 and 3 still on
        clip 1 and 2 on a NEW clip marked with an alternate. If all three moved, the fork edited
        the CLIP rather than retargeting one appearance — which would mean a draft of chorus 1
        silently rewrote choruses 2 and 3" ;;
esac
echo "  forks: only placement 2 moved to a draft, and it is flagged ($FORKED)"

# ---- VISIBLE. The A/B cannot be offered by a UI that cannot see there is another version.
case "$FORKED" in
  *"2:"*"*"*) : ;;
  *) fail "the forked appearance does not publish has_alternate, so nothing can offer the swap" ;;
esac
case "$FORKED" in
  "1:1 "*) : ;;
  *) fail "an unforked appearance is flagged as having an alternate" ;;
esac

# ---- ISOLATES. A write after the fork must land in the draft and nowhere else. Placement 2
# covers bar 2, so this tick is inside it.
cli do note --track 0 --nanotick $((BAR + Q / 2)) --pitch 55 --velocity 100 \
    --duration $((Q / 2)) >/dev/null 2>&1 || true
sleep 1.3
N_DRAFT="$(notes)"
[ "$N_DRAFT" -gt "$N_BEFORE" ] || \
  fail "the write after the fork did not land at all ($N_BEFORE -> $N_DRAFT)"
echo "  isolates: the agent's write landed in the draft ($N_BEFORE -> $N_DRAFT notes)"

# ---- SWAPS, and restores EXACTLY. This is the A/B.
cli do scratch swap --track 0 --placement 2 >/dev/null 2>&1 || true
sleep 1.3
SWAPPED="$(spread)"
N_YOURS="$(notes)"
[ "$N_YOURS" = "$N_BEFORE" ] || \
  fail "swapping back gave $N_YOURS notes, expected the original $N_BEFORE. The A/B has to be
        exact or it is not a comparison"
case "$SWAPPED" in
  "1:1 2:1* 3:1") : ;;
  *) fail "after the swap the appearances read [$SWAPPED], expected placement 2 back on clip 1
        with its draft still available as the alternate" ;;
esac
echo "  swaps: back to yours exactly, draft still one command away ($SWAPPED)"

# ---- AND THE UNDO STACK IS UNTOUCHED. THIS IS THE PROPERTY THE FEATURE EXISTS FOR: an audition
# is not an edit, so `undo` must still undo your last MUSICAL change, not the swap. Type a note
# BEFORE all this and it must be what undo removes.
cli do note --track 0 --nanotick $((Q / 2)) --pitch 60 --velocity 100 \
    --duration $((Q / 2)) >/dev/null 2>&1 || true
sleep 1.2
N_TYPED="$(notes)"
cli do scratch swap --track 0 --placement 2 >/dev/null 2>&1 || true
sleep 1.2
cli do scratch swap --track 0 --placement 2 >/dev/null 2>&1 || true
sleep 1.2
[ "$(notes)" = "$N_TYPED" ] || fail "two swaps did not return to where they started"
cli do undo >/dev/null 2>&1 || true
sleep 1.3
N_UNDONE="$(notes)"
[ "$N_UNDONE" = "$N_YOURS" ] || \
  fail "after typing a note, auditioning twice and pressing undo, the track has $N_UNDONE notes —
        expected $N_YOURS, i.e. the typed note removed. If undo instead reversed a SWAP, the
        audition consumed the undo stack and the agent's draft is standing between you and your
        own edit history, which is exactly what this feature exists to prevent"
echo "  undo: an audition is not an edit — undo still removed the typed note"

# ---- SURVIVES. A draft is work; it has to be there tomorrow.
cli do save scout --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
SAVED="$(python3 - "$TMP/scout.uniproj.json" <<'PYS'
import json, sys
d = json.load(open(sys.argv[1]))
tr = [t for t in d["tracks"] if not t.get("is_master")][0]
print(" ".join("%d:%d%s" % (p["id"], p["clip_id"],
                            "*" if p.get("alternate_clip_id") else "")
               for p in sorted(tr["placements"], key=lambda x: x["id"])))
print(len(d["clips"]))
PYS
)"
SAVED_SPREAD="$(echo "$SAVED" | head -1)"
SAVED_CLIPS="$(echo "$SAVED" | tail -1)"
[ "$SAVED_SPREAD" = "1:1 2:1* 3:1" ] || \
  fail "the saved file has [$SAVED_SPREAD], expected the alternate link on placement 2"
[ "$SAVED_CLIPS" = "2" ] || \
  fail "the saved file has $SAVED_CLIPS clip(s), expected 2 — the draft is work and has to be
        saved with the project, not thrown away on close"
echo "  survives: the draft and the alternate link are in the file ($SAVED_SPREAD, $SAVED_CLIPS clips)"

echo "scratch_clip_check: PASS — an agent gets its own copy, the A/B is one command, undo is yours"
