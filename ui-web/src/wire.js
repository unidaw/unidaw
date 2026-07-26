// Decoder for the sidecar's binary frames. Mirrors the encoder in
// ui/daw-sidecar/src/main.rs — change both together and bump WIRE_VERSION.
//
// Allocation-free in the steady state: the decoder fills a caller-owned store
// whose note objects are pooled and mutated in place. Frames arrive at ~86 Hz,
// so allocating a fresh object graph per frame would reintroduce exactly the GC
// churn the renderer was built to avoid. See GUIDELINES.md section 3.

export const WIRE_MAGIC = 0x31494e55; // "UNI1"
export const WIRE_VERSION = 6;

export const KIND_STATE = 0;
// Reserved for per-track DSP scope feeds. The kind/feed bytes exist from the
// start so those can be added additively instead of re-versioning both sides.

const HEADER_BYTES = 68;   // 56 + aggTracks u16 + extentCount u16 + lpb[8]
const NOTE_BYTES = 40;

/** A reusable decode target. One per connection. */
export function createStore() {
  return {
    ok: false,
    /** The engine has stopped publishing; this store is a corpse, not a snapshot. */
    stale: false,
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
    /** Per (track, row) aggregate for the requested window, from the engine's
     *  aggregate_rows. Flat: index = track * aggRows + row. Four values each. */
    aggCount: new Uint32Array(0),
    aggRep: new Uint8Array(0),
    aggLo: new Uint8Array(0),
    aggHi: new Uint8Array(0),
    aggRows: 0,
    aggTracks: 0,
    /** Real clip placements from the engine. Pooled like everything else. */
    extents: [],
    extentCount: 0,
    extentsRevision: 0,
    /** Per-track lines_per_beat. Needed to render a lane on its own grid AND to
     *  compute the tick a write targets — both halves of the projection. */
    lpb: new Uint8Array(8),
    /** The lines-per-beat the incoming rows are projected in. Part of the note
     *  cache key: zoom moves every row without touching clipVersion or
     *  noteCount, so a cache keyed only on those serves rows from the old grid. */
    rowGrid: 0,
    /** Bumped whenever notes actually changed, so consumers can skip work. */
    notesRevision: 0,
    /** Same, for aggregates. They change when the VIEWPORT moves, not when notes
     *  do — so a consumer keyed only on notesRevision never sees them arrive.
     *  That is the third time this exact shape has bitten: content changing
     *  without the thing the renderer watches changing. */
    aggRevision: 0,
  };
}

function note(store, i) {
  let n = store.notes[i];
  if (!n) {
    n = { tOn: 0, tOff: 0, id: 0, pitch: 0, velocity: 0, column: 0, track: 0,
          retrigger: 0, probability: 0, delayTicks: 0, row: 0,
          muted: false, isAdd: false, placementId: 0 };
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
  // Byte 7 is liveness for state frames: the engine has stopped publishing, so
  // everything below is the last thing it said rather than what is true now.
  store.stale = v.getUint8(7) !== 0;

  store.seq = Number(v.getBigUint64(8, true));
  store.playheadTick = Number(v.getBigUint64(16, true));
  store.visualSample = Number(v.getBigUint64(24, true));
  const clipVersion = v.getUint32(32, true);
  store.harmonyVersion = v.getUint32(36, true);
  store.transport = v.getUint16(40, true);
  store.trackCount = v.getUint16(42, true);

  const peakCount = v.getUint16(44, true);
  const rowGrid = v.getUint16(46, true);
  const noteCount = v.getUint32(48, true);
  const aggRows = v.getUint32(52, true);
  const aggTracks = v.getUint16(56, true);
  const extentCount = v.getUint16(58, true);
  for (let i = 0; i < 8; i++) store.lpb[i] = v.getUint8(60 + i);
  const aggN = aggRows * aggTracks;
  if (buf.byteLength < HEADER_BYTES + peakCount * 4 + noteCount * NOTE_BYTES + extentCount * 56 + aggN * 8) return false;

  if (store.peaks.length < peakCount) store.peaks = new Float32Array(peakCount);
  for (let i = 0; i < peakCount; i++) store.peaks[i] = v.getFloat32(HEADER_BYTES + i * 4, true);
  store.peakCount = peakCount;

  // Notes only move on an edit; the sidecar re-serialises them only when
  // clipVersion changes, so skip the copy when it hasn't.
  if (clipVersion !== store.clipVersion || noteCount !== store.noteCount
      || rowGrid !== store.rowGrid) {
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
      // Row comes from LaneGrid on the sidecar; the frontend never re-derives
      // the projection, so triplet grids work and there is one definition of it.
      n.row = v.getUint32(o + 36, true);
      // Offsets 30/31 are the note's two spare bytes; the 40-byte stride is
      // load-bearing for every section after the notes.
      const pf = v.getUint8(o + 30);
      n.muted = (pf & 1) !== 0;      // still shipped, drawn struck out
      n.isAdd = (pf & 2) !== 0;      // an override add, shown with provenance
      n.placementId = v.getUint8(o + 31);
    }
    store.noteCount = noteCount;
    store.clipVersion = clipVersion;
    store.rowGrid = rowGrid;
    store.notesRevision++;
  }

  // Clip extents: the rails. One per placement, timeline-positioned only.
  {
    let o = HEADER_BYTES + peakCount * 4 + noteCount * NOTE_BYTES;
    let changed = extentCount !== store.extentCount;
    for (let i = 0; i < extentCount; i++, o += 56) {
      let e = store.extents[i];
      if (!e) e = store.extents[i] = { placementId: 0, track: 0, startTick: 0, endTick: 0, name: '' };
      const pid = v.getUint32(o, true), tr = v.getUint32(o + 4, true);
      const st = Number(v.getBigUint64(o + 8, true)), en = Number(v.getBigUint64(o + 16, true));
      if (e.placementId !== pid || e.track !== tr || e.startTick !== st || e.endTick !== en) changed = true;
      e.placementId = pid; e.track = tr; e.startTick = st; e.endTick = en;
      if (changed) {
        let s = '';
        for (let k = 0; k < 32; k++) { const c = v.getUint8(o + 24 + k); if (!c) break; s += String.fromCharCode(c); }
        e.name = s;
      }
    }
    store.extentCount = extentCount;
    if (changed) store.extentsRevision++;
  }

  // Aggregates change every frame the viewport moves, so they are always read.
  if (aggN) {
    if (store.aggCount.length < aggN) {
      store.aggCount = new Uint32Array(aggN);
      store.aggRep = new Uint8Array(aggN);
      store.aggLo = new Uint8Array(aggN);
      store.aggHi = new Uint8Array(aggN);
    }
    let o = HEADER_BYTES + peakCount * 4 + noteCount * NOTE_BYTES + extentCount * 56;
    let changed = aggRows !== store.aggRows || aggTracks !== store.aggTracks;
    for (let i = 0; i < aggN; i++, o += 8) {
      const c = v.getUint32(o, true);
      const lo = v.getUint8(o + 5), hi = v.getUint8(o + 6);
      if (!changed && (store.aggCount[i] !== c || store.aggLo[i] !== lo || store.aggHi[i] !== hi)) changed = true;
      store.aggCount[i] = c;
      store.aggRep[i] = v.getUint8(o + 4);
      store.aggLo[i] = lo;
      store.aggHi[i] = hi;
    }
    if (changed) store.aggRevision++;
  }
  store.aggRows = aggRows;
  store.aggTracks = aggTracks;

  store.ok = true;
  return true;
}

const NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];

/** MIDI pitch to tracker notation, e.g. 60 -> "C-4". Allocates; call sparingly. */
export function pitchName(p) {
  return NAMES[p % 12] + Math.floor(p / 12 - 1);
}
