/**
 * Just enough WAV reading to answer "did anything come out".
 *
 * The engine writes 16-bit PCM. There is no dependency here on purpose: the one
 * question these tests ask of audio is whether sound happened where notes are
 * and silence happened where they are not, and that is an envelope, not a
 * spectrum.
 */
import { readFileSync } from 'node:fs';

/** @returns {{rate:number, channels:number, mono:Float32Array}} */
export function readWav(path) {
  const b = readFileSync(path);
  if (b.length < 44 || b.toString('ascii', 0, 4) !== 'RIFF') {
    throw new Error(`not a WAV: ${path}`);
  }
  let off = 12, rate = 0, channels = 0, bits = 0, dataAt = -1, dataLen = 0;
  while (off + 8 <= b.length) {
    const id = b.toString('ascii', off, off + 4);
    const len = b.readUInt32LE(off + 4);
    if (id === 'fmt ') {
      channels = b.readUInt16LE(off + 10);
      rate = b.readUInt32LE(off + 12);
      bits = b.readUInt16LE(off + 22);
    } else if (id === 'data') {
      dataAt = off + 8; dataLen = Math.min(len, b.length - dataAt);
    }
    off += 8 + len + (len & 1);
  }
  if (dataAt < 0 || bits !== 16) throw new Error(`unsupported WAV (${bits} bits)`);
  const frames = Math.floor(dataLen / (2 * channels));
  const mono = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    let sum = 0;
    for (let c = 0; c < channels; c++) sum += b.readInt16LE(dataAt + (i * channels + c) * 2);
    mono[i] = sum / (channels * 32768);
  }
  return { rate, channels, mono };
}

/** RMS per slice, in order. */
export function envelope(mono, rate, sliceSeconds = 0.05) {
  const n = Math.max(1, Math.floor(rate * sliceSeconds));
  const out = [];
  for (let i = 0; i + n <= mono.length; i += n) {
    let s = 0;
    for (let k = 0; k < n; k++) s += mono[i + k] * mono[i + k];
    out.push(Math.sqrt(s / n));
  }
  return out;
}

/**
 * Loudness over one window of the capture, in seconds from the file's start.
 *
 * Separate from summarise() because the question is different: summarise asks
 * "did the take have sound in it", this asks "did the take have sound HERE" —
 * which is what any A/B against a change made mid-run needs. Out-of-range
 * windows return 0 rather than throwing, so a mis-timed test reads as silence
 * and its own non-vacuity check catches it; see stack.captureOffset for turning
 * a wall-clock moment into these numbers.
 */
export function rmsBetween(mono, rate, from, to) {
  const a = Math.max(0, Math.round(from * rate));
  const b = Math.min(mono.length, Math.round(to * rate));
  if (b <= a) return 0;
  let sum = 0;
  for (let i = a; i < b; i++) sum += mono[i] * mono[i];
  return Math.sqrt(sum / (b - a));
}

/** What fraction of a window is above a floor — density, where rms is level. */
export function loudFraction(mono, rate, from, to, floor = 0.004) {
  const a = Math.max(0, Math.round(from * rate));
  const b = Math.min(mono.length, Math.round(to * rate));
  if (b <= a) return 0;
  const env = envelope(mono.subarray(a, b), rate);
  if (!env.length) return 0;
  let loud = 0;
  for (const v of env) if (v > floor) loud++;
  return loud / env.length;
}

/**
 * How many note attacks are in a window, per second.
 *
 * Level is the wrong measure for "how many notes are playing" and the capture
 * that proved it is in patchcfg.mjs: a synth patch with a long release smears
 * every note into the next, so density read 0.99 at five notes a bar and 1.00 at
 * sixteen — a change you can SEE in the envelope and cannot measure as loudness.
 * Random pitches move the level around by more than the note count does.
 *
 * An attack is a RISE, though, and a rise survives being played over a bed of
 * decaying tails. So: a short-window envelope, and a slice counts as an onset
 * when it is `rise`x the one before it and above the floor, with a refractory
 * gap so one attack's leading edge is not counted twice.
 */
export function onsetsPerSecond(mono, rate, from, to, { rise = 1.5, floor = 0.004,
                                                        slice = 0.01, gap = 0.05 } = {}) {
  const a = Math.max(0, Math.round(from * rate));
  const b = Math.min(mono.length, Math.round(to * rate));
  if (b <= a) return 0;
  const env = envelope(mono.subarray(a, b), rate, slice);
  const refractory = Math.max(1, Math.round(gap / slice));
  let count = 0, last = -refractory;
  for (let i = 1; i < env.length; i++) {
    if (env[i] < floor) continue;
    if (env[i] > env[i - 1] * rise && i - last >= refractory) { count++; last = i; }
  }
  return count / ((b - a) / rate);
}

/** Loudest slice, and how many slices are above a floor. */
export function summarise(mono, rate, floor = 0.004) {
  const env = envelope(mono, rate);
  let peak = 0, loud = 0;
  for (const v of env) { if (v > peak) peak = v; if (v > floor) loud++; }
  return { slices: env.length, loud, peak, seconds: mono.length / rate };
}
