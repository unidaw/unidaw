#!/usr/bin/env bash
# THE ARRANGE SUMMARY'S GATE HAS TWO INPUTS, AND THIS PINS THE SECOND ONE.
#
# The region carries the marker list, the meter points AND songEndTick. It is republished only when
# something it carries has moved, which is what stops a 4 KB rewrite every frame — and the gate was
# once the section/arrange version ALONE:
#
#     if (!force && version == lastArrangeVersion) return;
#
# The song end changes on a PLACEMENT edit, which moves no marker and no meter point. So a client
# that drew the song end from this region kept the value from the last arrangement edit, and NO
# READER COULD TELL: the version it caches on had not moved either, so the stale value looked
# exactly like a current one. The fix was to gate on both inputs and publish a generation that
# advances whenever anything in the region did.
#
# THAT PROPERTY HAD NO COVERAGE. arrange_summary_check no longer exists — it was replaced by
# arrangement_check, which pins markers, the meter and the ripple and never reads song_end_tick.
# Dropping the second half of the gate left the entire suite green. This check is the missing half.
#
# WHY A PLACEMENT MOVE AND NOT A NOTE EDIT. A note edit moves the song end by nothing at all
# (recomputeSongEnd runs on a placement edit, a section ripple or a load), and a marker or meter
# edit bumps arrangeVersion, which would satisfy the one-input gate too — so either of those would
# pass with the bug present. A placement move is the only edit that separates the two inputs.
#
# Needs the engine and daw-cli built. Runs with no audio device.
#   tools/arrange_song_end_gate_check.sh
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
ENG=""
cleanup() {
  [ -n "$ENG" ] && stop_engine "$ENG"
  rm -rf "$TMP"
}
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

# One track, two placements. The later one at 12 bars is what the song end follows.
python3 - "$TMP/se.uniproj.json" "$Q" <<'PY'
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
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 1, "id": 2, "at": 12 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "se"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "markers": [{"id": 1, "nanotick": 0, "name": "intro", "color_rgb": 0}],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/segate_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --no-audio --run-seconds 60 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
# THREADS FIRST, THEN THE LOAD, THEN wait_for_boot — in that order, because wait_for_boot waits for
# the LOAD to be visible and a load that has not been sent yet never becomes visible. Getting this
# backwards reports "the engine is stuck rather than dead", which is true and useless.
for _ in $(seq 1 120); do
  grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null && break
  sleep 0.25
done
cli do load se --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80 || fail "engine did not boot"

# Read the published region through python: a grep that matches the wrong line is how a check
# passes with the bug present.
field() { cli get arrangement 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(1)
print(d.get('$1', ''))
"; }

# WAIT FOR THE SONG END TO BE THE LOADED ONE, not merely for the region to exist. The region is
# published before a project is loaded, carrying a song end of 0 — polling for the key would be
# satisfied instantly and every assertion below would race the load.
want0=$((13 * BAR))
have0_ready() { [ "$(field song_end_tick)" = "$want0" ]; }
wait_until 20 have0_ready || fail "song end never reached $want0 after load (saw '$(field song_end_tick)')"

v0="$(field version)"
e0="$(field song_end_tick)"
[ -n "$v0" ] || fail "no generation published"
[ "$e0" = "$want0" ] || fail "song end after load is $e0, expected $want0"

# THE EDIT: move the last placement four bars later. It touches no marker, no meter point and no
# section, so arrangeVersion does NOT move — which is the whole point. The song end must follow it
# anyway, and the generation must advance to say so.
cli do move-placement --track 0 --placement 2 --at $((16 * BAR)) >/dev/null 2>&1 \
  || fail "move-placement was refused"

# THE LAST VALUE SEEN IS RECORDED WHILE THE ENGINE IS STILL ALIVE. The first version of this loop
# read the region again inside the failure message — by which time the engine had exited and both
# fields came back EMPTY, so the control fired with a diagnostic that named neither number. A
# failure message that has to re-query a dead process is not a message.
want1=$((17 * BAR))
seen_end=""; seen_ver=""
moved() {
  seen_end="$(field song_end_tick)"
  seen_ver="$(field version)"
  [ "$seen_end" = "$want1" ]
}
if ! wait_until 20 moved; then
  echo "  the arrangement region still reports song_end_tick=$seen_end, generation=$seen_ver"
  echo "  after moving a placement from 12 to 16 bars. The gate is not watching the song end:"
  echo "  a client reading the song end from this region cannot tell it is stale, because the"
  echo "  version it caches on did not move either."
  fail "song end did not follow the placement (expected $want1, first read $e0)"
fi

v1="$(field version)"
e1="$(field song_end_tick)"
[ "$e1" = "$want1" ] || fail "song end is $e1, expected $want1"

# THE GENERATION MUST ADVANCE TOO, and it is a separate assertion on purpose: a region whose body
# changed without its version moving is unreadable — every client caches on that number, so a
# correct body behind a stale stamp is not delivered.
[ "$v1" -gt "$v0" ] 2>/dev/null \
  || fail "generation did not advance: $v0 -> $v1, though the song end changed $e0 -> $e1"

# AND A SECOND READ MUST BE STABLE. If the gate had been left permanently open instead — the other
# way to make the assertions above pass — the generation would climb on every publish, which is the
# 4 KB-per-frame rewrite the gate exists to prevent.
sleep 0.5
v2="$(field version)"
[ "$v2" = "$v1" ] \
  || fail "the generation is still climbing with nothing changed ($v1 -> $v2): the gate is open"

echo "arrange_song_end_gate_check: PASS — a placement move at 12->16 bars moved the song end" \
     "$e0 -> $e1 and the generation $v0 -> $v1, and the generation then held steady"
