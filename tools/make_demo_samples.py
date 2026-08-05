#!/usr/bin/env python3
"""Generate the musical one-shots the sampler demo needs.

WHY THIS EXISTS. `presets/audio` held exactly two files, both of them
`waveform_probe*.wav` — the peak-pyramid probe asset, stepped level regions for
testing the waveform display. Both are DIGITALLY SILENT for their first second by
construction. That is correct for what they are for and wrong for everything else:
the sampler transposes by resampling, so playing below the slot's root stretches
that silence (an octave down doubles it), and the tracker's default `oct 4` puts
`z` a whole octave below the root of 60 that a freshly loaded slot gets. The very
first gesture anyone performs — load a sample, type a note — therefore produces
two seconds of nothing.

It has cost two wrong bug reports so far: "a note at tick 0 is dropped from the
render" and "a sampler alone on a track starts a voice and reaches no output".
Neither was real. Both were this asset being used as if it were an instrument.

So the fix is not another warning in a runbook. Every sample here has its ATTACK
IN THE FIRST MILLISECOND, which makes it sound immediately at any transposition,
and is tuned so that the default root of 60 is the truth rather than a guess.

Regenerate with:  python3 tools/make_demo_samples.py

Deterministic — no RNG anywhere, so re-running produces byte-identical files and a
rebuild never shows up as a diff.
"""

import math
import struct
import pathlib

RATE = 44100
OUT = pathlib.Path(__file__).resolve().parent.parent / 'presets' / 'audio'


def write_wav(path, samples):
    """16-bit stereo PCM, matching the probe assets already in this directory.

    Normalised to 0.89 rather than 1.0: a sampler that transposes UP interpolates
    between samples and can overshoot the peak it was given, and a one-shot that
    clips the moment someone plays it a fifth higher is not a usable demo asset.
    """
    peak = max((abs(v) for v in samples), default=0.0) or 1.0
    scale = 0.89 / peak
    frames = bytearray()
    for v in samples:
        s = int(max(-1.0, min(1.0, v * scale)) * 32767)
        frames += struct.pack('<hh', s, s)
    header = b'RIFF' + struct.pack('<I', 36 + len(frames)) + b'WAVE'
    header += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, RATE, RATE * 4, 4, 16)
    header += b'data' + struct.pack('<I', len(frames))
    path.write_bytes(header + bytes(frames))
    print(f'  {path.name:16} {len(samples) / RATE:.2f}s  {len(header) + len(frames)} bytes')


def kick(seconds=0.55):
    """A sine whose pitch drops — the whole of a kick drum.

    Pitch-independent by nature, so it is the safe thing to reach for when
    demonstrating that a sample answers every key: it sounds like a kick wherever
    it is played, and nothing about it invites the question "is this in tune".
    """
    n = int(RATE * seconds)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        f = 45.0 + 115.0 * math.exp(-t * 38.0)      # 160Hz down to 45Hz, fast
        phase += 2.0 * math.pi * f / RATE
        body = math.sin(phase) * math.exp(-t * 7.0)
        # A click in the first 4ms. Without it the attack is a rising sine and
        # reads as soft on laptop speakers, which is what a room hears.
        click = math.exp(-t * 900.0) * math.sin(2.0 * math.pi * 1400.0 * t) * 0.35
        out.append(body + click)
    return out


def pluck(seconds=1.6, freq=261.6256):
    """A decaying harmonic pluck at middle C, so root 60 is a FACT.

    A freshly loaded slot is rooted at 60 and the note the tracker writes is
    compared against that root. If the asset is not actually middle C the whole
    mapping is a lie that happens to sound like an instrument — every interval a
    person plays is off by the same wrong amount, which is exactly the kind of
    error a demo audience hears and cannot name.

    Harmonics decay faster the higher they are, which is what makes a plucked
    string sound plucked rather than like an organ with a fade.
    """
    n = int(RATE * seconds)
    partials = [(1, 1.00, 3.2), (2, 0.50, 4.6), (3, 0.28, 6.1),
                (4, 0.16, 7.8), (5, 0.09, 9.4), (6, 0.05, 11.0)]
    out = []
    for i in range(n):
        t = i / RATE
        v = 0.0
        for mult, amp, decay in partials:
            v += amp * math.sin(2.0 * math.pi * freq * mult * t) * math.exp(-t * decay)
        # 1.5ms attack ramp. Not a taste decision: a waveform that starts at full
        # amplitude mid-cycle is a step, and a step is a click on every note.
        a = min(1.0, t / 0.0015)
        out.append(v * a)
    return out


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    print(f'writing to {OUT}')
    write_wav(OUT / 'demo_kick.wav', kick())
    write_wav(OUT / 'demo_pluck_c4.wav', pluck())
