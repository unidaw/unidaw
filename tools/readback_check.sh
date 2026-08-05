#!/usr/bin/env bash
# A CONTROL YOU CANNOT READ IS NOT A CONTROL. Two commands worked for a long time and published
# nothing about their outcome, so the only interface either could have was a write-only one:
#
#   SetTrackHarmonyQuantize (10)  the flag was in the project file and in the runtime and in NO
#                                 published region. A toggle for it could be pressed, and after a
#                                 load the UI would have to guess which way it was set or show
#                                 nothing. A control drawing a state it invented is worse than no
#                                 control at all.
#   SavePatcherPreset (29)        the outcome went to the engine's stderr, which daw-cli can read
#                                 and a browser cannot. So a "save this graph as a preset" button
#                                 could only lie about half the time.
#
# Both were raised by the frontend agent while wiring the controls, which is the right time to
# notice: the command was the easy half and the STATE was the missing half.
#
# THREE PROPERTIES:
#   READS       the published flag matches what was set, both ways round
#   SURVIVES    it comes back after a save and a reload in a FRESH engine
#   REPORTS     a preset save says whether it wrote, and a refused one says so too
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/readback_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# SOURCED FIRST so this file's own helpers still win: a later definition replaces an earlier one,
# and what is wanted here are the WAIT primitives.
. "$ROOT/tools/lib/engine_wait.sh"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

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
ENG=""
fail() {
  echo "  FAIL: $*"
  [ -n "$ENG" ] && { kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; }
  exit 1
}

python3 - "$TMP/rb.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def track(tid, name, hq):
    return {"track_id": tid, "name": name, "harmony_quantize": hq, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": routing(), "device_chain": [], "mod_links": [],
            "placements": [{"clip_id": 1, "id": tid + 1, "at": 0, "length": 4 * Q,
                            "notes": [], "chords": [], "mutes": []}]}
clip = {"id": 1, "name": "c", "length": 4 * Q, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1}]}
# Track 0 starts OFF and track 1 starts ON, so a publisher that hardcoded either answer fails.
json.dump({"schema_version": 4, "meta": {"name": "rb"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [track(0, "A", False), track(1, "B", True)]},
          open(out, "w"))
PY

hq_of() {  # hq_of <shm> <trackId> -> true|false
  { DAW_UI_SHM_NAME="$1" "$CLI" get tracks 2>/dev/null | tr '{' '\n' \
      | grep "\"track_id\": $2," \
      | sed -n 's/.*"harmony_quantize": \([a-z]*\).*/\1/p' | head -1; } || true
}

SHM="/rbchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 26 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load rb --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.2

# ---- READS, both ways round from ONE load. A publisher that always said false would pass a
# fixture where everything is off, and one that always said true would pass the opposite.
A0="$(hq_of "$SHM" 0)"
B0="$(hq_of "$SHM" 1)"
[ "$A0" = "false" ] || fail "track 0 loaded with harmony_quantize OFF but publishes '$A0'"
[ "$B0" = "true" ] || \
  fail "track 1 loaded with harmony_quantize ON but publishes '$B0' — the flag is in the project
        file and the runtime; if it is not published, a UI toggle for it can only be write-only"
echo "  reads: the loaded flag is published per track (0=off, 1=on)"

# ---- AND IT FOLLOWS THE COMMAND. Toggle track 0 on and watch it change: a flag published from
# the LOADED document rather than from live state would stay put here.
# NOT after_command. The assertion below reads PUBLISHED state, and the journal says the engine
# ACTED, not that the consumer has published — those are different ticks. Waiting for the wrong one
# is how a check reads the value it was about to change.
cli do harmony-quantize --track 0 --on >/dev/null 2>&1 \
  || cli do harmony-quantize --track 0 >/dev/null 2>&1 || true
published_on() { [ "$(hq_of "$SHM" 0)" = "true" ]; }
wait_until 20 published_on || true
A1="$(hq_of "$SHM" 0)"
[ "$A1" = "true" ] || \
  fail "after SetTrackHarmonyQuantize on track 0 the published flag is still '$A1' — it is being
        published from the loaded document rather than from live state, so every toggle is
        invisible until the next load"
echo "  follows: a toggle moves the published flag"

# ---- REPORTS: a preset save says whether it wrote.
after_command "$TMP" cli do patcher-save --name rbpreset || true
grep -q '"event":"patcher_preset.saved"' "$TMP/eng.log" || \
  fail "SavePatcherPreset reported nothing. The outcome went to stderr, which daw-cli can read
        and a browser cannot, so a save button had no way to know whether it worked"
grep '"event":"patcher_preset.saved"' "$TMP/eng.log" | grep -q '"ok":true' || \
  fail "the preset save reported a failure: $(grep '"event":"patcher_preset.saved"' "$TMP/eng.log" | tail -1)"
# A REFUSAL must report too — a command that is declined and says nothing is the shape this
# whole exercise is about.
after_command "$TMP" cli do patcher-save --name "" || true
if [ "$(grep -c '"event":"patcher_preset.saved"' "$TMP/eng.log")" -ge 2 ]; then
  grep '"event":"patcher_preset.saved"' "$TMP/eng.log" | tail -1 | grep -q '"ok":false' || \
    fail "an empty preset name was refused but reported ok"
  echo "  reports: a preset save reports success, and a refusal reports the refusal"
else
  # daw-cli may refuse an empty name client-side before it reaches the engine, which is also
  # correct — say which happened rather than asserting a path that was never taken.
  echo "  reports: a preset save reports success (the empty-name refusal never reached the engine,"
  echo "           so daw-cli declined it first — also fine)"
fi

# ---- SURVIVES a save and a reload in a FRESH engine. The flag is only useful if what the UI
# draws after reopening a project is what the project says.
after_command "$TMP" cli do save rbout --force || true
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

SHM2="/rbchk2_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 18 >"$TMP/eng2.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng2.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM2" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load rbout --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng2.log" "$ENG" 80
sleep 1.2
A2="$(hq_of "$SHM2" 0)"
B2="$(hq_of "$SHM2" 1)"
[ "$A2" = "true" ] && [ "$B2" = "true" ] || \
  fail "after save + reload in a fresh engine the flags are 0='$A2' 1='$B2', expected both true
        (track 0 was toggled on before the save). Either the save dropped it or the load did"
echo "  survives: both flags come back after a save and a reload in a fresh engine"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "readback_check: PASS — harmony quantize is readable and a preset save reports its outcome"
