#!/usr/bin/env bash
# THE VERSION ON AN ANSWER DESCRIBES THAT ANSWER.
#
# The kit read-back carries two version numbers and they mean different things:
#
#   kit_version      the model's poll counter, written every publish cycle. "The kit has moved
#                    since you last looked."
#   content_version  stamped into the answer when it is built, from the snapshot it was built
#                    from. "This is what you are looking at."
#
# There was only the first, and a reader had no choice but to take it as describing the answer it
# arrived with. It does not: the two are written on different clocks, so about a second after an
# edit a request returns the NEW version alongside the OLD content. Read again and the content is
# right with no version change at all.
#
# WHY THAT IS WORSE THAN A PLAIN STALE READ. This is the field a UI polls to decide whether what
# it drew is still current. Poll says "changed" -> request -> receive the old content labelled
# with the new version -> redraw the stale kit -> and never poll again, because the version
# already matches. The mistake latches until some unrelated edit moves the counter again.
#
# THE PROPERTY, which is the one thing worth asserting: the content and the version it is
# labelled with must agree, on EVERY answer, including the ones taken mid-flight. Not "the answer
# eventually becomes current" — that was true before and hid the defect.
#
# TESTED STRUCTURALLY, NOT BY RACING THE WINDOW. Two earlier versions of this check tried to
# sample the interval where the model has moved and an answer has not, and its negative control
# PASSED both times: the window is about one publish cycle and each sample costs a process
# launch, so whether you land inside it is luck. A check that only catches the bug when it wins a
# race is a check that reports the scheduler.
#
# So the discriminator is a SECOND TRACK. kit_version is GLOBAL — any track's sampler edit bumps
# it — while an answer describes ONE track's snapshot. Edit track 1 and then ask track 0: its
# kit_version must move, because the kit did move somewhere; its content_version must NOT, because
# nothing about track 0 changed. With content_version taken from the model counter, track 0's
# answer claims a new version for content that is byte-for-byte what it was, which is the defect
# stated exactly. No timing, no window, no luck.
#
#   tools/kit_readback_version_check.sh
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
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

python3 - "$TMP/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr)))
w.close()
PY

python3 - "$TMP/k.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 1, "next_source_id": 1, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [], "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": []}}
def track(i):
    import copy
    return {"track_id": i, "name": "S%d" % i, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": [copy.deepcopy(dev)], "mod_links": [], "placements": []}
# TWO SAMPLER TRACKS. The second one is the whole discriminator: it is what gets edited, so that
# track 0's answer has to distinguish "the kit moved" from "MY kit moved".
json.dump({"schema_version": 4, "meta": {"name": "k"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [track(0), track(1)]}, open(out, "w"))
PY

export DAW_UI_SHM_NAME="/kitver_$$" DAW_PROJECT_DIR="$TMP"
( cd "$BUILD" && ./daw_engine --project k --run-seconds 30 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
# WAITS FOR THE PROJECT, NOT FOR THE THREADS. "starting threads" is printed before the startup
# project has been loaded, so a command sent on that signal can arrive at an engine whose tracks
# do not exist yet — and it is REFUSED, with a reason, into the engine's log where nothing here
# was looking. With two tracks the load takes longer and the race became reliable: every
# sampler-load came back "no_sampler_device" and the check reported that the read-back returned
# nothing, which was true and about the wrong thing.
wait_for_boot "$TMP/eng.log" "$ENG" 160
grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null || fail "the engine never loaded its project"

"$CLI" do sampler-load --track 0 --file s.wav --root 60 >/dev/null 2>&1
"$CLI" do sampler-load --track 1 --file s.wav --root 60 >/dev/null 2>&1
sleep 1.5

read3() {  # kit_version content_version filter_type for a track, on one line
  "$CLI" get sampler-kit --track "$1" 2>/dev/null |
    grep -oE '"kit_version": [0-9]+|"content_version": [0-9]+|"filter_type": [0-9]+' |
    grep -oE '[0-9]+$' | tr '\n' ' '
}

# WAITS FOR THE FIRST ANSWER rather than sleeping a fixed time. Two sampler tracks mean two
# hosts to bring up, and a fixed sleep that was ample for one is not for two — which showed up
# as "the kit read-back returned nothing", a message about the read-back that was really about
# the clock.
KV0=""; CV0=""; FT0=""
for _ in $(seq 1 40); do
  read -r KV0 CV0 FT0 <<<"$(read3 0)"
  [ -n "${FT0:-}" ] && break
  sleep 0.25
done
[ -n "${FT0:-}" ] || fail "the kit read-back returned nothing within 10s — see $TMP/eng.log"
[ "$FT0" = "0" ] || fail "track 0 starts with filter_type $FT0, not 0, so a change to it would
        not be distinguishable and the comparison below would be meaningless"
echo "  track 0 before: kit_version $KV0, content_version $CV0, filter_type $FT0"

# ---- THE EDIT LANDS ON TRACK 1. Nothing about track 0 changes.
"$CLI" do sampler-filter --track 1 --type lp12 --cutoff 300 >/dev/null 2>&1

# Wait for the edit to be visible SOMEWHERE, so the comparison is not made before anything
# happened. Track 1 is the one that changed, so that is what to wait on.
CONVERGED=0
for _ in $(seq 1 40); do
  read -r KV1 CV1 FT1 <<<"$(read3 1)"
  if [ "${FT1:-0}" = "1" ]; then CONVERGED=1; break; fi
  sleep 0.25
done
[ "$CONVERGED" = "1" ] || fail "the edit never became visible on track 1 within 10s, so there is
        nothing to compare against on track 0"
echo "  track 1 after:  kit_version $KV1, content_version $CV1, filter_type $FT1"

read -r KVA CVA FTA <<<"$(read3 0)"
echo "  track 0 after:  kit_version $KVA, content_version $CVA, filter_type $FTA"

# ---- THE POLL COUNTER IS GLOBAL, so it must have moved: the kit DID change, somewhere.
[ "$KVA" -gt "$KV0" ] || \
  fail "kit_version did not move ($KV0 -> $KVA) after an edit on another track. It is the
        engine-wide 'something changed' counter, and a UI that polls it would never learn that
        track 1 moved at all"

# ---- THE ANSWER DESCRIBES ONE TRACK, so it must NOT have moved: nothing about track 0 changed.
[ "$CVA" = "$CV0" ] || \
  fail "track 0's content_version moved ($CV0 -> $CVA) although nothing about track 0 changed —
        the edit was on track 1. So content_version is not describing this answer, it is
        repeating the global poll counter, and the two facts it exists to separate are still one.
        A reader comparing them to decide whether to redraw learns nothing"

[ "$FTA" = "0" ] || \
  fail "track 0's filter_type became $FTA after an edit to track 1 — the edit crossed tracks,
        which is a different and worse bug than the one under test"

echo "  the edit was on track 1: the global counter moved, track 0's answer did not claim to"

echo "kit_readback_version_check: PASS — the version on an answer describes that answer"
