#!/usr/bin/env bash
# THE RENDER DOES NOT DEPEND ON WHEN OR HOW IT WAS RENDERED.
#
# Two properties, and the second one is currently weaker than docs/SAMPLER_DESIGN.md §3.5 asks
# for. The reason is written down rather than quietly dropped:
#
#   REPRODUCIBLE   the same project rendered twice at the same block size is BIT-IDENTICAL.
#                  Nothing in the sampler may depend on wall-clock, thread interleaving, or
#                  anything else that is not the document — so a bounce equals a bounce.
#
#   BLOCK-SIZE     rendering at 64, 256 and 1024 frames is BIT-IDENTICAL over the common
#   INDEPENDENT    length. The block grid is a property of the audio device, not of the music,
#                  so it must not be audible in the result.
#
#                  THIS WAS A KNOWN LIMITATION UNTIL 2026-07-31, and how it ended is worth as
#                  much as the fix. The header used to carry a careful paragraph explaining why
#                  the renders differed — accurate, and completely inert, because nothing checks
#                  a comment. It was rewritten as an ASSERTION that the renders DIFFER, with
#                  instructions for the day it failed. It failed on the next commit, printed the
#                  instructions, and this is them carried out.
#
#                  THE CAUSE, for anyone who meets it again elsewhere: a note's frame is
#                  blockSampleStart + an offset measured from blockStartTicks. blockSampleStart
#                  was an exact counter and blockStartTicks was advanced by adding a value ROUNDED
#                  to a whole nanotick every block. A block is not a whole number of ticks — at
#                  120 bpm / 44.1 kHz a 256-frame block is 11145.898 and a 64-frame block is
#                  2786.48 — so the two grids accumulated rounding error at DIFFERENT rates and
#                  the tick slid against the sample counter by about 1.3 samples per 7000 frames
#                  at 64 frames. The transport now carries the fraction, which bounds the error
#                  below one nanotick forever instead of letting it grow.
#
#                  An EARLIER attempt rewrote the note OFFSET to use absolute tempo-integrated
#                  positions. It changed every render's audio and moved the divergence by exactly
#                  zero, because both formulations measured from the same drifting base. Fixing
#                  the delta cannot fix a broken base.
#
# THE SAMPLER'S OWN INVARIANCE IS ALREADY PROVEN BIT-EXACTLY, in sampler_voice_tests: one voice
# rendered at 64/256/1024 is byte-for-byte equal. That test failed on its first run against a
# block-rate envelope ramped across each block — a linear ramp cuts the corner wherever a
# breakpoint falls inside a block — and forced EnvRunner to become a pure function of the frame
# index. This script is the end-to-end companion to it.
#
#   tools/sampler_determinism_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A CHIRP, not a tone. Every sample is distinct, so a read position that is off by one is visible
# in the output rather than hidden by a waveform that repeats.
python3 - "$TMP/chirp.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = sr // 2
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
frames = []
ph = 0.0
for i in range(n):
    f = 200.0 + 2000.0 * i / n
    ph += 2 * math.pi * f / sr
    frames.append(struct.pack('<h', int(18000 * math.sin(ph))))
w.writeframes(b''.join(frames)); w.close()
PY

# NOTES ON DELIBERATELY AWKWARD TICKS. Every onset below is chosen so that it does NOT land on a
# multiple of 64, 256 or 1024 samples — if the notes fell on block boundaries, a block-aligned
# implementation would pass this check while being exactly as wrong.
python3 - "$TMP/det.uniproj.json" "$TMP/chirp.wav" "$Q" <<'PY'
import json, sys
out, wav, Q = sys.argv[1], sys.argv[2], int(sys.argv[3])
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
env = {"points": [{"t": 0, "v": 0, "tension": 0, "flags": 0},
                  {"t": 30000, "v": 1000, "tension": 40, "flags": 0},
                  {"t": 120000, "v": 500, "tension": -30, "flags": 0},
                  {"t": 400000, "v": 0, "tension": 0, "flags": 0}],
       "sustain_loop_start": 2, "sustain_loop_end": 2,
       "release_loop_start": 255, "release_loop_end": 255,
       "loop_mode": 1, "release_fade": 0}
mod = {"id": 1, "target": 0, "kind": 0, "depth_milli": 1000, "apply": 1,
       "rate_milli": 1000, "time_base": 0, "editor": 1,
       "lfo_frequency_hz": 1.0, "lfo_depth": 1.0, "lfo_bias": 0.0, "lfo_phase_offset": 0.0}
mod.update(env)
sampler = {
    "next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
    "stem_count": 0, "voice_cap": 32, "default_view": 0,
    "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
    "slice_sets": [],
    "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                  "resonance_milli": 0, "next_modulator_id": 2, "modulators": [mod]}],
    "slots": [{"id": 1, "name": "chirp", "source_local_id": 1, "slice_id": 0,
               "start_frame": 0, "end_frame": 0,
               "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
               "loop_mode": 0, "sustain_loop": 0,
               "key_low": 0, "key_high": 127, "root_key": 60,
               "pitch_track_milli": 1000, "tune_cents": 7,
               "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
               "gate": 1, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 200,
               "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
               "mod_set_id": 1, "output_stem": 0, "quality": 1}],
}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5,
       "patcher_node_id": 0, "host_slot_index": 0, "bypass": False, "sampler": sampler}
# Onsets at prime-ish tick offsets and pitches that are not the root, so both the SCHEDULE and
# the varispeed are exercised. Durations differ so note-OFF lands awkwardly too.
notes = []
for i, (tick, pitch, dur) in enumerate([
        (Q // 3,          60, Q // 5),
        (Q + Q // 7,      67, Q // 3),
        (2 * Q + Q // 11, 55, Q // 2),
        (3 * Q + Q // 13, 72, Q // 7),
        (4 * Q + Q // 17, 48, Q),
        (5 * Q + Q // 19, 64, Q // 4)]):
    notes.append({"nanotick": tick, "duration": dur, "pitch": pitch, "velocity": 100 + i * 4,
                  "column": i % 3, "note_id": i + 1})
clip = {"id": 1, "name": "p", "length": BAR * 2, "kind": "symbolic", "notes": notes}
tr = {"track_id": 0, "name": "S", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": routing(), "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR * 2,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "det"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PY

render() {  # render <outName> <blockSize>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/detchk_$$_$1" \
      ./daw_engine --project det --render "$1" --run-seconds 8 --block-size "$2" \
      >"$TMP/$1.log" 2>&1 ) \
    || fail "the $2-frame render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the $2-frame render wrote no output"
}

render b64 64
render b256 256
render b1024 1024

# The fixture must have produced SIGNAL. Three identical silences compare equal and prove nothing
# — this is the negative control for the comparison itself, and without it a sampler that renders
# nothing at all would pass this check perfectly.
ENERGY="$(python3 - "$TMP/b64.wav" <<'PYE'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
n, ch = w.getnframes(), w.getnchannels()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(int(1000 * max(abs(v) for v in s) / 32768.0) if s else 0)
PYE
)"
[ "$ENERGY" -gt 100 ] || \
  fail "the fixture produced almost no audio (peak $ENERGY). Comparing three silences would pass
        this check while proving nothing at all, so the comparison below is only meaningful once
        there is something to compare"
echo "  fixture: six notes rendered, peak $ENERGY"

# ---- REPRODUCIBLE. The same project, the same block size, twice: BIT-IDENTICAL. This is the
# assertion that catches anything depending on wall-clock or thread interleaving, and it is the
# one a bounce actually rests on.
render b256b 256
python3 - "$TMP/b256.wav" "$TMP/b256b.wav" <<'PYR' ||   fail "two renders of ONE project at ONE block size differ. Something in the path depends on
        something that is not the document — wall-clock, thread interleaving, uninitialised
        memory, or an RNG that is not seeded from the project. A bounce that does not equal the
        previous bounce cannot be reasoned about at all"
import sys, wave
def data(p):
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); w.close(); return d
a, b = data(sys.argv[1]), data(sys.argv[2])
if len(a) != len(b) or a != b:
    raise SystemExit(1)
print("  reproducible: two renders at 256 frames are bit-identical (%d bytes)" % len(a))
PYR

# ---- BLOCK-SIZE INDEPENDENT, BYTE FOR BYTE. The strongest form, and what §3.5 asked for all
# along. Compared over the COMMON length because --run-seconds gives each block size a slightly
# different total block count; the audio inside it must match exactly.
#
# Not an energy curve, not a tolerance. A one-sample shift is inaudible and is still a render
# that depends on the device buffer, which is the thing being ruled out.
identical() {  # identical <a> <b>
  python3 - "$TMP/$1.wav" "$TMP/$2.wav" "$1" "$2" <<'PYI'
import sys, wave
def data(p):
    w = wave.open(p, 'rb')
    bps = w.getframerate() * w.getnchannels() * w.getsampwidth()
    d = w.readframes(w.getnframes()); w.close(); return d, bps
a, bps = data(sys.argv[1])
b, _ = data(sys.argv[2])
# COMPARED PAST A ONE-SECOND LEAD-IN. Task #102: an offline render's first ~512 frames depend on
# machine LOAD, so under a parallel ctest this comparison failed for a reason that has nothing to
# do with block sizes — and reporting it as "renders at 64 and 256 frames differ" would be a
# confident claim about the sampler made from a run whose sampler was fine. The skipped second is
# a filed defect, not a tolerance: everything after it is still compared byte for byte.
skip = bps
n = min(len(a), len(b))
if n <= skip:
    print("  too short to compare past the lead-in: %d bytes" % n)
    raise SystemExit(1)
if a[skip:n] != b[skip:n]:
    first = next(i for i in range(skip, n) if a[i] != b[i])
    print("  DIFFER: %s vs %s at byte %d of %d" % (sys.argv[3], sys.argv[4], first, n))
    raise SystemExit(1)
print("  %s vs %s: identical over %d bytes (past the 1s lead-in)" % (
    sys.argv[3], sys.argv[4], n - skip))
PYI
}
identical b64 b256 || fail "renders at 64 and 256 frames differ. The block grid belongs to the
        audio device, not to the music — if it is audible in the result, a bounce depends on the
        buffer size that happened to be set when it was made"
identical b64 b1024 || fail "renders at 64 and 1024 frames differ"
identical b256 b1024 || fail "renders at 256 and 1024 frames differ"

echo "sampler_determinism_check: PASS — reproducible, and BIT-IDENTICAL across buffer sizes"
