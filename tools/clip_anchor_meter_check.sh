#!/usr/bin/env bash
# A NEW CLIP LANDS ON THE BAR THE RULER DRAWS — including after the meter changes.
#
# Note entry anchors a new clip to "the bar containing the tick", and it computed that as
# `(tick / barLength) * barLength` with barLength hardcoded to one 4/4 bar. In one signature that
# is exactly right. Add a 7/8 bridge and the bar containing a tick is at a multiple of NOTHING, so
# no single length can express it: clips landed on a grid the ruler does not draw, drifting
# further from it with every bar past the change. Task #43, and the last of the four hardcoded-4/4
# sites — the other three were bar ENDS and were fixed in 4ec1720.
#
# WHY THIS FIXTURE HAS A METER CHANGE IN IT, and why that is not optional: with a constant meter
# the old computation and the new one AGREE ON EVERY TICK. A check without a signature change is
# not a weak check, it is a check that cannot fail. That is the trap this suite keeps finding, so
# it is stated here rather than left to be rediscovered.
#
# THE ARITHMETIC, laid out so the expected numbers are not magic:
#   4/4 from tick 0, 7/8 from tick 8Q (the start of bar 2)
#   bar 0  [0Q,    4Q)      4/4
#   bar 1  [4Q,    8Q)      4/4
#   bar 2  [8Q,    11.5Q)   7/8   — 7 eighths = 3.5 quarters
#   bar 3  [11.5Q, 15Q)     7/8
#   bar 4  [15Q,   18.5Q)   7/8
#   bar 5  [18.5Q, 22Q)     7/8
#   A note entered at 19Q is in BAR 5, which starts at 18.5Q.
#   The old computation gives (19Q / 4Q) * 4Q = 16Q — two and a half quarters off, and off by
#   more the further past the change you type. 16Q is not a bar start in this song at all.
#
# THE SECOND NOTE IS FAR OUT ON PURPOSE. Note entry stretches the previous clip when the gap is
# within one bar ("keep typing after the last note"), and the first draft of this fixture put the
# two notes close enough that the second took the STRETCH branch and never reached the anchor
# computation at all — it joined the control's clip and the check saw one placement where it
# expected two. A fixture that never reaches the code under test is the failure this suite keeps
# finding; the gap here is fourteen quarters, well past any stretch.
#
# THREE PROPERTIES:
#   CONTROL      a note before the meter change anchors where BOTH computations agree. If this
#                fails the fixture is broken rather than the anchor, and it also proves the note
#                reached the engine at all
#   RULER        a note after the change anchors to the 7/8 bar start (18.5Q), not to the 4/4
#                multiple (16Q, which is not a bar start anywhere in this song). This is the whole
#                check, and the two answers differ by design
#   REBASED      the note's offset INSIDE its new clip is measured from that anchor. The anchor
#                and the offset are one decision — an anchor moved without the offset following
#                puts the note in the wrong place in the bar, which sounds wrong rather than
#                merely drawing wrong
#
#   tools/clip_anchor_meter_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# An EMPTY track. Every placement this check looks at is created by note entry, because creating
# it is the behaviour under test — a fixture with authored placements would take the
# InsidePlacement branch and never reach the anchor computation at all.
python3 - "$TMP/m.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "M", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "m"},
           "nanoticks_per_quarter": Q,
           "timebase": {"time_sig_numerator": 4, "time_sig_denominator": 4},
           # 4/4 from 0, 7/8 from the top of bar 2 (tick 8Q).
           "time_sig_map": [{"nanotick": 0, "numerator": 4, "denominator": 4},
                            {"nanotick": 8 * Q, "numerator": 7, "denominator": 8}],
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

SHM="/clipanchor_$$"
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project m --run-seconds 30 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
mcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }

# WAITS FOR THE EDIT TO LAND, rather than sleeping and hoping. daw-cli sends a note with the
# clip version it read a moment earlier, so a second note issued before the first has been APPLIED
# carries a stale base and is REFUSED — silently, from the caller's point of view. A fixed sleep
# is long enough on an idle machine and not under a parallel ctest, which is exactly how this
# check failed once in a full run and passed alone. Task #106's lesson one level up: wait for the
# state, not for a duration.
track_clip_version() {
  mcli get tracks 2>/dev/null | python3 -c "
import json, sys
try: print(json.load(sys.stdin)['tracks'][0].get('clip_version', 0))
except Exception: print(-1)"
}

send_note() {  # send_note <tick> <pitch> <what>
  local before after i
  before="$(track_clip_version)"
  mcli do note --track 0 --nanotick "$1" --pitch "$2" >/dev/null 2>&1
  for i in $(seq 1 80); do
    after="$(track_clip_version)"
    if [ "$after" != "$before" ] && [ "$after" != "-1" ]; then
      return 0
    fi
    kill -0 "$ENG" 2>/dev/null || \
      fail "the engine exited while waiting for $3 to land — see $TMP/eng.log"
    sleep 0.25
  done
  fail "$3 never landed: track 0's clip_version stayed at $before for 20s. A note sent with a
        stale base version is refused without any sign at the caller, so this is what that looks
        like from outside — check $TMP/eng.log for a rejection"
}

# ---- CONTROL: a note in bar 1, where 4/4 still rules and both computations say 4Q.
send_note "$((5 * Q))" 60 "the control note at 5Q"
# ---- RULER: a note in bar 5, which starts at 18.5Q under 7/8. The old form says 16Q, which is
# not a bar start anywhere in this song.
send_note "$((19 * Q))" 64 "the note at 19Q"

mcli do save out >/dev/null 2>&1
# The save is a file appearing, so wait for the engine to SAY it wrote one.
for _ in $(seq 1 60); do
  grep -q '"event":"project.save"' "$TMP/eng.log" 2>/dev/null && break
  kill -0 "$ENG" 2>/dev/null || break
  sleep 0.25
done
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/out.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"

python3 - "$TMP/out.uniproj.json" "$Q" <<'PYC' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
Q = int(sys.argv[2])
tr = d["tracks"][0]
pls = sorted(tr["placements"], key=lambda p: p["at"])
print("  placements: %r" % [(p["at"], p["at"] / Q) for p in pls])
clips = {c["id"]: c for c in d["clips"]}

if len(pls) != 2:
    print("  FAIL: expected TWO placements, one per note — got %d. The notes are fourteen"
          " quarters apart, far past the one-bar stretch threshold, so they must not have joined"
          " one clip. One placement means the second note took the STRETCH branch and the anchor"
          " computation was never reached — the fixture is not testing what it says."
          % len(pls))
    raise SystemExit(1)

# ---- CONTROL. Bar 1 under 4/4: both the old and new computations say 4Q, so this only proves
# the note landed and the fixture works.
if pls[0]["at"] != 4 * Q:
    print("  FAIL: the note at 5Q is in bar 1, which starts at 4Q in ANY reading of this meter,"
          " and its clip anchored at %d (%.3f quarters). Nothing below is meaningful — the"
          " fixture or the entry path is broken rather than the meter arithmetic."
          % (pls[0]["at"], pls[0]["at"] / Q))
    raise SystemExit(1)

# ---- RULER. The whole check.
want = 37 * Q // 2          # 18.5Q — the start of bar 5 under 7/8
naive = 16 * Q              # what (tick / 4Q) * 4Q gives for a note at 19Q
got = pls[1]["at"]
if got == naive:
    print("  FAIL: the note at 19Q anchored its clip at %d (%.1f quarters), which is the 4/4"
          " multiple and is not a bar start anywhere in this song. Under this meter bar 5 starts"
          " at %d (18.5 quarters): the anchor is still (tick / barLength) * barLength with a"
          " hardcoded 4/4 bar, so a new clip lands two and a half quarters off the ruler here and"
          " further off with every bar past the change."
          % (got, got / Q, want))
    raise SystemExit(1)
if got != want:
    print("  FAIL: the note at 19Q anchored its clip at %d (%.3f quarters). Bar 5 starts at %d"
          " (18.5 quarters) under 7/8, and the old 4/4 answer would have been %d — this is"
          " neither, so the bar grid is answering something else again."
          % (got, got / Q, want, naive))
    raise SystemExit(1)
print("  the note at 19Q anchored at %d = 18.5 quarters, the start of bar 5 under 7/8" % got)

# ---- REBASED. The anchor and the offset are one decision.
cl = clips.get(pls[1]["clip_id"])
if cl is None:
    print("  FAIL: the second placement names clip %r, which is not in the file"
          % pls[1]["clip_id"])
    raise SystemExit(1)
notes = cl.get("notes", [])
if len(notes) != 1:
    print("  FAIL: the new clip holds %d notes, expected 1" % len(notes))
    raise SystemExit(1)
rel = notes[0]["nanotick"]
if rel != Q // 2:
    print("  FAIL: the note sits at %d inside its clip; it was entered at 19Q and its clip is"
          " anchored at 18.5Q, so it belongs half a quarter in (%d). The old 4/4 anchor of 16Q"
          " would put it three quarters in. An anchor that moved without the offset following"
          " puts the note at the wrong place in the bar, which SOUNDS wrong rather than merely"
          " drawing wrong." % (rel, Q // 2))
    raise SystemExit(1)
print("  and the note sits %d into it — half a quarter, as entered" % rel)
PYC

echo "clip_anchor_meter_check: PASS — a new clip anchors to the bar the ruler draws on both sides"
echo "                         of a 4/4 -> 7/8 change, and the note is rebased to that anchor"
