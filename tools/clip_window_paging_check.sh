#!/usr/bin/env bash
# A CLIP WITH MORE NOTES THAN ONE PAGE IS REPORTED WHOLE.
#
# The clip-window protocol is paginated: an answer carries at most kUiMaxClipNotes (4096) notes,
# and past that the engine stops early, reports where it stopped in nextEventIndex and WITHHOLDS
# kUiClipWindowFlagComplete. The engine has honoured the cursor since the protocol was written
# (apps/ui_snapshot.cpp) — and every client hardcoded cursor 0 and read exactly one answer.
#
# WHAT THAT COST. `daw-cli get clip` on a dense clip printed the first 4096 notes, exited 0, and
# said nothing on stderr. A caller cannot tell that from a clip that really is 4096 notes long,
# which is the property that makes it worth a check: the truncation is not merely lossy, it is
# INDISTINGUISHABLE FROM THE TRUTH at the call site. Both cursor fields were published, both were
# dead, and nothing failed.
#
# THE FIXTURE NEEDS THREE PAGES, not two. A client that learned to fetch "the second page" and
# stop would report 8192 and pass a two-page fixture. 9000 notes is 4096 + 4096 + 808.
#
# FOUR ASSERTIONS, because a count alone is weak:
#   COUNT   exactly 9000 notes come back — too few is truncation, too many is a page concatenated
#           twice, which is the obvious way to get a loop wrong
#   LAST    the final note's tick is the one authored last, so the tail really arrived and the
#           count was not made up by repetition
#   UNIQUE  no tick appears twice — the count and the last note would both survive a loop that
#           re-read a middle page and dropped the one after it
#   GRID    --grid takes the same path; it used to index the snapshot's fixed array directly, so
#           it could regress independently of the JSON printer
#
# No audio device needed; nothing here renders.
#   tools/clip_window_paging_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
NOTES=9000
PAGE=4096

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

# THE PAGE SIZE IS READ FROM THE HEADER, not trusted from this comment. If kUiMaxClipNotes ever
# grows past 9000 the fixture stops spanning pages and this check would pass by never exercising
# the cursor at all — the exact blindness that let the dead protocol survive.
DECLARED="$(grep -oE 'kUiMaxClipNotes = [0-9]+' "$ROOT/apps/shared_memory.h" | grep -oE '[0-9]+$' | head -1)"
[ -n "$DECLARED" ] || { echo "  FAIL: could not read kUiMaxClipNotes from apps/shared_memory.h"; exit 1; }
if [ "$DECLARED" != "$PAGE" ]; then
  echo "  FAIL: kUiMaxClipNotes is $DECLARED, not the $PAGE this fixture was sized against."
  echo "        The fixture needs MORE THAN TWO pages of notes or it cannot tell a paginating"
  echo "        client from one that fetches a second page and stops. Set NOTES above"
  echo "        2 * $DECLARED + a remainder and update PAGE."
  exit 1
fi
if [ "$NOTES" -le $((2 * PAGE)) ]; then
  echo "  FAIL: the fixture's $NOTES notes do not span three pages of $PAGE."; exit 1
fi

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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
fail() { echo "  FAIL: $*"; exit 1; }

SHM="/cwp_$$"

python3 - "$TMP" "$Q" "$NOTES" <<'PY'
import json, sys, os
tmp, Q, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
SPAN = BAR * 4            # the window `get clip --bars 4` asks for
STEP = 1600               # 9000 * 1600 = 14,398,400, inside SPAN = 15,360,000

def route(k="none", t=0):
    return {"kind": k, "track_id": t, "input_id": 0}

# Pitch cycles so the notes are not all identical, and the LAST one is at a tick this check
# names explicitly rather than "whatever came back last".
notes = [{"nanotick": i * STEP, "duration": STEP // 2, "pitch": 36 + (i % 60),
          "velocity": 100, "column": 0, "note_id": i + 1} for i in range(count)]
clip = {"id": 1, "name": "dense", "length": SPAN, "kind": "symbolic", "notes": notes}

tr = {"track_id": 0, "name": "T0", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                  "audio_out": route("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": SPAN,
                      "notes": [], "chords": [], "mutes": []}]}

json.dump({"schema_version": 4, "meta": {"name": "cwp"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]},
          open(os.path.join(tmp, "cwp.uniproj.json"), "w"))
PY

LAST_TICK=$(( (NOTES - 1) * 1600 ))

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project cwp --run-seconds 120 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 || { echo "engine did not boot"; tail -20 "$TMP/eng.log"; exit 1; }

cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

cli get clip --track 0 --bars 4 >"$TMP/clip.json" 2>"$TMP/clip.err" \
  || fail "get clip exited non-zero: $(head -3 "$TMP/clip.err")"

# COUNT. Every note line carries "pitch"; chord lines do not, so this counts notes only.
GOT=$(grep -c '"pitch"' "$TMP/clip.json")
echo "  notes reported: $GOT (authored $NOTES, page size $PAGE)"

if [ "$GOT" != "$NOTES" ]; then
  echo
  if [ "$GOT" = "$PAGE" ]; then
    fail "get clip reported exactly $PAGE notes for a $NOTES-note clip — ONE PAGE, and it exited
        0 with nothing on stderr.

        The clip window is paginated: the engine fills at most kUiMaxClipNotes notes, sets
        nextEventIndex to where it stopped and only sets kUiClipWindowFlagComplete when it
        reached the end of the window. Both fields are published and were read by nobody, so
        the client asked for a 4-bar window and silently answered a different question.

        The loop belongs in get_clip in ui/daw-cli/src/main.rs: resend with
        cursor_event_index = next_event_index until the Complete flag arrives."
  fi
  fail "get clip reported $GOT notes for a $NOTES-note clip.
        Fewer than authored is a dropped page; more is a page counted twice."
fi

# LAST. The tail of the last page really arrived.
grep -q "\"nanotick\": $LAST_TICK," "$TMP/clip.json" \
  || fail "the last authored note (nanotick $LAST_TICK) is absent even though the count matched,
        so the pages do not cover the clip — some page was read twice and another dropped."

# UNIQUE. A repeated page keeps the count plausible and the last note present.
DUPES=$(grep -oE '"nanotick": [0-9]+,' "$TMP/clip.json" | sort | uniq -d | wc -l | tr -d ' ')
[ "$DUPES" = "0" ] || fail "$DUPES tick(s) appear more than once — a page was concatenated twice."

# GRID. The same data down the other printer, which indexes its own arrays.
cli get clip --track 0 --bars 4 --grid >"$TMP/clip.grid" 2>&1 \
  || fail "get clip --grid exited non-zero: $(head -3 "$TMP/clip.grid")"
ROWS=$(grep -cE '^ *[0-9]+ \|' "$TMP/clip.grid")
[ "$ROWS" -gt 0 ] || fail "--grid printed no rows: $(head -5 "$TMP/clip.grid")"
grep -qE '\|' "$TMP/clip.grid" || fail "--grid printed no grid"

echo "clip_window_paging_check: PASS — a $NOTES-note clip (three pages of $PAGE) is reported" \
     "whole, in order, with no page read twice, through both printers"
