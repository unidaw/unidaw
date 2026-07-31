#!/usr/bin/env bash
# THE FILTER CAN BE TURNED ON, AND TURNING IT ON IS AUDIBLE.
#
# modSet.filterType was READ at the kit publish site and WRITTEN NOWHERE. Not by a command, not by
# a default, not by anything but loading a project file that already said so. The whole filter
# section of the sampler — two of its five modulation targets, cutoff and resonance — was
# reachable only by hand-editing JSON.
#
# That made every cutoff envelope built through the CLI or the UI inert BY CONSTRUCTION. The
# modulator was created, it saved, it reloaded, it published its bit in modMask, and it modulated
# a filter that was off. The web-UI agent found it from the outside: they drew the inert state
# honestly instead of hiding it, then could not construct a live one no matter which flags they
# tried, and said so rather than shipping a check that passed for the wrong reason.
#
# WHY THIS MEASURES AUDIO AND NOT THE READ-BACK. Asserting that filter_type comes back as 1 would
# pass on an engine that stored the byte and never gave it to the voice — which is exactly the
# failure being fixed, one layer along. The read-back is checked too, but the assertion that
# matters is that a bright sample gets DARKER.
#
# FOUR PROPERTIES:
#   REACHES     filter_type comes back as what was sent, and modMask carries the cutoff bit
#   AUDIBLE     with the filter on and cutoff low, high-frequency energy drops sharply
#   BASE VALUE  --cutoff moves it: a low cutoff is darker than a high one
#   REFUSED     a filter type outside the enumeration is rejected, not clamped to the nearest
#
# Rendered OFFLINE. No audio device needed.
#   tools/sampler_filter_check.sh
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
mkdir -p "$TMP/projects"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# A SAWTOOTH, because a low-pass has to have something to remove. A sine at the fundamental would
# pass a low-pass unchanged and this whole check would compare two identical files — the trap that
# an earlier filter fixture in this repo actually fell into.
python3 - "$TMP/projects/saw.wav" <<'PY'
import sys, wave, struct
sr = 48000
n = sr
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
period = sr // 220
w.writeframes(b''.join(
    struct.pack('<h', int(12000 * (2.0 * (i % period) / period - 1.0))) for i in range(n)))
w.close()
PY

python3 - "$TMP/projects/f.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
# NO MOD SETS AT ALL, deliberately. This is the state a sampler is in before anything touches it,
# and it is the state in which sampler-env used to apply to nothing at all.
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 1,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   # THE SAMPLE IS IN THE PROJECT, not loaded by a command, so the only thing this
                   # check depends on the CLI for is the thing under test. An offline render does
                   # not wait for commands to arrive: when the load came over the wire too, the
                   # render was past the notes before the slot existed and all three takes came
                   # out silent — which the vacuity guard below caught rather than letting three
                   # silences compare equal.
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   # NO MOD SETS. That is the state a sampler is in before anything touches it,
                   # and the state in which sampler-env used to apply to nothing at all.
                   "mod_sets": [],
                   "slots": [{"id": 1, "name": "saw", "source_local_id": 1, "slice_id": 0,
                              "start_frame": 0, "end_frame": 0,
                              "loop_start_frame": 0, "loop_end_frame": 0,
                              "loop_xfade_frames": 0, "loop_mode": 0, "sustain_loop": 0,
                              "key_low": 0, "key_high": 127, "root_key": 60,
                              "pitch_track_milli": 1000, "tune_cents": 0,
                              "vel_low": 0, "vel_high": 127, "layer_group": 0,
                              "select_mode": 0, "gate": 1, "reverse": 0,
                              "gain_millibels": 0, "pan_thousandths": 0, "voice_group": 0,
                              "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
                              "mod_set_id": 1, "output_stem": 0, "quality": 1}]}}
# THE NOTES START AT BAR 2, NOT AT TICK 0, and that is not arbitrary. The filter command has to
# be sent while the render is running, and editing a sampler WHILE VOICES ARE SOUNDING is a
# confirmed heap-use-after-free (task #97: the retired snapshot is freed while voices still hold
# raw pointers into its envelopes). Starting the notes after the edit lands keeps this check
# measuring what it is about — whether the filter is audible — instead of colliding with an
# unrelated crash and reporting it as a filter failure.
notes = [{"nanotick": BAR + i * Q, "duration": Q - Q // 8, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": i + 1} for i in range(4)]
clip = {"id": 1, "name": "p", "length": BAR * 3, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 3,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "f"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY
cp "$TMP/projects/saw.wav" "$TMP/projects/s.wav"

SHM="/filtchk_$$"
export DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP/projects"

up() {  # bring the engine up and wait for it
  ( cd "$BUILD" && ./daw_engine --project f --run-seconds 30 >"$TMP/projects/eng.log" 2>&1 ) &
  ENG=$!
  # WAITS FOR THE PROJECT, NOT FOR THE THREADS. "starting threads" is printed before the startup
  # project has been loaded, so a command sent on that signal can arrive at an engine whose tracks
  # do not exist yet — and it is REFUSED, with a reason, into the engine's log where nothing here
  # was looking. With two tracks the load takes longer and the race became reliable: every
  # sampler-load came back "no_sampler_device" and the check reported that the read-back returned
  # nothing, which was true and about the wrong thing.
  wait_for_boot "$TMP/projects/eng.log" "$ENG" 120
}
up

# NO sampler-load HERE, and that is the point. sampler-load mints a default mod set of its own
# (daw_engine_main.cpp, "if (d.sampler.modSets.empty())"), so a check that loads first is not
# testing a sampler with no mod sets — it is testing one that just got given some. The first
# version of this file did exactly that and printed "envelope on a sampler with no mod sets"
# about a sampler that had one. The project below carries its slot already, so what arrives here
# genuinely has an empty modSets vector.

kitfield() {  # kitfield <field>
  "$CLI" get sampler-kit --track 0 2>/dev/null | grep -oE "\"$1\": [0-9]+" | head -1 |
    grep -oE '[0-9]+$'
}

# WAITS FOR A VALUE RATHER THAN SLEEPING A FIXED TIME, and the reason is a defect and not
# flakiness. kit_version is written every publish cycle from the MODEL counter while the answer
# slot is filled at request-service time from the snapshot, so the two are read at different
# moments: a read about a second after an edit returns the NEW version alongside the OLD content.
# Read again and the content is right with no version change. See task #96.
#
# Polling here would hide that if it were the thing under test. It is not — this check is about
# whether the filter can be turned on at all — so the wait is bounded and the timeout says which
# field never arrived rather than reporting a generic failure.
waitfield() {  # waitfield <field> <want> <whatFailed>
  for _ in $(seq 1 40); do
    [ "$(kitfield "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}

# ---- The envelope, on a sampler that has NO mod sets. This used to apply to nothing and report
# "no_such_mod_set" to the engine's own log while the CLI printed {"sent": ...}.
"$CLI" do sampler-env --track 0 --target cutoff --attack 200000 --decay 400000 \
  --sustain 1000 --release 100000 --depth 900 >/dev/null 2>&1
MASK=""
for _ in $(seq 1 40); do
  MASK="$(kitfield mod_mask)"
  [ "${MASK:-0}" -ge 64 ] && break
  sleep 0.25
done
[ "${MASK:-0}" -ge 64 ] || \
  fail "the cutoff envelope did not reach a sampler that had no mod sets: modMask ${MASK:-0}, and
        bit 6 is the cutoff envelope. The handlers iterate modSets looking for a match and an
        empty vector never enters the loop, so the command applied to nothing"
echo "  envelope on a sampler with no mod sets: modMask $MASK"

# ---- REACHES. The filter type comes back as what was sent.
"$CLI" do sampler-filter --track 0 --type lp12 --cutoff 120 >/dev/null 2>&1
waitfield filter_type 1 || \
  fail "filter_type never read back as 1 (lp12) within 10s, it stayed at $(kitfield filter_type).
        Before opcode 86 nothing in the engine wrote this field at all: it was read at the kit
        publish site and written nowhere, so the only way to turn a sampler's filter on was to
        hand-edit the project JSON"
echo "  filter_type reads back as 1"

# ---- REFUSED. An enumeration is not a continuous control: 7 is a caller with the wrong idea of
# the encoding, not a value a hair past BP.
"$CLI" do sampler-filter --track 0 --type bogus >/dev/null 2>&1 && \
  fail "a --type outside the enumeration was accepted"
echo "  a --type outside the enumeration is refused by the CLI"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- AUDIBLE, and the BASE VALUE moves it. Three renders of the same project, differing only in
# the filter command sent beforehand. Measured as high-frequency energy, because that is what a
# low-pass is for.
# render <name> <filterType> <cutoffMilli>  — writes a project with the filter ALREADY SET and
# renders it. No command is sent during the render, deliberately.
#
# It used to send sampler-filter after a fixed sleep, and an offline render does not wait for
# commands: under `ctest -j8` the render was past the notes before the command landed, so the
# "dark" take came out bright and the check reported that cutoff 100 and cutoff 900 sound the
# same. A real failure message about a fixture race — the exact thing this suite keeps catching
# in its own fixtures, this time in one I had just written.
#
# THE TWO HALVES PROVE THE TWO LINKS. The interactive phase above proves the COMMAND reaches the
# model, by reading filter_type back. This proves the MODEL reaches the voice, by rendering. A
# timing-dependent test of both at once proved neither reliably.
render() {  # render <name> <filterType> <cutoffMilli>
  python3 - "$TMP/projects/$1.uniproj.json" "$Q" "$2" "$3" <<'PYP'
import json, sys
out, Q, ftype, cutoff = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "saw", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 1000, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": ftype,
                                 "cutoff_milli": cutoff, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
notes = [{"nanotick": i * Q, "duration": Q - Q // 8, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": i + 1} for i in range(4)]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": sys.argv[1]}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PYP
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/filtchk_${$}_$1" DAW_PROJECT_DIR="$TMP/projects" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 5 --block-size 256 \
      >"$TMP/projects/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero"
  [ -s "$TMP/projects/$1.wav" ] || fail "the '$1' render wrote no output"
}

render off 0 1000
render dark 1 100
render bright 1 900

hf() {  # high-frequency energy of a render
  python3 - "$TMP/projects/$1.wav" <<'PY'
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
mono = [s[i * ch] / 32768.0 for i in range(n)]
# A crude high-pass: successive differences. Enough to separate "the harmonics are there" from
# "they are not", which is the only distinction being made.
#
# SCALED BY A MILLION, NOT A THOUSAND. At 1000x every filtered take truncated to the integer 0
# while the unfiltered one came out as 11, so the comparisons below were dividing by a rounding
# artefact — the check reported "a cutoff of 100 and a cutoff of 900 sound the same" about two
# takes whose peaks were plainly different. A metric with three significant figures of headroom
# is not optional when the assertion is a RATIO.
d = sum((mono[i] - mono[i - 1]) ** 2 for i in range(1, len(mono)))
print(int(1000000 * math.sqrt(d / max(1, len(mono) - 1))))
PY
}

OFF="$(hf off)"; DARK="$(hf dark)"; BRIGHT="$(hf bright)"
echo "  high-frequency energy — filter off: $OFF, cutoff 900: $BRIGHT, cutoff 100: $DARK"

[ "${OFF:-0}" -gt 0 ] || fail "the unfiltered render has no high-frequency content at all, so
        there is nothing for a low-pass to remove and every comparison below is vacuous. A
        sawtooth was chosen precisely to avoid this"

# ---- AUDIBLE.
python3 -c "
raise SystemExit(0 if $DARK * 2 < $OFF else 1)" || \
  fail "turning the filter on changed nothing audible: $DARK against $OFF unfiltered. The byte
        reached the mod set and the voice never used it, which is the same failure one layer along"

# ---- BASE VALUE. --cutoff is not decoration.
python3 -c "
raise SystemExit(0 if $DARK * 2 < $BRIGHT else 1)" || \
  fail "a cutoff of 100 and a cutoff of 900 sound the same ($DARK against $BRIGHT). The type is
        being applied and the base value is not"

echo "sampler_filter_check: PASS — the filter can be turned on, and it is audible when it is"
