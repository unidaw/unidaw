#!/usr/bin/env bash
# A SAMPLER MADE ENTIRELY BY COMMANDS MAKES SOUND, WITH NO ENVELOPE SENT.
#
# This is the whole chop workflow and the first thing anyone does: add a sampler, load a sample,
# play a note. Nothing in it sets an envelope, so the slot uses the mod set `sampler-load` mints
# — defaultModSet — and that path was rendered by NO check in this suite.
#
# Every sampler fixture here writes its own mod set with an explicit `"modulators": []`, which
# skips the default entirely. So the default amp envelope, which every command-created kit in the
# product actually uses, was exercised by nothing. The web-UI agent reported silence on exactly
# this path from their tree; it renders at full level on mine, and the gap that let the question
# be open at all is that no check covered it either way.
#
# WHY A DEFAULT ENVELOPE IS EASY TO GET WRONG: makeAdsr(0, 0, 1000, 5000) puts THREE of its four
# points at t=0 — the zero, the attack peak and the sustain — with a sustain loop pinned to point
# 2. A runner that lands on the first point at a shared time holds level 0 forever and the kit is
# mute, with every structural fact correct. That is a one-character difference from correct.
#
# THREE PROPERTIES:
#   SOUNDS     the take is not silent
#   SUSTAINS   it is still at level well after the 5 ms release would have ended it, which is
#              what says the sustain loop held rather than the note blipping and stopping
#   FULL       it is within a few dB of the same slot rendered with an explicit full-level
#              envelope — so "audible but at a fraction of the level" fails too
#
#   tools/sampler_default_sound_check.sh
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
# A STEADY second-long tone. Steady because the property below is "still at level late in the
# note": a decaying sample would go quiet on its own and the check would pass on the sample's
# envelope rather than the slot's.
w.writeframes(b''.join(
    struct.pack('<h', int(13000 * math.sin(2 * math.pi * 400.0 * i / sr))) for i in range(sr)))
w.close()
PY

python3 - "$TMP/d.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# AN EMPTY SAMPLER — no sources, no slots, and NO MOD SETS. This is what add-device produces, and
# it is the state in which sampler-load mints defaultModSet. A fixture that declared its own mod
# set here would skip the very thing under test, which is what every other fixture in this suite
# does.
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 1, "next_source_id": 1, "next_mod_set_id": 1,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [], "slice_sets": [], "mod_sets": [], "slots": []}}
notes = [{"nanotick": 0, "duration": Q * 2, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": 1}]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "d"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY

export DAW_UI_SHM_NAME="/defsnd_$$" DAW_PROJECT_DIR="$TMP"
( cd "$BUILD" && ./daw_engine --project d --run-seconds 20 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 160
grep -q '"event":"project.load"' "$TMP/eng.log" 2>/dev/null || \
  fail "the engine never loaded its project — see $TMP/eng.log"

# THE ONLY COMMAND. No sampler-env, deliberately: sending one would set the very thing whose
# default is under test, and the check would pass on the command rather than on the default.
"$CLI" do sampler-load --track 0 --file s.wav --root 60 >/dev/null 2>&1
sleep 1.2
"$CLI" do save loaded >/dev/null 2>&1
sleep 1.5
kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
[ -f "$TMP/loaded.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"

# The default really is the one under test, not something the load happened to author.
python3 - "$TMP/loaded.uniproj.json" <<'PYC' || fail "the loaded project does not carry the
        default amp envelope, so this check is measuring something else"
import json, sys
d = json.load(open(sys.argv[1]))
for t in d["tracks"]:
    for dev in t["device_chain"]:
        for ms in dev.get("sampler", {}).get("mod_sets", []):
            for m in ms.get("modulators", []):
                if m.get("target") == 0 and m.get("kind") == 0:
                    pts = m.get("points", [])
                    print("  default amp envelope: %d points, depth %s, apply %s, sustain loop %s..%s"
                          % (len(pts), m.get("depth_milli"), m.get("apply"),
                             m.get("sustain_loop_start"), m.get("sustain_loop_end")))
                    raise SystemExit(0)
raise SystemExit(1)
PYC

# A SECOND PROJECT with an EXPLICIT full-level envelope, as the reference the default is compared
# against. Without it "audible" is the only assertion available, and a default at a tenth of the
# right level would pass it.
python3 - "$TMP/ref.uniproj.json" "$TMP/loaded.uniproj.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[2]))
for t in d["tracks"]:
    for dev in t["device_chain"]:
        s = dev.get("sampler")
        if not s:
            continue
        for ms in s.get("mod_sets", []):
            # An explicit instant-on, full-sustain envelope with a long release: the same thing
            # the default is TRYING to be, written the way a fixture would.
            ms["modulators"] = [{
                "id": 1, "target": 0, "kind": 0, "depth_milli": 1000, "apply": 1,
                "rate_milli": 1000, "time_base": 0, "editor": 0,
                "lfo_frequency_hz": 1.0, "lfo_depth": 1.0, "lfo_bias": 0.0,
                "lfo_phase_offset": 0.0,
                "points": [{"t": 0, "v": 1000, "tension": 0, "flags": 0},
                           {"t": 4000000, "v": 1000, "tension": 0, "flags": 0}],
                "sustain_loop_start": 1, "sustain_loop_end": 1,
                "release_loop_start": 255, "release_loop_end": 255,
                "loop_mode": 1, "release_fade": 0}]
json.dump(d, open(sys.argv[1], "w"))
PY

render() {  # render <project> <name>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/defsnd_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 4 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

peak() {  # peak <name> <fromSec> <toSec>
  python3 - "$TMP/$1.wav" "$2" "$3" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
a, b = int(sr * float(sys.argv[2])), int(sr * float(sys.argv[3]))
seg = [abs(s[i * ch]) for i in range(max(a, 0), min(b, n))]
print(max(seg) if seg else 0)
PY
}

render loaded def
render ref ref
DEF_EARLY="$(peak def 0.0 0.05)"
DEF_LATE="$(peak def 0.30 0.80)"
REF_LATE="$(peak ref 0.30 0.80)"
echo "  default envelope: $DEF_EARLY early, $DEF_LATE late"
echo "  explicit full:    $REF_LATE late"

[ "${REF_LATE:-0}" -gt 2000 ] || \
  fail "the REFERENCE render with an explicit full-level envelope is silent late in the note
        (peak ${REF_LATE:-0}), so the fixture cannot sustain anything and the comparison below
        would be meaningless"

# ---- SOUNDS.
[ "${DEF_EARLY:-0}" -gt 1000 ] || \
  fail "a sampler built entirely by commands is SILENT (peak ${DEF_EARLY:-0} in the first 50 ms).
        No envelope was sent, so the slot uses the mod set sampler-load mints — and if that
        default produces no level, the whole chop workflow is mute while every structural fact
        about it stays correct"

# ---- SUSTAINS. Past the 5 ms release, so this is the sustain loop holding and not a blip.
[ "${DEF_LATE:-0}" -gt 1000 ] || \
  fail "the default envelope sounded at the start (peak $DEF_EARLY) and is silent by 0.3 s
        (peak ${DEF_LATE:-0}). Its release is 5 ms and its sustain loop is pinned to point 2, so
        a decay to nothing means the loop is not holding — three of its four points share t=0 and
        a runner that lands on the first of them holds level 0"

# ---- FULL. Within ~2x of an explicit full-level envelope.
python3 -c "
d, r = ${DEF_LATE:-0}, ${REF_LATE:-0}
raise SystemExit(0 if r > 0 and d * 2 >= r else 1)" || \
  fail "the default envelope sustains at $DEF_LATE where an explicit full-level one gives
        $REF_LATE — audible, and at a fraction of the level. A default that is quiet is a
        different defect from a default that is silent and needs saying separately"

# =================================================================================================
# THE LIVE PATH — and this is the half that could not fail before.
#
# Everything above renders the SAVED project. That round-trips the envelope through
# deserialization, which calls repairEnvShape... and repairEnvShape is what makes the default
# envelope playable at all. makeAdsr(0, 0, 1000, 5000) puts three of its four points at t=0, and
# for a long time only the LOAD path and the SetEnvelope command path nudged them apart. The mint
# did not. So a sampler built by commands and played WITHOUT a save ran an envelope whose runner
# held the first of three coincident points, at v=0, and was silent.
#
# This check asserted "a sampler made only by commands sounds" and PASSED throughout, because
# saving and rendering was the one thing that repaired the shape before anything was heard. The
# web-UI agent, playing live, heard silence and was right; "it works on mine" was an artifact of
# this fixture, not a fact about the engine.
#
# So the property is measured where the user is: a live engine, captured off the device, with no
# save between the load and the sound.
#
# WITH A POSITIVE CONTROL, because a silent capture is far more often the harness than the engine
# — capture starts at device-start rather than engine-start, and a mistimed play is indistinguish-
# able from a mute kit. The SAME live path with an explicit envelope must be loud. If both are
# silent the harness is broken and this says so instead of blaming the default.
# =================================================================================================

live_run() {  # live_run <name> <sendEnv 0|1>
  local name="$1" sendenv="$2"
  local shm="/defsnd_live_${$}_$name"
  # THE ENGINE MUST EXIT ON ITS OWN. The capture wav is written during clean shutdown, after the
  # audio device is stopped and the buffer is quiescent (see audio.capture_written) — so killing
  # the engine produces no file at all, which reads as "silent" and is not.
  #
  # LIFETIME COMFORTABLY EXCEEDS EVERYTHING. Boot wait is 10 s against a 22 s life; a wait budget
  # longer than --run-seconds means the script can be waiting for an engine that already exited
  # and then reports "never loaded", which is a statement about the harness. See task #106.
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$shm" \
      DAW_CAPTURE_WAV="$TMP/$name.wav" DAW_CAPTURE_SECONDS=6 \
      ./daw_engine --project d --run-seconds 22 >"$TMP/$name.log" 2>&1 ) &
  ENG=$!
  wait_for_boot "$TMP/$name.log" "$ENG" 40
  grep -q '"event":"project.load"' "$TMP/$name.log" 2>/dev/null || \
    fail "the '$name' engine never loaded — see $TMP/$name.log"
  lcli() { env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="$shm" "$CLI" "$@"; }
  lcli do sampler-load --track 0 --file s.wav --root 60 >/dev/null 2>&1
  sleep 1.5
  if [ "$sendenv" = "1" ]; then
    lcli do sampler-env --track 0 --device 1 --amp \
      --attack 0 --decay 0 --sustain 1000 --release 5000 >/dev/null 2>&1
    sleep 0.5
  fi
  lcli do play --force >/dev/null 2>&1 || true
  wait "$ENG" 2>/dev/null; ENG=""
  grep -q '"event":"audio.capture_written"' "$TMP/$name.log" 2>/dev/null || \
    fail "the '$name' run never wrote its capture — see $TMP/$name.log. The engine did not shut
        down cleanly, so this says nothing about the kit"
  [ -s "$TMP/$name.wav" ] || fail "the '$name' live run captured no audio file. That is the
        harness, not the kit — and specifically:
        $(capture_diagnosis "$TMP/$name.log")
        Full log: $TMP/$name.log"
}

live_run livedef 0
live_run liveref 1
LIVE_DEF="$(peak livedef 0.0 6.0)"
LIVE_REF="$(peak liveref 0.0 6.0)"
echo "  live, no envelope sent:   peak $LIVE_DEF"
echo "  live, explicit envelope:  peak $LIVE_REF"

# ---- THE POSITIVE CONTROL FIRST. If this is silent nothing below means anything.
[ "${LIVE_REF:-0}" -gt 2000 ] || \
  fail "the live capture with an EXPLICIT envelope is silent (peak ${LIVE_REF:-0}). The harness
        did not capture the transport at all — wrong capture window, the play never landed, or
        the device never started — so it cannot say anything about the default either way"

# ---- LIVE SOUNDS. The property the whole check is named for, finally measured where it lives.
[ "${LIVE_DEF:-0}" -gt 2000 ] || \
  fail "a sampler built entirely by commands is SILENT WHEN PLAYED LIVE (peak ${LIVE_DEF:-0}),
        while the same path with an explicit envelope gives $LIVE_REF. No envelope was sent, so
        the slot uses the mod set sampler-load mints — and makeAdsr(0, 0, ...) puts three points
        at t=0, so unless the mint produces strictly increasing times the runner holds the first
        of them at level 0. Saving and reloading hides this, because deserialization repairs the
        shape; this is the live path, where nobody repairs anything"

# ---- AND AT FULL LEVEL, not merely audible.
python3 -c "
d, r = ${LIVE_DEF:-0}, ${LIVE_REF:-0}
raise SystemExit(0 if r > 0 and d * 2 >= r else 1)" || \
  fail "live, the default envelope peaks at $LIVE_DEF where an explicit full-level one gives
        $LIVE_REF — audible, and at a fraction of the level"

echo "sampler_default_sound_check: PASS — a sampler made only by commands sounds, sustains, and"
echo "                             is at full level with no envelope sent, both rendered from a"
echo "                             save AND played live with no save in between"
