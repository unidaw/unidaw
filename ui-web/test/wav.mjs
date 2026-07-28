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

/** Loudest slice, and how many slices are above a floor. */
export function summarise(mono, rate, floor = 0.004) {
  const env = envelope(mono, rate);
  let peak = 0, loud = 0;
  for (const v of env) { if (v > peak) peak = v; if (v > floor) loud++; }
  return { slices: env.length, loud, peak, seconds: mono.length / rate };
}
