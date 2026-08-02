#!/usr/bin/env bash
# THE BIGGEST DESTRUCTIVE EDIT IN THE PROGRAM WAS THE ONE YOU COULD NOT TAKE BACK.
#
# InsertRemoveTime moves every placement on EVERY track, plus the tempo map, the harmony timeline,
# every automation clip, the meter map and every marker, in one transaction — and when it was
# SetSectionLength it pushed no undo entry at all.
# EngineUndoEntry carries at most two tracks (`hasSecond`, added for a cross-track placement move),
# so there was nothing it could have pushed. Nor was a successful ripple recorded in history.jsonl:
# only the REFUSALS were, which is the opposite of what a "what changed since Tuesday" artifact is
# for. The refusal messages say "empty those bars first", which is thin comfort when the mistake was
# pressing the wrong thing.
#
# SIX THINGS MOVE AND ALL SIX MUST COME BACK. Asserting on placements alone would pass a restore
# that put the notes back and left the filter sweep at its new position — and a PARTIAL restore of a
# ripple is worse than no undo at all, because the song looks recovered and is not. So this fixture
# is built so that every one of the five is in the path of the edit:
#
#   placements   two tracks, so a restore that handles one is caught
#   automation   a lane that straddles the boundary: one point before it, one after
#   tempo map    a point at the boundary (moves) and one before it (must not)
#   harmony      the same, so "carried everything" and "carried nothing" both fail
#   markers      one at the boundary and one before it
#   the METER    a 7/8 point at the boundary — the capability the spine could not author at all,
#                and the one whose restore a placements-only assertion would never notice
#
# THE MODEL, stated because it surprises people: undo here is a whole-store SWAP, not a per-edit
# inverse. It restores the song to a captured state, so an edit made after the ripple and undone by
# it goes with it. That is what a swap means, and it is the same model the per-track undo uses.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/ripple_undo_check.sh
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
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

python3 - "$TMP/ru.uniproj.json" "$Q" <<'PY'
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
# The edit point is 4*BAR (bar 5). Everything at or after that moves by the delta;
# everything before it must not. Both sides are populated deliberately — a check with only
# later-side entries would pass an engine that moved everything, which is equally wrong.
def track(tid, at_before, at_after):
    auto = [{"param_id": "cutoff", "target_plugin_index": 4294967295, "discrete": False,
             "points": [{"nanotick": 2 * BAR, "value": 0.2},
                        {"nanotick": 6 * BAR, "value": 0.8}]}] if tid == 0 else []
    return {"track_id": tid, "name": "T%d" % tid, "harmony_quantize": False,
            "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": routing(), "device_chain": [], "mod_links": [],
            "automation": auto,
            "placements": [
                {"clip_id": 1, "id": tid * 10 + 1, "at": at_before, "length": BAR,
                 "notes": [], "chords": [], "mutes": []},
                {"clip_id": 1, "id": tid * 10 + 2, "at": at_after, "length": BAR,
                 "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "ru"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0},
                         {"nanotick": 1 * BAR, "bpm": 90.0},     # before: must not move
                         {"nanotick": 4 * BAR, "bpm": 140.0}],   # at the boundary: must move
           "harmony_timeline": [{"nanotick": 1 * BAR, "root": 0, "scale_id": 0},
                                {"nanotick": 4 * BAR, "root": 7, "scale_id": 0}],
           "markers": [{"id": 1, "nanotick": 0, "name": "intro", "color_rgb": 0},
                       {"id": 2, "nanotick": 4 * BAR, "name": "verse", "color_rgb": 0}],
           "time_sig_map": [{"nanotick": 0, "numerator": 4, "denominator": 4},
                            {"nanotick": 4 * BAR, "numerator": 7, "denominator": 8}],
           "clips": [clip],
           # Track 0 has material either side of the boundary; track 1 too, at different
           # ticks, so a restore that only handles one track is caught.
           "tracks": [track(0, 0, 5 * BAR), track(1, 2 * BAR, 6 * BAR)]},
          open(out, "w"))
PY

SHM="/ruchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load ru --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.5

# ALL FIVE in one string, read from a SAVE — the durable answer, and the only view that carries
# the tempo map and the harmony timeline. `get automation-points` covers the runtime separately
# below, because the file and the RT snapshot are the two views that can disagree.
state() {  # state <name> -> "PL[...] AUTO[...] TEMPO[...] HARM[...] SEC[...]"
  cli do save "$1" --force >/dev/null 2>&1 || true
  sleep 1.6
  python3 - "$TMP/$1.uniproj.json" <<'PYS'
import json, sys
doc = json.load(open(sys.argv[1]))
pl, au = [], []
for t in doc.get("tracks", []):
    if t.get("is_master") or t.get("is_aux_child"):
        continue
    for p in t.get("placements", []):
        pl.append("%d@%d" % (p["id"], p.get("at", -1)))
    for a in t.get("automation", []):
        au.append("%s:%s" % (a["param_id"],
                             ",".join(str(q["nanotick"]) for q in a["points"])))
tempo = ",".join("%d@%g" % (p["nanotick"], p["bpm"]) for p in doc.get("tempo_map", []))
harm = ",".join("%d@%d" % (e["nanotick"], e["root"]) for e in doc.get("harmony_timeline", []))
sec = ",".join("%d@%d" % (m["id"], m["nanotick"]) for m in doc.get("markers", []))
sec += " METER[" + ",".join("%d:%d/%d" % (p["nanotick"], p["numerator"], p["denominator"])
                            for p in doc.get("time_sig_map", [])) + "]"
print("PL[%s] AUTO[%s] TEMPO[%s] HARM[%s] MARK[%s]"
      % (" ".join(sorted(pl)), " ".join(sorted(au)), tempo, harm, sec))
PYS
}
runtime_points() {
  cli get automation-points --track 0 --param cutoff 2>/dev/null | python3 -c '
import json, sys
try:
    a = json.load(sys.stdin)
except Exception:
    print("NOANSWER"); raise SystemExit
print("NOTFOUND" if not a.get("found") else
      ",".join(str(p["nanotick"]) for p in a["points"]))
'
}

BEFORE="$(state before)"
RT_BEFORE="$(runtime_points)"
case "$BEFORE" in
  *"TEMPO[0@120,3840000@90,15360000@140]"*) : ;;
  *) fail "the fixture did not load as written — got $BEFORE" ;;
esac
echo "  loaded: $BEFORE"

# ---- RIPPLE. Insert 4 bars at bar 5: everything at or after it moves by 4 bars.
cli do time insert --nanotick 15360000 --bars 4 >/dev/null 2>&1 || true
sleep 1.5
grep -q '"event":"time.edited"' "$TMP/eng.log" || \
  fail "the ripple was not applied: $(grep -o '"event":"time[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
grep '"event":"time.edited"' "$TMP/eng.log" | tail -1 | grep -q '"undoable":true' || \
  fail "the ripple did not report itself as undoable"
# JOURNALLED. Only refusals were recorded before, so history.jsonl held every ripple that did
# nothing and none that did something.
if [ -f "$TMP/history.jsonl" ]; then
  grep '"op":"insert_remove_time"' "$TMP/history.jsonl" | grep -q '"outcome":"received"' || \
    fail "the applied ripple is not in history.jsonl — only its refusals ever were"
  echo "  journalled: the applied ripple is in history.jsonl, not just its refusals"
fi
AFTER="$(state after)"
[ "$AFTER" != "$BEFORE" ] || fail "the ripple changed nothing, so the undo proves nothing"
echo "  rippled: $AFTER"

# ---- AND THE SONG IS STILL ON ITS OWN BAR GRID. The insert point is where a 7/8 change begins,
# and that point MOVES with the edit — so the bars being inserted are in the PRECEDING meter, not
# the one that used to start there. Taking the meter AT the point instead moved everything by
# 4 * 3.5 quarters while the inserted span was still 4/4, landing the 7/8 change at 7.5 bars;
# TimeSignatureMap then snapped it forward to bar 8, silently parting it from the marker that had
# moved with it. Measured, not theorised.
GRID="$(python3 - "$TMP/after.uniproj.json" "$BAR" <<'PYG'
import json, sys
doc = json.load(open(sys.argv[1])); BAR = int(sys.argv[2])
pts = doc.get("time_sig_map", [])
late = [p for p in pts if p["nanotick"] > 0]
print("NOPOINT" if not late else ("ON" if late[0]["nanotick"] % BAR == 0 else
      "OFF:%d" % late[0]["nanotick"]))
PYG
)"
[ "$GRID" = "ON" ] ||   fail "after inserting 4 bars, the meter change is not on a bar line ($GRID). The inserted span
        must be whole bars of the meter PRECEDING the edit point — the meter at the point is the
        one that moves away"
echo "  on the grid: the 7/8 change still lands on a bar line after the insert"

# ---- UNDO. Every one of the five back.
cli do undo >/dev/null 2>&1 || true
sleep 1.6
grep -q '"event":"undo.song"' "$TMP/eng.log" || \
  fail "the undo did not restore a SONG-scoped entry, so the ripple pushed none — this is the
        defect: the largest destructive edit in the program with no way back:
        $(grep -o '"event":"undo[a-z._]*"[^}]*' "$TMP/eng.log" | tail -1)"
UNDONE="$(state undone)"
[ "$UNDONE" = "$BEFORE" ] || \
  fail "undo did not restore the song.
        expected $BEFORE
        got      $UNDONE
        A PARTIAL restore is worse than none: the song looks recovered and is not. Compare the
        five groups — PL (both tracks), AUTO, TEMPO, HARM, SEC — to see which one was left behind"
echo "  undone: all six restored (placements, automation, tempo, harmony, markers, meter)"

# ---- AND THE RUNTIME AGREES, not just the file. The RT scheduler reads automation from the track
# SNAPSHOT, so a restore that puts the points back in the model and not in the snapshot is right on
# disk and wrong in your ears — the exact divergence the ripple itself had to fix.
RT_UNDONE="$(runtime_points)"
[ "$RT_UNDONE" = "$RT_BEFORE" ] || \
  fail "the FILE was restored but the runtime was not: read-back had [$RT_BEFORE], now [$RT_UNDONE].
        A restored point that is not republished into the track snapshot is a point that does not
        play"
echo "  runtime agrees: the published read-back matches too ($RT_UNDONE)"

# ---- REDO puts it back, or the entry was consumed rather than moved.
cli do redo >/dev/null 2>&1 || true
sleep 1.6
grep -q '"event":"redo.song"' "$TMP/eng.log" || fail "redo did not re-apply the song entry"
REDONE="$(state redone)"
[ "$REDONE" = "$AFTER" ] || \
  fail "redo did not re-apply the ripple.
        expected $AFTER
        got      $REDONE"
echo "  redone: the ripple is re-applied exactly"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "ripple_undo_check: PASS — a time edit is one undoable, journalled transaction"
