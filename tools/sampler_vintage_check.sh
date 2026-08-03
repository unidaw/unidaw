#!/usr/bin/env bash
# VINTAGE IS REACHABLE, INDEPENDENT, PERSISTED, AND THE TWO CONTROLS ARE DIFFERENT SOUNDS.
#
# Bit depth and sample-rate reduction — the SP-1200 / MPC60 character — live on the sampler MOD
# SET, for the reason the filter does: a chopped break is one instrument, and sixteen copies of
# its character is sixteen edits.
#
# WHY THIS MEASURES THE TWO EFFECTS SEPARATELY, and does not merely assert that the render
# CHANGED. "Vintage on sounds different from vintage off" passes on an engine that applied bit
# reduction when asked for rate reduction, or applied one of them twice — the sound is different
# either way and the check would be happy. So each control is identified by the fingerprint only
# IT leaves:
#
#   BIT DEPTH quantises amplitude, so the number of DISTINCT sample values collapses (a sine
#             visits thousands; at 3 bits it visits three) while the signal still moves at
#             audio rate.
#   RATE      is a sample-and-hold, so the number of TRANSITIONS collapses — the signal stands
#             still for runs of frames — while the number of distinct values it visits stays
#             LARGE, because every latched value is a different point on the sine.
#
# Those two go in OPPOSITE directions on the two statistics, so neither can be mistaken for the
# other and neither can be faked by a gain change.
#
# SEVEN PROPERTIES:
#   REACHES     vintage_bits / vintage_rate_hz read back as what was sent
#   INDEPENDENT setting the rate does not clear the bits — the flags in the payload are real.
#               Zero is a LEGAL value for both (it means off), so "leave it alone" cannot be
#               encoded as a zero, and an engine that ignored the flags would silently undo the
#               previous call. This is the omitted-is-not-zero trap in its sharpest form.
#   PERSISTS    both survive a save into the project JSON
#   REFUSED     a bit depth outside 0..16 is refused, not clamped to the nearest legal value
#   AUDIBLE     bits collapse DISTINCT VALUES; rate collapses TRANSITIONS and does NOT collapse
#               distinct values
#   NOT A REDUCTION  a target rate at or above the engine's own holds for one frame, which is no
#               reduction at all, and must render identically to vintage off
#   NO LEAK     voices are pooled, so the hold state must be cleared at note-on: two identical
#               notes, the second reusing the first's voice, must render identically
#
# Rendered OFFLINE for the audible half. No audio device needed.
#   tools/sampler_vintage_check.sh
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
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# A SINE, and that is the whole reason this check can tell the two controls apart. A sine visits
# a new value nearly every frame, so BOTH statistics start high and each control can be seen
# pulling ONE of them down. A sawtooth or a drum hit would already be steppy in places and the
# baseline would be muddy.
python3 - "$TMP/projects/s.wav" <<'PY'
import sys, wave, struct, math
sr = 48000; n = sr
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220 * i / sr))) for i in range(n)))
w.close()
PY

SHM="/vintchk_$$"
export DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP/projects"

# The interactive project carries its slot but NO MOD SETS, which is the state a sampler is in
# before anything touches it. The handler has to mint one (ensureDefaultModSet) or the command
# applies to nothing and reports no_such_mod_set to a log nobody is reading — the exact shape of
# the bug the filter check was written for.
python3 - "$TMP/projects/v.uniproj.json" "$Q" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 1,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
                   "slice_sets": [], "mod_sets": [],
                   "slots": [{"id": 1, "name": "sine", "source_local_id": 1, "slice_id": 0,
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
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "v"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

# `exec`, so $! is the ENGINE rather than the subshell around it — otherwise `kill "$ENG"` reaps
# the subshell and leaves the engine holding the audio device until --run-seconds elapses, or
# forever if it is blocked waiting for a device another engine already has.
( cd "$BUILD" && exec ./daw_engine --project v --run-seconds 45 \
    >"$TMP/projects/eng.log" 2>&1 ) &
ENG=$!
# WAITS FOR THE PROJECT, NOT FOR THE THREADS: "starting threads" is printed before the startup
# project is loaded, and a command sent on that signal is refused into the engine's own log.
wait_for_boot "$TMP/projects/eng.log" "$ENG" 120

kitfield() {  # kitfield <field>
  "$CLI" get sampler-kit --track 0 2>/dev/null | grep -oE "\"$1\": [0-9]+" | head -1 |
    grep -oE '[0-9]+$'
}
waitfield() {  # waitfield <field> <want>
  for _ in $(seq 1 40); do
    [ "$(kitfield "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}

# ---- REACHES. The bit depth arrives on a sampler that had no mod sets at all.
"$CLI" do sampler-vintage --track 0 --bits 3 >/dev/null 2>&1
waitfield vintage_bits 3 || \
  fail "vintage_bits never read back as 3 within 10s, it stayed at $(kitfield vintage_bits).
        Either opcode 91 did not reach the mod set, or it reached a sampler whose modSets vector
        was empty and never entered the loop — the empty-vector trap the filter check found"
echo "  vintage_bits reads back as 3 on a sampler that had no mod sets"

# ---- INDEPENDENT. Now set ONLY the rate. The bit depth must survive it.
#
# This is the assertion with the most teeth in the file. The payload carries a flag per field
# precisely because zero is a legal value for both, so an engine that wrote both fields on every
# command would clear the bits here — and would look completely correct to any check that only
# ever set one of them.
"$CLI" do sampler-vintage --track 0 --rate 3000 >/dev/null 2>&1
waitfield vintage_rate_hz 3000 || \
  fail "vintage_rate_hz never read back as 3000, it stayed at $(kitfield vintage_rate_hz)"
BITS_AFTER="$(kitfield vintage_bits)"
[ "${BITS_AFTER:-0}" = "3" ] || \
  fail "setting the RATE cleared the BIT DEPTH: vintage_bits is ${BITS_AFTER:-0}, was 3. The
        payload carries kSamplerVintageSetBits/SetRate so that each call says which field it is
        about. Zero is a legal value for both — it means OFF — so absence cannot be encoded as a
        zero, and an engine that ignores the flags silently undoes the previous call"
echo "  setting the rate left the bit depth alone: bits 3, rate 3000"

# ---- PERSISTS. Both fields survive into the saved project.
"$CLI" do save v --force >/dev/null 2>&1 || true
SAVED="$TMP/projects/v.uniproj.json"
for _ in $(seq 1 40); do
  grep -q '"bit_depth": *3' "$SAVED" 2>/dev/null && break
  sleep 0.25
done
grep -q '"bit_depth": *3' "$SAVED" 2>/dev/null || \
  fail "bit_depth 3 is not in the saved project. It reached the model and the writer does not
        emit it, so the sound is heard, drawn, and lost on reload"
grep -q '"rate_hz": *3000' "$SAVED" 2>/dev/null || \
  fail "rate_hz 3000 is not in the saved project"
echo "  both fields persist into the saved project"

# ---- REFUSED BY THE ENGINE. The field means 2^n levels, so 17 is a caller with the wrong idea of
# the unit, not a value a hair past 16. Clamping would hand back a sound nobody asked for.
#
# ASSERTED ON THE ENGINE'S REJECTION, not on daw-cli's exit code. This line used to read
# `"$CLI" do ... && fail`, which asserted that the CLI refused — and the CLI had its own copy of
# 0..16, so with the ENGINE's guard deleted this check still passed. The engine's copy is the one
# every producer meets, including the web UI's sidecar writing to the ring directly, so daw-cli
# now checks only that the number fits the payload's byte and the engine judges the range.
"$CLI" do sampler-vintage --track 0 --bits 17 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  grep -q '"event":"sampler.vintage_rejected".*"reason":"bit_depth_out_of_range"' "$TMP/projects/eng.log" && break
  sleep 0.25
done
grep -q '"event":"sampler.vintage_rejected".*"reason":"bit_depth_out_of_range"' "$TMP/projects/eng.log" || \
  fail "a --bits above 16 was not refused by the ENGINE — no sampler.vintage_rejected with
        reason bit_depth_out_of_range in the log"
echo "  a bit depth outside 0..16 is refused by the engine, with a reason"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- THE AUDIBLE HALF, rendered offline with the vintage settings ALREADY IN THE PROJECT.
#
# No command is sent during these renders, deliberately. An offline render does not wait for
# commands to arrive: under `ctest -j8` a render can be past the notes before a command lands,
# and the take comes out clean while the check reports that vintage does nothing. The
# interactive half above proves the COMMAND reaches the model; this proves the MODEL reaches the
# voice. A timing-dependent test of both at once proves neither.
render() {  # render <name> <bitDepth> <rateHz> [noteCount]
  python3 - "$TMP/projects/$1.uniproj.json" "$Q" "$2" "$3" "$1" "${4:-1}" <<'PYP'
import json, sys
out, Q, bits, rate, nm = (sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                          int(sys.argv[4]), sys.argv[5])
COUNT = int(sys.argv[6])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "sine", "source_local_id": 1, "slice_id": 0,
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
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "bit_depth": bits, "rate_hz": rate,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
# ONE BAR APART, so the first note's voice has ENDED and gone back to the pool before the second
# starts. That is what makes the second note a test of voice REUSE rather than of polyphony.
notes = [{"nanotick": i * BAR, "duration": Q * 3, "pitch": 60, "velocity": 110,
          "column": 0, "note_id": i + 1} for i in range(COUNT)]
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * COUNT,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": nm}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR * COUNT, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PYP
  # THE RATE IS PINNED, and that is not cosmetic. Without --sample-rate the offline render adopts
  # the DEFAULT OUTPUT DEVICE's rate, so this check measured whatever was plugged in. It passed
  # for weeks on built-in speakers at 44100 and failed the first time a Bluetooth device made the
  # default 48000 — with no commit involved, and while the engine was correct.
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/vintchk_${$}_$1" DAW_PROJECT_DIR="$TMP/projects" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 5 --block-size 256 \
      --sample-rate "${5:-48000}" \
      >"$TMP/projects/$1.log" 2>&1 ) || fail "the '$1' render exited non-zero"
  [ -s "$TMP/projects/$1.wav" ] || fail "the '$1' render wrote no output"
}

render clean 0 0
render bits3 3 0
render rate3k 0 3000
# THE SAME RATE REDUCTION AT A DIFFERENT ENGINE RATE. Its only job is to prove the verdict below
# does not depend on the rate — the exact property whose absence let this check fail a correct
# engine. 44100/3000 is 14 frames where 48000/3000 is 16, so any assertion that has quietly become
# rate-coupled again will disagree between these two takes.
render rate3k44 0 3000 1 44100
# 65535 is above ANY plausible engine rate, which is the point: the guard is `rateHz <
# sampleRate`, so this must take the no-reduction branch on a 44.1k machine and a 96k one alike.
# Naming the engine's actual rate here would make the check pass or fail on the audio settings.
render ratemax 0 65535

# distinct / transitions / peak, measured INSIDE THE FIRST NOTE. The clip loops every bar, so the
# render is note, silence, note, silence — and a window spanning the silences would count long
# runs of zero as "the signal stood still", which is exactly the statistic rate reduction is
# being judged on. Measuring one note avoids crediting silence to the sample-and-hold.
stats() {  # stats <name> -> "<distinct> <transitions> <peak> <modalRun> <sampleRate>"
  python3 - "$TMP/projects/$1.wav" <<'PY'
import sys, wave, struct, collections
w = wave.open(sys.argv[1], 'rb')
ch, nf, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (nf * ch), w.readframes(nf)); w.close()
lo, hi = int(0.10 * sr), int(0.90 * sr)      # inside the first note, clear of attack and end
x = [s[i * ch] for i in range(lo, min(hi, nf))]
distinct = len(set(x))
trans = sum(1 for i in range(1, len(x)) if x[i] != x[i - 1])
peak = max((abs(v) for v in x), default=0)
# THE MODAL RUN LENGTH: how many frames the signal typically STANDS STILL for. This is the direct
# fingerprint of a sample-and-hold, which latches and then holds for sampleRate/rateHz frames, so
# the commonest run length IS the hold period. A quantiser has no run structure at all — it snaps
# values onto a coarse grid and the signal keeps moving every frame — so its modal run is 1.
runs = collections.Counter()
n = 1
for i in range(1, len(x)):
    if x[i] == x[i - 1]:
        n += 1
    else:
        runs[n] += 1
        n = 1
runs[n] += 1
modal = runs.most_common(1)[0][0] if runs else 0
# REGULARITY: the share of runs that are the modal length, in percent.
#
# This is what separates the two effects, and the presence of runs is NOT. A sample-and-hold
# latches on a CLOCK, so every run is the same length and this lands near 100. Bit depth is
# driven by the WAVEFORM — short runs where the sine is steep, long ones at the peaks — so its
# run lengths scatter and this stays low. Both numbers are dimensionless ratios, so no audio
# setting can flip either of them.
total_runs = sum(runs.values())
regular = (100 * runs[modal] // total_runs) if total_runs else 0
# The sample rate is REPORTED, not assumed: it comes from the take itself, so every assertion
# built on it survives a machine whose audio device runs at a different rate.
print(f"{distinct} {trans} {peak} {modal} {regular} {sr}")
PY
}

read -r C_D C_T C_P C_R C_REG SRATE <<<"$(stats clean)"
read -r B_D B_T B_P B_R B_REG _ <<<"$(stats bits3)"
read -r R_D R_T R_P R_R R_REG _ <<<"$(stats rate3k)"
read -r M_D M_T M_P M_R M_REG _ <<<"$(stats ratemax)"
read -r X_D X_T X_P X_R X_REG XRATE <<<"$(stats rate3k44)"
# What a correct hold is ON THIS TAKE, derived rather than assumed.
HOLD_WANT=$(( SRATE / 3000 ))
echo "  rendered at ${SRATE} Hz, so a 3 kHz sample-and-hold must stand still ${HOLD_WANT} frames"
echo "  distinct/transitions/peak/modal-run/run-regularity inside the first note:"
echo "    vintage off : $C_D / $C_T / $C_P / $C_R / ${C_REG}%"
echo "    3 bits      : $B_D / $B_T / $B_P / $B_R / ${B_REG}%"
echo "    3 kHz       : $R_D / $R_T / $R_P / $R_R / ${R_REG}%"
echo "    rate 65535  : $M_D / $M_T / $M_P / $M_R / ${M_REG}%"
echo "    3 kHz @${XRATE}: $X_D / $X_T / $X_P / $X_R / ${X_REG}%"

# ---- VACUITY GUARDS. Every comparison below is a ratio against the clean take, so if the clean
# take is silent or already steppy they all pass for the wrong reason.
[ "${C_P:-0}" -gt 0 ] || fail "the vintage-off render is silent inside the measured window, so
        every comparison below is vacuous"
[ "${C_D:-0}" -gt 500 ] || fail "the vintage-off render only visits $C_D distinct values, so the
        bit-depth assertion below cannot mean anything. A sine was chosen precisely to avoid this"
[ "${C_T:-0}" -gt 10000 ] || fail "the vintage-off render only makes $C_T transitions, so the
        rate assertion below cannot mean anything"

# ---- AUDIBLE: BIT DEPTH collapses the number of distinct values.
[ "${B_D:-0}" -lt 16 ] || fail "3-bit vintage still visits $B_D distinct values against $C_D
        clean. Three bits is 2^2 = 4 levels either side of zero, so a handful of distinct values
        is the whole signature of the effect. The byte reached the mod set and the voice did not
        quantise with it"

# ---- AUDIBLE: RATE collapses transitions WITHOUT collapsing distinct values. The second half is
# what makes this a test of sample-and-hold rather than of "something changed": a quantiser would
# also reduce transitions, and it would take the distinct count down with it.
python3 -c "raise SystemExit(0 if $R_T * 4 < $C_T else 1)" || \
  fail "rate reduction to 3 kHz made $R_T transitions against $C_T clean — not the collapse a
        sample-and-hold produces. holdFrames is derived at the mod-set boundary as
        sampleRate/rateHz; a holdFrames of 0 or 1 means the per-sample path never held anything"
# THE HOLD ITSELF, measured rather than inferred from a value count.
#
# This assertion used to read "distinct values must stay above 200", on the reasoning that every
# latched value is a different point on the sine. That is only true when the hold period is not a
# whole number of frames. The sine is 220 Hz: at 44100 the hold is 14.7 frames and the latch drifts
# across the waveform, visiting hundreds of values; at 48000 it is exactly 16, the latch phase
# advances by exactly 11/150 of a cycle, and sine symmetry leaves just 75 distinct values. A
# CORRECT sample-and-hold scores 75 there, and the check failed the engine for being right.
#
# It failed the day the machine's audio device came back to life at 48 kHz, because the offline
# render adopts the device's rate. Nothing in the engine had changed. The check's author had
# already spotted this hazard and guarded the ratemax case against naming a rate; this assertion
# was the one that got away.
#
# So measure the MECHANISM. A sample-and-hold stands still for sampleRate/rateHz frames at a time;
# a quantiser snaps values to a grid and keeps moving every frame. The modal run length separates
# those two directly, and is derived from the take's own rate, so no audio setting can flip it.
[ "${C_R:-0}" -le 2 ] || fail "the vintage-off take already stands still for $C_R frames at a
        time, so a hold is not distinguishable from the baseline and the assertion below is
        vacuous. A sine was chosen precisely to keep this at 1"
python3 -c "raise SystemExit(0 if abs($R_R - $HOLD_WANT) <= 1 else 1)" || \
  fail "rate reduction to 3 kHz held for $R_R frames at a time, and a ${SRATE} Hz take needs
        $HOLD_WANT. That is the sample-and-hold either not holding or holding for the wrong
        span — holdFrames is derived at the mod-set boundary as sampleRate/rateHz"
python3 -c "raise SystemExit(0 if $R_REG >= 80 else 1)" || \
  fail "only ${R_REG}% of the 3 kHz take's runs are the modal ${R_R} frames. A sample-and-hold
        latches on a CLOCK, so every run is the same length and this should be near 100%. Runs
        of scattered lengths mean the hold is following the WAVEFORM, which is what bit depth
        does — so bit depth is being applied where rate was asked for"
python3 -c "raise SystemExit(0 if $B_REG < 60 else 1)" || \
  fail "${B_REG}% of the 3-BIT take's runs are the same length, which is a CLOCK's signature,
        not a quantiser's. Bit depth is driven by the waveform — short runs where the sine is
        steep, long ones at its peaks — so its run lengths must scatter. This regularity means
        rate reduction is being applied where bit depth was asked for, the same confusion in
        the other direction"
python3 -c "raise SystemExit(0 if $R_D > 20 * $B_D else 1)" || \
  fail "the 3 kHz take ($R_D distinct) and the 3-bit take ($B_D distinct) leave the same
        fingerprint, so the two controls are not two different effects"

# ---- THE VERDICT MUST NOT DEPEND ON THE ENGINE'S RATE, and this is the assertion that says so.
#
# The same 3 kHz reduction rendered at a different engine rate must reach the same conclusion: a
# clock-regular hold of that take's own sampleRate/3000 frames. The DISTINCT COUNT deliberately is
# NOT compared between the two — it legitimately differs (75 at 48000, several hundred at 44100,
# because a non-integer hold drifts across the waveform), and asserting on it is precisely the
# mistake that made this check fail a correct engine.
# A MEASUREMENT THAT CANNOT FAIL IS NOT A MEASUREMENT. If --sample-rate were silently ignored,
# both takes would come out at the device's rate and this section would compare a render with
# itself — passing forever while proving nothing. The rates must actually differ.
[ "$XRATE" != "$SRATE" ] || \
  fail "both takes rendered at ${SRATE} Hz, so the rate-independence check below is comparing a
        take with itself. --sample-rate is being ignored, which is exactly the silent fallback
        that flag was written to refuse"
XHOLD_WANT=$(( XRATE / 3000 ))
python3 -c "raise SystemExit(0 if abs($X_R - $XHOLD_WANT) <= 1 else 1)" || \
  fail "at ${XRATE} Hz the 3 kHz reduction held for $X_R frames and needs $XHOLD_WANT. The hold
        is not tracking the engine's rate, so it is not sampleRate/rateHz"
python3 -c "raise SystemExit(0 if $X_REG >= 80 else 1)" || \
  fail "at ${XRATE} Hz only ${X_REG}% of runs are the modal length, against ${R_REG}% at the
        pinned rate. The same effect must look the same at both, and a verdict that changes with
        the engine's rate is the defect this pair of renders exists to catch"

# ---- NOT A REDUCTION. A target rate at or above the engine's own holds for one frame, which is
# no reduction at all, so this must be indistinguishable from vintage off.
[ "$M_D" = "$C_D" ] && [ "$M_T" = "$C_T" ] || \
  fail "a target rate of 65535 Hz, at or above the engine's own, changed the render:
        $M_D/$M_T against $C_D/$C_T clean. The guard is 'rateHz > 0 && rateHz < sampleRate', so
        this must take the no-reduction branch — a hold of one frame is not a reduction"

# ---- THE HOLD DOES NOT LEAK BETWEEN NOTES. Voices are POOLED and reused, so the per-voice hold
# counter and its latched sample must be cleared at note-on exactly as the filter states are
# (start() calls filtL_.reset() for precisely this reason). If they are not, a voice that played
# a note with rate reduction on begins the NEXT note MID-HOLD: its first output is the previous
# note's last latched sample, and its whole staircase is offset by however many frames were left
# over.
#
# WHY THIS DESERVES ITS OWN PROPERTY. The failure is HISTORY-DEPENDENT — the same note sounds
# different depending on what the voice played before it — so a bounce stops being a function of
# the project. Every other assertion in this file renders ONE note and cannot see it. The
# determinism check cannot see it either: its fixture has vintage off.
render twice 0 3000 2
render twiceoff 0 0 2

# The two notes are one BAR apart — 2s at 120bpm — and each is compared to the other over the
# same window inside it. Identical notes through identical processing must produce identical
# samples; anything else is state carried across the note boundary.
notepair() {  # notepair <name> -> "<differing> <compared> <peak>"
  python3 - "$TMP/projects/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, nf, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (nf * ch), w.readframes(nf)); w.close()
bar = int(2.0 * sr)                       # one bar at 120bpm
lo, hi = int(0.05 * sr), int(0.95 * sr)   # inside the note, clear of its edges
a, b = [], []
for i in range(lo, hi):
    j = bar + i
    if j >= nf:
        break
    a.append(s[i * ch]); b.append(s[j * ch])
diff = sum(1 for i in range(len(a)) if a[i] != b[i])
print(f"{diff} {len(a)} {max((abs(v) for v in a), default=0)}")
PY
}
read -r L_DIFF L_N L_PEAK <<<"$(notepair twice)"
read -r O_DIFF O_N O_PEAK <<<"$(notepair twiceoff)"
echo "  two identical notes, second reusing the first's voice:"
echo "    rate 3 kHz  : $L_DIFF of $L_N samples differ (peak $L_PEAK)"
echo "    vintage off : $O_DIFF of $O_N samples differ (peak $O_PEAK)"

[ "${L_N:-0}" -gt 10000 ] || fail "only $L_N samples were compared between the two notes, so this
        assertion is vacuous — the render is shorter than the window this check reads"
[ "${L_PEAK:-0}" -gt 0 ] || fail "the first note is silent in the compared window, so two silences
        are being compared and the assertion cannot fail"
# THE CONTROL FIRST, and it is a harness check rather than a claim about vintage: with vintage
# OFF the two notes are trivially identical, so a non-zero count here means the two windows are
# not aligned on the notes and the real assertion below would be failing for the wrong reason.
[ "${O_DIFF:-1}" = "0" ] || \
  fail "with vintage OFF the two identical notes already differ in $O_DIFF of $O_N samples. That
        is not a vintage defect — the comparison windows are not landing on the same offset within
        each note, so the assertion below would be measuring misalignment"
[ "${L_DIFF:-1}" = "0" ] || \
  fail "the second note differs from the first in $L_DIFF of $L_N samples with only rate
        reduction on. The voice was reused and its hold state was NOT cleared at note-on, so the
        second note starts mid-hold holding the FIRST note's last latched sample and runs its
        staircase offset by the leftover frames. start() resets filtL_/filtR_ for exactly this
        reason; holdCount_/held_ must be reset with them, or the same note sounds different
        depending on what played before it and a bounce is no longer a function of the project"
echo "  the hold does not leak: two identical notes render identically"

echo "sampler_vintage_check: PASS — bits and rate each reach the voice, they are two different
  effects, and neither leaks across a reused voice"
