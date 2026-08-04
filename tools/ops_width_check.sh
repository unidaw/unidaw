#!/usr/bin/env bash
# THE OPS COLUMN'S WIDTH IS A PROPERTY OF THE TRACK, NOT OF WHAT IS ON SCREEN.
#
# `uiTrackOpsWidth` publishes the widest op run on any note in a track — how many glyphs the
# collapsed ops cell has to be able to draw. Both unbuilt halves of SAMPLER_DESIGN R5 need this
# one fact: 0 means no note in the track carries an op, so do not draw the column at all; N means
# the widest run is N.
#
# WHY THE ENGINE AND NOT THE CLIENT. Requested by the web-UI agent, and their reason is the whole
# justification: a client sees a WINDOW — the rows being drawn — so anything computed from it
# changes as you scroll. A column that widens when you scroll past a dense row and narrows coming
# back is worse than a clipped one: the grid reflows under the cursor while you are typing into
# it, and two people scrolling differently get different layouts for one song. The engine owns
# every note, so it can answer once.
#
# It was becoming urgent from the other end too: they measured the collapsed cell at about seven
# glyphs, and v33 took the op count from five to seven. The eighth op — FILL or PRE, both already
# sketched — would have been clipped silently, and the ops that fall off the end are precisely
# the ones nobody can see are missing.
#
# THREE PROPERTIES:
#   NONE      a track whose notes carry no ops publishes 0. Without this, "it publishes 4" is
#             equally consistent with a field that is always the note count or always nonzero
#   WIDEST    a track publishes the MAX over its notes, not the first or last one's count — and
#             the fixture puts the widest note in the MIDDLE, so both of those wrong answers give
#             a different number rather than accidentally the right one
#   PER TRACK the two tracks disagree, which is what says the number is indexed by track at all
#
#   tools/ops_width_check.sh
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
# `wait` after the kill, or the shell prints its own "Terminated" job notice AFTER the verdict
# line, which reads like the check crashed at the end of a successful run.
cleanup() { [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }; rm -rf "$TMP"; }
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
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP/o.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def note(tick, nid, **ops):
    n = {"nanotick": tick, "duration": Q // 2, "pitch": 60, "velocity": 100,
         "column": 0, "note_id": nid}
    n.update(ops)
    return n
# TRACK 0. The WIDEST note is in the MIDDLE, deliberately: taking the first note's count would
# give 1 and taking the last note's would give 0, so both wrong answers are visibly wrong rather
# than accidentally right.
#   note 1: retrigger only                              -> 1 glyph
#   note 2: retrigger, probability, delay, sound, ramp   -> 5 glyphs   <- the answer
#   note 3: no ops at all                                -> 0 glyphs
busy = [
    note(0, 1, retrigger=3),
    note(Q, 2, retrigger=4, probability=60, delay=Q // 8, sound=7, retrig_ramp=-60),
    note(Q * 2, 3),
]
# TRACK 1. No ops anywhere — R5's "do not draw the column".
plain = [note(0, 11), note(Q, 12)]
def track(i, name, clip_id):
    return {"track_id": i, "name": name, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": [], "mod_links": [],
            "placements": [{"clip_id": clip_id, "id": clip_id, "at": 0, "length": BAR,
                            "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "o"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "busy", "length": BAR, "kind": "symbolic",
                      "notes": busy},
                     {"id": 2, "name": "plain", "length": BAR, "kind": "symbolic",
                      "notes": plain}],
           "tracks": [track(0, "Busy", 1), track(1, "Plain", 2)]}, open(out, "w"))
PY

SHM="/opswidth_$$"
( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" \
    ./daw_engine --project o --run-seconds 25 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 40
ocli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$SHM" "$CLI" "$@"; }
# POLL FOR THE TRACK REGION rather than sleeping at it — wait_for_boot says the project loaded,
# not that `get tracks` has anything to read.
tracks_ready() { ocli get tracks 2>/dev/null | grep -q '"track_id"'; }
wait_until 20 tracks_ready || true

width() {  # width <trackIndex>
  ocli get tracks 2>/dev/null | python3 -c "
import json, sys
t = json.load(sys.stdin).get('tracks', [])
print(t[int('$1')].get('ops_width') if len(t) > int('$1') else 'missing')"
}

W0="$(width 0)"
W1="$(width 1)"
echo "  track 0 (widest note carries 5): ops_width $W0"
echo "  track 1 (no ops at all):         ops_width $W1"

# ---- NONE.
[ "$W1" = "0" ] || \
  fail "a track whose notes carry NO ops publishes ops_width '$W1'. 0 is what R5 reads as 'do not
        draw the column', so a nonzero here means the column is drawn for every track — and it
        also means '$W0' on the other track could be a constant rather than a measurement"

# ---- WIDEST, and the middle note is the answer.
[ "$W0" = "5" ] || \
  fail "track 0's widest note carries FIVE ops (retrigger, probability, delay, sound, ramp) and
        ops_width is '$W0'. 1 would be the FIRST note's count, 0 the LAST note's — the widest is
        deliberately in the middle so neither wrong answer lands on the right number. Anything
        else means the ops are being counted differently from the way the cell draws them, and a
        width that is not the glyph count is wrong by one at the worst moment"

# ---- PER TRACK.
[ "$W0" != "$W1" ] || \
  fail "both tracks publish the same width ($W0), so the number is not indexed by track"

echo "ops_width_check: PASS — the width is the widest run in the TRACK, 0 when there are no ops,"
echo "                 and the two tracks disagree"
