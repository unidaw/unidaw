#!/usr/bin/env python3
"""Generate the audio fixture the waveform work is tested against.

    python3 tools/gen_audio_fixture.py     # writes both files under presets/audio/

Writes TWO files:
    waveform_probe.wav          mono, the 8 sections below
    waveform_probe_stereo.wav   the same signal on the left, NEGATED on the right

No project in presets/projects/ contains an audio clip, so there has never been a
waveform to draw or a peak computation to check. This makes one — and makes it a
TEST, not a noise.

Every section has a peak amplitude that is known in advance, so any peak
implementation can be checked numerically rather than by eye:

    0.0-1.0s  silence                    min  0.00  max  0.00
    1.0-2.0s  440 Hz sine, full scale    min -1.00  max  1.00
    2.0-3.0s  440 Hz sine, quarter       min -0.25  max  0.25
    3.0-4.0s  sine under a 0->1 ramp     grows linearly across the second
    4.0-5.0s  impulse train, 10/s        min -1.00  max  1.00 in the buckets that
                                         contain one, 0 in the ones that do not
    5.0-6.0s  DC +0.5                    min  0.50  max  0.50
    6.0-7.0s  alternating +/-1           min -1.00  max  1.00
    7.0-8.0s  silence                    min  0.00  max  0.00

Three of those sections exist to catch specific mistakes, and they are the reason
this file is worth committing rather than generating a sine and moving on:

  - THE IMPULSE TRAIN catches averaging. A single full-scale sample inside a
    1024-sample bucket has a mean near zero and an RMS near 1/32. Any peak
    implementation that averages, or that samples every Nth frame instead of
    scanning all of them, draws this second as silence — and a transient that
    vanishes from the display is the single most misleading thing a waveform can
    do, because it is exactly what you zoom in to find.

  - THE ALTERNATING SECTION catches storing one magnitude per bucket instead of a
    min AND a max. Its mean is 0 and its |peak| is 1; a single-value scheme has to
    choose, and both choices are wrong. It also catches downsampling that takes
    max(|x|) and mirrors it, which draws a symmetric shape for asymmetric audio.

  - THE DC SECTION catches the mirror assumption from the other side: a waveform
    drawn as +/-|peak| around a centre line shows this as a band straddling zero,
    when the truth is a solid block entirely above it. Real audio has DC offsets;
    this is how you find out whether the renderer can express one.

  - THE STEREO FILE catches a mono downmix, which is not merely lossy but FALSE.
    Its channels sum to exactly zero at every frame, so a waveform built from a
    downmix draws all eight seconds — including the full-scale sine — as a flat
    line, while the file is as loud as a file can be. Nothing about that failure
    looks like a bug in the peak code; it looks like a quiet recording.

16-bit PCM at 44100 Hz, 8 seconds = 4 bars at 120 BPM, so the section boundaries
land on beats and are easy to point at in the arrangement.

Written with the standard library alone: this is a fixture generator, and adding
a dependency to produce a WAV whose format is 44 bytes of header would be a poor
trade.
"""

import math
import struct
import os
import sys

RATE = 44100
SECONDS = 8
BITS = 16

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MONO = os.path.join(ROOT, "presets", "audio", "waveform_probe.wav")
STEREO = os.path.join(ROOT, "presets", "audio", "waveform_probe_stereo.wav")


def sample_at(n):
    """The fixture's value at frame `n`, in -1.0..1.0.

    A pure function of the frame index so the file is bit-identical on every
    machine and every run. A fixture that differs between runs turns a real
    regression into a coin flip.
    """
    t = n / RATE
    section = int(t)
    phase = 2.0 * math.pi * 440.0 * t

    if section == 0 or section == 7:
        return 0.0
    if section == 1:
        return math.sin(phase)
    if section == 2:
        return 0.25 * math.sin(phase)
    if section == 3:
        return (t - 3.0) * math.sin(phase)          # 0 -> 1 ramp across the second
    if section == 4:
        # One full-scale sample every 4410 frames: ten impulses, alternating sign
        # so the section exercises both the min and the max side.
        into = n - 4 * RATE
        if into % 4410 == 0:
            return 1.0 if (into // 4410) % 2 == 0 else -1.0
        return 0.0
    if section == 5:
        return 0.5                                   # DC
    if section == 6:
        return 1.0 if n % 2 == 0 else -1.0           # alternating at Nyquist
    return 0.0


def write_wav(path, channels, frames, value):
    """`value(n, c)` gives the sample for frame n, channel c, in -1.0..1.0."""
    # 32767, not 32768: -32768 is representable and +32768 is not, so scaling by
    # 32768 clips the positive full-scale sine by one LSB and makes the file's
    # actual peak asymmetric — which would then show up in the very peak values
    # this fixture exists to verify.
    peak = 32767
    body = bytearray()
    for n in range(frames):
        for c in range(channels):
            v = value(n, c)
            v = -1.0 if v < -1.0 else (1.0 if v > 1.0 else v)
            body += struct.pack("<h", int(round(v * peak)))

    byte_rate = RATE * channels * BITS // 8
    block_align = channels * BITS // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, RATE,
                                    byte_rate, block_align, BITS)
    header += b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(header)
        f.write(body)
    print(f"wrote {path}")
    print(f"  {frames} frames, {SECONDS}s, {RATE} Hz, {channels}ch, {BITS}-bit, "
          f"{len(header) + len(body)} bytes")


def main():
    os.makedirs(os.path.dirname(MONO), exist_ok=True)
    frames = RATE * SECONDS
    write_wav(MONO, 1, frames, lambda n, c: sample_at(n))

    # STEREO: right is the exact negation of left.
    #
    # This one file turns "a mono downmix is FALSE, not merely lossy" into a
    # numeric assertion. (L + R) / 2 is identically zero for every frame, so a
    # waveform built from a downmix draws all eight seconds — including the
    # full-scale sine — as a flat line at zero, while the file is as loud as a
    # file can be. Nothing about that failure looks like a bug in the peak code;
    # it looks like a quiet recording, which is why it needs a fixture rather
    # than a comment.
    write_wav(STEREO, 2, frames, lambda n, c: sample_at(n) if c == 0 else -sample_at(n))
    # Print the expected peaks per second, which is what a peak implementation
    # should be checked against.
    print("\n  expected min/max per second:")
    for s in range(SECONDS):
        # Seeded from the first sample, NOT from 0.0. Seeding at zero reports the
        # DC section as min 0.000 when every sample in it is +0.5 — the report
        # would then contradict the docstring above it, which is a fine way to
        # cost somebody an afternoon over a fixture that was correct all along.
        lo = hi = sample_at(s * RATE)
        for n in range(s * RATE, (s + 1) * RATE):
            v = sample_at(n)
            lo = min(lo, v)
            hi = max(hi, v)
        print(f"    {s}.0-{s + 1}.0s   min {lo:+.3f}   max {hi:+.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
