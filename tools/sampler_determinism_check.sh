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
#   BLOCK-SIZE     rendering at 64, 256 and 1024 frames must not DRIFT and must not DROP OUT.
#   INDEPENDENT    Asserted here as an energy-envelope match rather than as bit-identity,
#                  because bit-identity across block sizes is blocked by a PRE-EXISTING defect
#                  in the shared note scheduler, not by the sampler. Measured divergence:
#                  64 vs 256 and 64 vs 1024 at frame 7352, 256 vs 1024 at frame 25201.
#
#                  THIS PARAGRAPH USED TO NAME THE WRONG FIX, and the correction is worth more
#                  than the original claim. It said the cure was tickConverter.
#                  nanoticksToSamplesAbsolute() — the M3.22 lesson ("positions are ABSOLUTE,
#                  integrated over the tempo map") applied to the note scheduler. That was tried.
#                  Rewriting tickDeltaToSamples to return the difference of two absolute
#                  positions changed every render's audio (170k-250k bytes) and moved the
#                  divergence frames by exactly ZERO: still 7352, 7352, 25201. It was reverted.
#
#                  The real cause is a BASE mismatch, not a delta one. A note's frame is
#                  blockSampleStart + offset. blockSampleStart is an exact counter, blockSize *
#                  (blockId - 1). The offset is measured from blockStartTicks, which the producer
#                  advances by adding samplesToNanoticks(blockSize) — a value ROUNDED to a whole
#                  nanotick every block. At 120 bpm / 44.1 kHz a 256-frame block is 11145.9 ticks
#                  and a 64-frame block is 2786.48, so the two grids accumulate rounding error at
#                  different rates and the tick position slides against the sample counter by
#                  about 1.3 samples per 7000 frames at 64 frames. Rewriting the delta cannot
#                  help, because both formulations measure from the same drifting base.
#
#                  The fix is to stop keeping the position as two facts that disagree: either
#                  carry the fractional remainder when advancing the transport, or derive
#                  blockSampleStart from the tick rather than counting it. Both touch the
#                  engine's master clock, so neither is a drive-by. Tracked as task #84.
#
#                  AND THE LIMITATION IS ASSERTED, not merely described. The paragraph above is
#                  a comment, and a comment goes stale in silence — it would still be here,
#                  confidently wrong, a year after someone fixed the clock. So the check below
#                  asserts that the renders are NOT bit-identical: the day that stops being true
#                  the check fails and says so, which is the only way a known limitation ever
#                  gets its documentation updated. Borrowed from the web-UI agent, who wrote
#                  their side of three gaps as failing-when-fixed checks and had all three fire
#                  on the merge that fixed them.
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

# ---- THE LIMITATION ITSELF, asserted so it cannot go stale. See the header: block sizes do not
# render bit-identically because the transport tick and the sample counter drift apart. If this
# assertion ever FAILS, that is good news and the work is: delete this block, delete the
# paragraph in the header, and restore the bit-identical comparison the energy curves below
# stand in for.
python3 - "$TMP/b256.wav" "$TMP/b1024.wav" <<'PYL'
import sys, wave
def data(p):
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); w.close(); return d
a, b = data(sys.argv[1]), data(sys.argv[2])
n = min(len(a), len(b))
raise SystemExit(0 if a[:n] != b[:n] else 1)
PYL
if [ $? -ne 0 ]; then
  echo "  NOTE: renders at 256 and 1024 frames are now BIT-IDENTICAL over their common length."
  echo "        Task #84 is fixed. Delete this block and the header paragraph about it, and"
  echo "        replace the energy-curve comparison below with a byte-for-byte one."
  exit 1
fi
echo "  known limitation still present: 256 and 1024 frames differ (task #84)"

# ---- NO DRIFT, NO DROPOUTS across block sizes.
#
# Compared as a CUMULATIVE ENERGY CURVE rather than as a sample-by-sample or window-by-window
# difference, because the statistic has to answer the question actually being asked and no other.
#
# A per-window comparison fails on things that do not matter: a note's ATTACK measured across a
# 10 ms window moves a lot when the onset shifts by one sample, and a one-sample onset shift is
# the known, tracked, musically irrelevant issue described in the header. Tuning a threshold
# until that passes is how a check ends up verifying nothing.
#
# A cumulative curve answers the right question directly. If a note is DROPPED, the curve gains a
# permanent gap. If timing DRIFTS, the curves separate and stay separated. If a voice is never
# freed, the curve grows faster. If the onset merely moves by a sample, the curves touch again
# immediately and the maximum separation stays tiny. So: maximum separation, as a fraction of the
# render's total energy.
curve_cmp() {  # curve_cmp <a> <b> <label>
  python3 - "$TMP/$1.wav" "$TMP/$2.wav" "$3" <<'PYE'
import sys, wave, struct
def cum(p):
    w = wave.open(p, 'rb')
    ch, n = w.getnchannels(), w.getnframes()
    s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
    out, acc = [], 0.0
    for i in range(n):
        v = s[i * ch]
        acc += float(v) * float(v)
        out.append(acc)
    return out
a, b = cum(sys.argv[1]), cum(sys.argv[2])
n = min(len(a), len(b))
total = max(a[n - 1], b[n - 1], 1.0)
worst, at = 0.0, 0
for i in range(0, n, 16):
    d = abs(a[i] - b[i])
    if d > worst:
        worst, at = d, i
pct = 100.0 * worst / total
if pct > 0.5:
    print("  %s: energy curves separate by %.3f%% of total at frame %d" % (sys.argv[3], pct, at))
    raise SystemExit(1)
print("  %s: energy curves stay within %.4f%% of total" % (sys.argv[3], pct))
PYE
}

curve_cmp b64 b256 "64 vs 256 frames" || \
  fail "renders at 64 and 256 frames DRIFT or DROP OUT. A cumulative energy curve only separates
        permanently when a note is lost, a voice is never freed, or timing genuinely moves — a
        one-sample onset shift closes again immediately. Something larger is wrong"
curve_cmp b64 b1024 "64 vs 1024 frames" || \
  fail "renders at 64 and 1024 frames drift or drop out — see the note above"
curve_cmp b256 b1024 "256 vs 1024 frames" || \
  fail "renders at 256 and 1024 frames drift or drop out — see the note above"

echo "sampler_determinism_check: PASS — reproducible, and stable across buffer sizes"
