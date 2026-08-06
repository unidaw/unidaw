/**
 * WHAT NOTES ARE IN THIS AUDIO? — pitch and onset detection, so a test can assert MUSIC.
 *
 * Everything measuring audio in this repo until now asked "was there a level here": peak, RMS,
 * loud-fraction, envelope shape. That answers "did something sound" and nothing else, and the
 * questions that actually matter about a DAW are not of that kind:
 *
 *   - did ALL the notes play, or did the second write get dropped?
 *   - is the harmony system applied to this track, or is it playing what was typed?
 *   - is that a strum or a block chord?
 *   - did the retrigger produce three strikes or one?
 *
 * Every one of those is a question about PITCH and TIME, and a peak cannot tell you any of them.
 * A track playing entirely the wrong notes has the same RMS as one playing the right ones.
 *
 * ── HOW IT WORKS ─────────────────────────────────────────────────────────────────────────────
 *
 * ONSETS by energy rise. A frame's RMS is compared against a running floor; a rise through the
 * threshold after being below it is an attack. Percussive and plucked material — which is what
 * this repo's fixtures are — gives clean rises. Sustained pads would need spectral flux, and
 * this says so rather than pretending otherwise.
 *
 * PITCH by YIN — the cumulative-mean-normalised difference function — with parabolic
 * interpolation, on a window just after each onset. Chosen over zero-crossing (which counts
 * harmonics as pitch on anything but a sine), over a bare FFT peak (which picks the loudest
 * PARTIAL, not the fundamental — a pluck's second harmonic is often louder than its first), and
 * over plain autocorrelation, which this was first written as and which failed: see the note on
 * `fundamental`.
 *
 * YIN is monophonic by nature: given a chord it reports the pitch its periodicity is strongest
 * at, usually the root. `detectNotes` is therefore honest about single lines and should not be
 * pointed at a chord and asked for three answers — `detectChordPitches` below is the best-effort
 * version for that, and its limits are written on it.
 *
 * ── WHAT IT IS NOT ───────────────────────────────────────────────────────────────────────────
 *
 * Not a transcriber. It will not follow overlapping voices, it has no opinion about note-offs,
 * and it reports what it is confident about rather than everything. Used as an assertion it is
 * strong in one direction: if it says a C4 sounded at 2.0s, one did. If it says nothing sounded,
 * check `confidence` before concluding silence.
 */

/** MIDI note number from a frequency in Hz. 69 = A4 = 440. */
export function midiOf(freq) {
  return 69 + 12 * Math.log2(freq / 440);
}

/** The nearest MIDI note, and how far off it was in cents. */
export function nearestNote(freq) {
  const exact = midiOf(freq);
  const midi = Math.round(exact);
  return { midi, cents: Math.round((exact - midi) * 100) };
}

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
/** `C-4` for 60, in the tracker's own spelling. */
export function noteName(midi) {
  return `${NAMES[((midi % 12) + 12) % 12]}-${Math.floor(midi / 12) - 1}`;
}

/**
 * The fundamental of one window, by YIN. Returns {freq, confidence} or null.
 *
 * `confidence` is 1 minus the normalised difference at the winning lag, 0..1. Below about 0.3
 * the window is noise or silence and the frequency means nothing — callers should reject on it
 * rather than believe a number.
 */
export function fundamental(samples, rate, { minHz = 50, maxHz = 2000, threshold = 0.15 } = {}) {
  const n = samples.length;
  if (n < 64) return null;

  let mean = 0;
  for (let i = 0; i < n; i++) mean += samples[i];
  mean /= n;

  const minLag = Math.max(2, Math.floor(rate / maxHz));
  const maxLag = Math.min(Math.floor(n / 2) - 1, Math.ceil(rate / minHz));
  if (maxLag <= minLag) return null;

  /*
   * YIN's DIFFERENCE FUNCTION, not plain autocorrelation.
   *
   * The first version of this used normalised autocorrelation and preferred "the shortest lag
   * that is nearly as good" to dodge the octave-down error. That heuristic is wrong in a way
   * that is obvious once measured and invisible until then: at tiny lags ANY smooth waveform
   * correlates with itself near 1.0, because consecutive samples are nearly equal. So it always
   * found an early lag and reported a pitch far too high — a synthesised C-2 came back as B-6,
   * and every other test tone landed exactly one semitone sharp.
   *
   * Calibrating against synthetic tones of known pitch is what caught it. Against real audio it
   * would have looked like plausible-but-wrong numbers, which is the shape that gets believed.
   *
   * YIN removes both failure modes by construction. d(τ) is the squared difference at lag τ, and
   * the CUMULATIVE MEAN NORMALISATION divides it by the running mean of all shorter lags — so a
   * trivially-good short lag is normalised away, and the first τ that dips below the threshold is
   * the true period rather than a multiple of it.
   */
  const d = new Float64Array(maxLag + 1);
  for (let lag = minLag; lag <= maxLag; lag++) {
    let sum = 0;
    for (let i = 0; i + lag < n; i++) {
      const delta = (samples[i] - mean) - (samples[i + lag] - mean);
      sum += delta * delta;
    }
    d[lag] = sum;
  }

  const dp = new Float64Array(maxLag + 1);
  dp[0] = 1;
  let running = 0;
  for (let lag = minLag; lag <= maxLag; lag++) {
    running += d[lag];
    dp[lag] = running === 0 ? 1 : (d[lag] * (lag - minLag + 1)) / running;
  }

  // The FIRST dip below the threshold, not the global minimum: the global minimum is often an
  // octave down, and taking the first qualifying dip is exactly what stops that.
  let bestLag = -1;
  for (let lag = minLag + 1; lag < maxLag; lag++) {
    if (dp[lag] < threshold && dp[lag] <= dp[lag + 1]) { bestLag = lag; break; }
  }
  if (bestLag < 0) {
    let lo = Infinity;
    for (let lag = minLag; lag <= maxLag; lag++) if (dp[lag] < lo) { lo = dp[lag]; bestLag = lag; }
    if (bestLag < 0 || lo > 0.6) return null;      // nothing periodic here
  }

  const y0 = dp[bestLag - 1] ?? dp[bestLag];
  const y1 = dp[bestLag];
  const y2 = dp[bestLag + 1] ?? dp[bestLag];
  const denom = y0 - 2 * y1 + y2;
  const shift = denom !== 0 ? (0.5 * (y0 - y2)) / denom : 0;
  const lag = bestLag + (Math.abs(shift) < 1 ? shift : 0);

  // Reported as 0..1 where 1 is a perfect period, so callers can reject on it the same way.
  return { freq: rate / lag, confidence: Math.max(0, Math.min(1, 1 - dp[bestLag])) };
}

/**
 * Every note in the audio, as {at, midi, name, freq, cents, confidence, level}.
 *
 * @param {Float64Array|Float32Array|number[]} mono
 * @param {number} rate
 * @param {object} [opts]
 * @param {number} [opts.floor]     RMS a frame must exceed to be sounding.
 * @param {number} [opts.minGap]    Seconds between onsets; below this they are one attack.
 * @param {number} [opts.minConf]   Reject a pitch below this autocorrelation confidence.
 * @param {number} [opts.skip]      Seconds to skip past an onset before measuring pitch.
 * @param {number} [opts.window]    Seconds of audio to measure the pitch over.
 */
export function detectNotes(mono, rate, opts = {}) {
  const {
    floor = 0.01, minGap = 0.05, minConf = 0.3,
    // MEASURED PAST THE ATTACK. A pluck's first few milliseconds are broadband noise — the
    // transient — and autocorrelating it returns confident nonsense. Skipping into the body of
    // the note is the difference between a reliable pitch and a random one.
    skip = 0.03, window = 0.12,
  } = opts;

  const hop = Math.max(1, Math.round(rate * 0.005));       // 5ms frames
  const frame = hop * 2;
  const rms = [];
  for (let i = 0; i + frame < mono.length; i += hop) {
    let s = 0;
    for (let k = 0; k < frame; k++) { const v = mono[i + k]; s += v * v; }
    rms.push(Math.sqrt(s / frame));
  }

  const out = [];
  let below = true, lastAt = -Infinity;
  for (let f = 0; f < rms.length; f++) {
    const at = (f * hop) / rate;
    if (below && rms[f] > floor) {
      below = false;
      if (at - lastAt < minGap) continue;
      lastAt = at;

      const from = Math.round((at + skip) * rate);
      const to = Math.min(mono.length, from + Math.round(window * rate));
      if (to - from < 64) continue;
      const seg = mono.slice(from, to);
      const f0 = fundamental(seg, rate);
      if (!f0 || f0.confidence < minConf) continue;
      const { midi, cents } = nearestNote(f0.freq);
      out.push({ at: +at.toFixed(4), midi, name: noteName(midi), freq: +f0.freq.toFixed(2),
                 cents, confidence: +f0.confidence.toFixed(3), level: +rms[f].toFixed(4) });
    } else if (!below && rms[f] < floor * 0.5) {
      below = true;
    }
  }
  return out;
}

/**
 * Best-effort pitches sounding together in one window — for CHORDS.
 *
 * HONEST LIMITS, because a confident wrong chord is worse than no answer. This picks spectral
 * peaks and keeps those that are not integer multiples of a lower kept peak, which separates a
 * triad's roots from one note's harmonics MOST of the time and not always: a major third is very
 * close to the 5th harmonic of the root two octaves down, and an inversion can put a voice below
 * the note whose harmonic series is being used to reject it.
 *
 * Use it to assert that a chord has MORE THAN ONE pitch, or that its lowest pitch moved when the
 * key changed. Do not use it to assert an exact three-note set — `detectNotes` on a strummed
 * chord, where the voices arrive apart, is the reliable way to get individual pitches.
 */
export function detectChordPitches(mono, rate, at, span = 0.25, { minRel = 0.2 } = {}) {
  const from = Math.max(0, Math.round(at * rate));
  const to = Math.min(mono.length, from + Math.round(span * rate));
  const n = to - from;
  if (n < 1024) return [];

  // A plain DFT over a musically useful range. Slow and exact; these windows are short and this
  // runs a handful of times per suite, so an FFT would be optimising the wrong thing.
  const lo = 60, hi = 1600, step = 1.0;
  const mags = [];
  for (let f = lo; f <= hi; f += step) {
    let re = 0, im = 0;
    const w = (2 * Math.PI * f) / rate;
    for (let i = 0; i < n; i++) {
      const s = mono[from + i];
      re += s * Math.cos(w * i);
      im += s * Math.sin(w * i);
    }
    mags.push({ f, m: Math.sqrt(re * re + im * im) / n });
  }
  const top = Math.max(...mags.map((x) => x.m));
  if (top <= 0) return [];

  const peaks = [];
  for (let i = 1; i < mags.length - 1; i++) {
    if (mags[i].m > mags[i - 1].m && mags[i].m >= mags[i + 1].m && mags[i].m > top * minRel) {
      peaks.push(mags[i]);
    }
  }
  peaks.sort((a, b) => b.m - a.m);

  const kept = [];
  for (const p of peaks) {
    // Drop a peak that is close to an integer multiple of one already kept: that is a harmonic
    // of a note we have, not a voice of its own.
    const harmonic = kept.some((k) => {
      const ratio = p.f / k.f;
      return ratio > 1.5 && Math.abs(ratio - Math.round(ratio)) < 0.04;
    });
    if (!harmonic) kept.push(p);
    if (kept.length >= 6) break;
  }
  return kept
    .map((p) => ({ ...nearestNote(p.f), freq: +p.f.toFixed(1), rel: +(p.m / top).toFixed(3) }))
    .sort((a, b) => a.midi - b.midi);
}
