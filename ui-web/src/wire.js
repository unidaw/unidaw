// Decoder for the sidecar's binary frames. Mirrors the encoder in
// ui/daw-sidecar/src/main.rs — change both together and bump WIRE_VERSION.
//
// Allocation-free in the steady state: the decoder fills a caller-owned store
// whose note objects are pooled and mutated in place. Frames arrive at ~86 Hz,
// so allocating a fresh object graph per frame would reintroduce exactly the GC
// churn the renderer was built to avoid. See GUIDELINES.md section 3.

export const WIRE_MAGIC = 0x31494e55; // "UNI1"
export const WIRE_VERSION = 2;

export const KIND_STATE = 0;
// Reserved for per-track DSP scope feeds. The kind/feed bytes exist from the
// start so those can be added additively instead of re-versioning both sides.

const HEADER_BYTES = 56;
const NOTE_BYTES = 40;

/** A reusable decode target. One per connection. */
export function createStore() {
  return {
    ok: false,
    seq: 0,
    playheadTick: 0,
    visualSample: 0,
    clipVersion: -1,
    harmonyVersion: 0,
    transport: 0,
    trackCount: 0,
    peaks: new Float32Array(64),
    peakCount: 0,
    /** Pooled note objects; only the first `noteCount` are meaningful. */
    notes: [],
    noteCount: 0,
    /** Bumped whenever notes actually changed, so consumers can skip work. */
    notesRevision: 0,
  };
}

function note(store, i) {
  let n = store.notes[i];
  if (!n) {
    n = { tOn: 0, tOff: 0, id: 0, pitch: 0, velocity: 0, column: 0, track: 0,
          retrigger: 0, probability: 0, delayTicks: 0 };
    store.notes[i] = n;
  }
  return n;
}

/**
 * Decode one frame into `store`. Returns false and leaves the store untouched
 * if the frame is not one we understand — a wrong magic or version means the
 * sidecar and the page disagree, and rendering a misread frame is worse than
 * rendering nothing.
 *
 * @param {ArrayBuffer} buf
 * @param {ReturnType<createStore>} store
 */
export function decode(buf, store) {
  if (buf.byteLength < HEADER_BYTES) return false;
  const v = new DataView(buf);
  if (v.getUint32(0, true) !== WIRE_MAGIC) return false;
  if (v.getUint16(4, true) !== WIRE_VERSION) return false;
  if (v.getUint8(6) !== KIND_STATE) return true; // a feed we don't handle yet

  store.seq = Number(v.getBigUint64(8, true));
  store.playheadTick = Number(v.getBigUint64(16, true));
  store.visualSample = Number(v.getBigUint64(24, true));
  const clipVersion = v.getUint32(32, true);
  store.harmonyVersion = v.getUint32(36, true);
  store.transport = v.getUint16(40, true);
  store.trackCount = v.getUint16(42, true);

  const peakCount = v.getUint16(44, true);
  const noteCount = v.getUint32(48, true);
  if (buf.byteLength < HEADER_BYTES + peakCount * 4 + noteCount * NOTE_BYTES) return false;

  if (store.peaks.length < peakCount) store.peaks = new Float32Array(peakCount);
  for (let i = 0; i < peakCount; i++) store.peaks[i] = v.getFloat32(HEADER_BYTES + i * 4, true);
  store.peakCount = peakCount;

  // Notes only move on an edit; the sidecar re-serialises them only when
  // clipVersion changes, so skip the copy when it hasn't.
  if (clipVersion !== store.clipVersion || noteCount !== store.noteCount) {
    let o = HEADER_BYTES + peakCount * 4;
    for (let i = 0; i < noteCount; i++, o += NOTE_BYTES) {
      const n = note(store, i);
      n.tOn = Number(v.getBigUint64(o, true));
      n.tOff = Number(v.getBigUint64(o + 8, true));
      n.id = Number(v.getBigUint64(o + 16, true));
      n.pitch = v.getUint8(o + 24);
      n.velocity = v.getUint8(o + 25);
      n.column = v.getUint8(o + 26);
      n.track = v.getUint8(o + 27);
      n.retrigger = v.getUint8(o + 28);
      n.probability = v.getUint8(o + 29);
      n.delayTicks = v.getUint32(o + 32, true);
    }
    store.noteCount = noteCount;
    store.clipVersion = clipVersion;
    store.notesRevision++;
  }

  store.ok = true;
  return true;
}

const NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];

/** MIDI pitch to tracker notation, e.g. 60 -> "C-4". Allocates; call sparingly. */
export function pitchName(p) {
  return NAMES[p % 12] + Math.floor(p / 12 - 1);
}
