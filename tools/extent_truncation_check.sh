#!/usr/bin/env bash
# A CAP THAT IS REACHED MUST SAY SO. Overflow the published clip-extent list and check the
# engine reports the shortfall rather than dropping it quietly.
#
# kUiMaxClipExtents went 64 -> 256 when the arrangement summary landed, and the overflow stayed a
# bare `break` — so the silent truncation was moved further out rather than fixed, while the
# arrangement region added in the SAME change does publish its truncation counts. The rule was
# written down at one site and not applied at the other.
#
# It matters because of what it looks like from outside: the rails simply stop. A person sees a
# song whose later clips have no boxes and no reason, and the natural conclusion is that the
# clips are gone — which is exactly the wrong place to start looking.
#
# The audio-clip table has the same shape and a worse version of it: kUiMaxAudioClips is 64 while
# the extent list holds 256, and its comment claimed the two were equal. So a project between
# those numbers publishes complete rails with waveform data missing from the tail — boxes with
# nothing in them.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/extent_truncation_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

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

# The cap read from the HEADER, not typed in. A test carrying its own copy of a constant stops
# testing the boundary the moment the real one moves — and this one already moved once.
CAP="$(sed -n 's/^constexpr uint32_t kUiMaxClipExtents = \([0-9]*\).*/\1/p' \
  "$ROOT/apps/shared_memory.h" | head -1)"
case "$CAP" in
  ''|*[!0-9]*) fail "could not read kUiMaxClipExtents from apps/shared_memory.h (got '$CAP') —
        the check cannot assert on a boundary it cannot find" ;;
esac
OVER=$((CAP + 12))
echo "  cap is $CAP; building a project with $OVER placements"

python3 - "$TMP/many.uniproj.json" "$Q" "$OVER" <<'PY'
import json, sys
out, Q, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = 4 * Q
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
# One clip, placed n times back to back on one track. The extent list has one entry per
# PLACEMENT, so this is the cheapest way to exceed it.
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic",
        "notes": [{"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                   "column": 0, "note_id": 1}]}
placements = [{"clip_id": 1, "id": i + 1, "at": i * BAR, "length": BAR,
               "notes": [], "chords": [], "mutes": []} for i in range(n)]
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [], "mod_links": [], "placements": placements}
json.dump({"schema_version": 4, "meta": {"name": "many"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

SHM="/extrunc_$$"
( cd "$BUILD" && env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --run-seconds 24 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
for _ in $(seq 1 120); do
  if grep -q 'starting threads' "$TMP/eng.log" 2>/dev/null; then break; fi
  sleep 0.25
done
cli() { DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }
cli do load many --force >/dev/null 2>&1 || true
wait_for_boot "$TMP/eng.log" "$ENG" 80
sleep 1.5

# The list itself must be FULL — a cap that publishes less than its capacity is a different bug
# and would make the shortfall number meaningless.
PUBLISHED="$({ cli get extents 2>/dev/null | grep -c '"placement":'; } || true)"
[ "${PUBLISHED:-0}" = "$CAP" ] || \
  fail "the extent list holds ${PUBLISHED:-0} of $CAP entries; expected it to be full before
        anything can be said about what did not fit"

# THE SHORTFALL IS REPORTED. Both as an event and to the caller — a UI that draws what it is
# given without checking is exactly the reader that needs the warning.
WARN="$(cli get extents 2>&1 >/dev/null | grep -c 'did not fit' || true)"
[ "${WARN:-0}" -ge 1 ] || \
  fail "daw-cli printed no truncation warning, so a caller reading the list has no way to know
        it is incomplete — the rails just stop and the clips look gone"
EV="$(grep -o '"event":"clip_extents.truncated"[^}]*' "$TMP/eng.log" | tail -1)"
[ -n "$EV" ] || \
  fail "the engine logged no clip_extents.truncated event. A truncated list nobody notices reads
        as a complete one, which is how 'the rails are missing clips' becomes a bug report about
        the rails"
# And the NUMBER must be right, not just present. 12 over the cap by construction.
echo "$EV" | grep -q "\"dropped\":12" || \
  fail "the engine reported the wrong shortfall: $EV (expected dropped:12 — the fixture is
        exactly 12 over the cap). A counter that stops at the first overflow says 'some' when
        the caller needs 'how many'"
echo "  truncation: the list is full at $CAP, the engine reports dropped:12, and daw-cli warns"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
echo "extent_truncation_check: PASS — a reached cap is published and said out loud"
