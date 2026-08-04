#!/usr/bin/env bash
# A REFUSED EDIT MUST BE LOUD, AND MUST NOT BE IN THE CLIP.
#
# Clip edits carry the base version the caller read. If the engine's version has moved since, the
# edit is REFUSED — and for a long time nothing on the client side looked: daw-cli printed
# `{"sent": "note", "base_version": 1}` and exited 0 on a command the engine had thrown away. The
# edit was lost and the caller was told it had worked.
#
# That is not a hypothetical. It surfaced as an intermittent chord_expression failure: the chord
# command was refused during the window after a project load, so the chord never entered the clip,
# the transport played over an empty bar, and the check reported "no chord.scheduled" — which
# reads as a scheduler bug rather than as a command nobody accepted.
#
# THE ASSERTION THAT MATTERS IS THE LAST ONE. Exit codes and messages prove the CLIENT noticed;
# only reading the clip back proves what the ENGINE did. A check that stopped at "it printed an
# error" would pass an implementation that reported a refusal and applied the edit anyway, or one
# that reported success and applied nothing — the two failures this exists to tell apart.
#
# STALENESS IS FORCED, NOT RACED. `--base` presents a version the caller read earlier, which is
# what a real concurrent author does. Waiting for the race to happen would make this check
# flaky in the direction that looks like a pass.
#
#   tools/clip_reject_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
. "$ROOT/tools/lib/engine_wait.sh"
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
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

python3 - "$TMP/rj.uniproj.json" <<'PY'
import json, sys
Q = 960000; BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "rj"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": [], "chords": []}],
           "tracks": [tr]}, open(sys.argv[1], "w"))
PY

SHM="/clipreject_$$"
# exec, so $! is the ENGINE and not the subshell that spawned it — without it the kill in cleanup
# reaches a shell that has already exited and the engine outlives the check.
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project rj --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

notes_with_pitch() {  # notes_with_pitch <pitch> -> count
  cli get notes --track 0 2>/dev/null | grep -c "\"pitch\": *$1" || true
}

# ---- A BASELINE EDIT, so the version is definitely past 1 and the stale base below is genuinely
# stale rather than merely early.
cli do note --track 0 --nanotick 0 --pitch 60 --duration "$Q" >/dev/null 2>&1
sleep 0.5
[ "$(notes_with_pitch 60)" -ge 1 ] || \
  fail "the baseline note never landed, so nothing below distinguishes a refusal from an engine
        that is not accepting edits at all"
echo "  baseline: a note with a fresh base lands"

# ---- REFUSED. A deliberately stale base must be reported AND must change nothing.
set +e
cli do note --track 0 --nanotick "$Q" --pitch 62 --duration "$Q" --base 1 >"$TMP/out1" 2>"$TMP/err1"
RC=$?
set -e
[ "$RC" -ne 0 ] || \
  fail "a note with a stale base exited 0. It printed $(tr -d '\n' < "$TMP/out1") and the engine
        refused it — this is the silent loss the whole check exists for: the caller is told the
        edit worked and it is not in the clip"
grep -q "REFUSED" "$TMP/err1" || \
  fail "a refused note exited $RC but said nothing about being refused; the message is what tells
        a caller which edit was lost and what to retry with"
echo "  refused: a stale base exits $RC and says so"

sleep 0.5
[ "$(notes_with_pitch 62)" -eq 0 ] || \
  fail "the REFUSED note is in the clip. The engine reported a refusal and applied the edit
        anyway, which is worse than either outcome on its own"
echo "  refused: and the note is NOT in the clip"

# ---- RETRIED. Opt-in, and it must actually land.
set +e
cli do note --track 0 --nanotick $((Q * 2)) --pitch 64 --duration "$Q" --base 1 --retry-stale \
    >"$TMP/out2" 2>"$TMP/err2"
RC2=$?
set -e
[ "$RC2" -eq 0 ] || \
  fail "--retry-stale exited $RC2. The engine hands back the version to retry with; a retry that
        cannot use it leaves the caller no way to make a stale edit land"
sleep 0.5
[ "$(notes_with_pitch 64)" -ge 1 ] || \
  fail "--retry-stale exited 0 but the note is NOT in the clip. Reporting success for an edit
        that was never applied is the exact defect this check was written for, one level up"
echo "  retried: --retry-stale recovers the edit and it IS in the clip"

echo "clip_reject_check: PASS — a refused edit is loud and absent, a retried one is silent and present"
