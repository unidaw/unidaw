#!/usr/bin/env bash
# A ROW OP THE ENGINE PUBLISHES BUT NOTHING CAN WRITE IS HALF A FEATURE.
#
# retrigger, probability, the sound address, the sample offset and the onset delay have been on
# UiClipNote since v23 and v32. Every one was readable and none was writable: the editor could
# draw an ops cell it had no way to commit, which is the gap the web-UI agent hit head-on while
# building pointer editing. SetRowOps (opcode 81) is the missing half.
#
# FOUR PROPERTIES, and two of them were found by someone else reading the wire:
#
#   WRITES     each op set through the real command comes back on the note that was addressed
#   MASKS      setting ONE op leaves the others alone, and --clear removes one without
#              disturbing the rest. A command that always writes all five cannot express
#              "remove the retrigger from this note" — it can only restate the other four, and
#              restating is how two facts about one note start disagreeing
#   AUTHORED   a note an AGENT wrote is addressable, and writing to it does not touch the
#              HUMAN note with the same counter. EventId packs the author into bits 48+ and each
#              author counts independently, so those two ids differ only in their top 16 bits —
#              a command carrying 32 of them edits whichever note the counter happens to match,
#              silently. Found by the web-UI agent reading the payload against event_id.h
#   PERSISTS   the ops survive a SAVE and a RELOAD. A row op that survives the wire but not the
#              file is the failure mode you find a week later in a project that used to work
#
# Read back from the PROJECT FILE rather than from the model that wrote it, because the file is
# the artefact that has to be right — a check that asks the engine what it thinks it stored will
# agree with itself whatever the serializer did.
#
# Needs a real audio device (non-test mode) + daw_engine and daw-cli built.
#   tools/row_ops_write_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# Four notes with KNOWN ids, so every assertion addresses a specific one and a command that wrote
# to the wrong note shows up as the wrong note changing rather than as nothing happening.
python3 - "$TMP/rowops.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
notes = [{"nanotick": i * Q, "duration": Q // 2, "pitch": 60 + i, "velocity": 100,
          "column": 0, "note_id": 100 + i} for i in range(4)]
# AN AGENT-AUTHORED NOTE WHOSE COUNTER COLLIDES WITH A HUMAN NOTE'S ID.
# EventId packs the author into bits 48+ and each author counts independently, so
# makeEventId(kAuthorAgent=1, 100) and the human note 100 differ ONLY in the top 16 bits. A
# command that carries the id in 32 bits truncates the agent note to 100 and edits the human
# note instead — silently, and only for notes an agent wrote. That is the whole point of this
# pair: the ids must be distinguishable end to end, not merely representable.
AGENT_100 = (1 << 48) | 100
notes.append({"nanotick": 5 * Q, "duration": Q // 2, "pitch": 72, "velocity": 100,
              "column": 1, "note_id": AGENT_100})
clip = {"id": 1, "name": "p", "length": BAR, "kind": "symbolic", "notes": notes}
def r(k="none"):
    return {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "rowops"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

start_engine() {  # $1=shm  $2=logfile
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="$1" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --run-seconds 40 >"$2" 2>&1 ) &
  ENG=$!
  for _ in $(seq 1 160); do
    if grep -q 'starting threads' "$2" 2>/dev/null; then return 0; fi
    sleep 0.25
  done
  fail "the engine never came up (see $2)"
}

# ops <file> <noteId> — the five row ops of one note, as "ret,prob,sound,offset,delay".
ops() {
  python3 - "$1" "$2" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
want = int(sys.argv[2])
for clip in doc.get("clips", []):
    for n in clip.get("notes", []):
        if n.get("note_id") == want:
            print("%d,%d,%d,%d,%d" % (n.get("retrigger", 0), n.get("probability", 0),
                                      n.get("sound", 0), n.get("sound_offset", 0),
                                      n.get("delay", 0)))
            raise SystemExit(0)
print("MISSING")
PY
}

SHM="/rowops_$$"
start_engine "$SHM" "$TMP/eng.log"
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load rowops --force >/dev/null 2>&1 || true
sleep 1.5

# ---- WRITES. Every op, on note 100.
cli do set-row-ops --track 0 --note 100 --ret 3 --prob 60 --sound 7 --offset 16384 \
    --delay 120000 >/dev/null 2>&1 || fail "set-row-ops was refused"
sleep 0.6
# ---- MASKS, part 1: only probability on note 101. The other four must stay zero.
cli do set-row-ops --track 0 --note 101 --prob 25 >/dev/null 2>&1 || fail "masked write refused"
sleep 0.6
# ---- MASKS, part 2: set two ops on note 102, then clear ONE of them.
cli do set-row-ops --track 0 --note 102 --ret 4 --sound 9 >/dev/null 2>&1 || fail "write refused"
sleep 0.6
cli do set-row-ops --track 0 --note 102 --clear ret >/dev/null 2>&1 || fail "clear refused"
sleep 0.6

# ---- AUTHORED IDS. Address the AGENT note (author 1, counter 100). The human note 100 already
# carries ops from the first write above, so if the id truncates, this command lands on it and
# changes them — which is exactly the silent wrong-note edit a 32-bit id produced.
AGENT_ID=$(python3 -c "print((1 << 48) | 100)")
cli do set-row-ops --track 0 --note "$AGENT_ID" --prob 77 >/dev/null 2>&1 || \
  fail "addressing an agent-authored note was refused"
sleep 0.6

cli do save afterA --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/afterA.uniproj.json" ] || fail "the save produced no file"

A100="$(ops "$TMP/afterA.uniproj.json" 100)"
A101="$(ops "$TMP/afterA.uniproj.json" 101)"
A102="$(ops "$TMP/afterA.uniproj.json" 102)"
A103="$(ops "$TMP/afterA.uniproj.json" 103)"
AAGENT="$(ops "$TMP/afterA.uniproj.json" "$AGENT_ID")"
echo "  after the writes: 100=[$A100] 101=[$A101] 102=[$A102] 103=[$A103]"
echo "  agent note (author 1, counter 100) = [$AAGENT]"

[ "$AAGENT" = "0,77,0,0,0" ] || \
  fail "the agent-authored note (author 1, counter 100) came back as [$AAGENT], not [0,77,0,0,0].
        Its id needs all 64 bits — a payload that carries 32 truncates the author away"
[ "$A100" = "3,60,7,16384,120000" ] || \
  fail "writing to the AGENT note (author 1, counter 100) changed the HUMAN note 100, which is
        now [$A100]. This is the silent wrong-note edit: the two ids differ only in the author
        bits, so a truncated id addresses whichever note the counter happens to match"
[ "$A100" = "3,60,7,16384,120000" ] || \
  fail "the five ops written to note 100 came back as [$A100], not [3,60,7,16384,120000]"
[ "$A101" = "0,25,0,0,0" ] || \
  fail "writing ONLY probability to note 101 produced [$A101]. A masked write must leave every
        op it did not name alone — otherwise setting one op silently clears the rest"
[ "$A102" = "0,0,9,0,0" ] || \
  fail "note 102 was given retrigger 4 and sound 9, then had its retrigger CLEARED. It came back
        as [$A102], expected [0,0,9,0,0] — either the clear did not happen or it took the sound
        with it"
[ "$A103" = "0,0,0,0,0" ] || \
  fail "note 103 was never addressed and came back as [$A103]. A row-op write reached a note the
        command did not name"

# ---- PERSISTS. Reload the saved file in a FRESH engine and read it back out again. This is the
# assertion that catches a serializer that writes an op it cannot read, which is invisible to any
# check that only looks at the file it just wrote.
SHM2="/rowops2_$$"
start_engine "$SHM2" "$TMP/eng2.log"
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load afterA --force >/dev/null 2>&1 || true
sleep 1.5
cli do save afterB --force >/dev/null 2>&1 || true
sleep 1.8
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/afterB.uniproj.json" ] || fail "the reload's save produced no file"

B100="$(ops "$TMP/afterB.uniproj.json" 100)"
B102="$(ops "$TMP/afterB.uniproj.json" 102)"
echo "  after save + reload: 100=[$B100] 102=[$B102]"
[ "$B100" = "$A100" ] || \
  fail "note 100's ops changed across a save and a reload: [$A100] -> [$B100]. A row op that
        survives the wire but not the file is the failure you find a week later"
[ "$B102" = "$A102" ] || \
  fail "note 102's ops changed across a save and a reload: [$A102] -> [$B102]"

echo "row_ops_write_check: PASS — ops write, only the ops named change, and they survive the file"
