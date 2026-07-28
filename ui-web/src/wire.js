// Decoder for the sidecar's binary frames. Mirrors the encoder in
// ui/daw-sidecar/src/main.rs — change both together and bump WIRE_VERSION.
//
// Allocation-free in the steady state: the decoder fills a caller-owned store
// whose note objects are pooled and mutated in place. Frames arrive at ~86 Hz,
// so allocating a fresh object graph per frame would reintroduce exactly the GC
// churn the renderer was built to avoid. See GUIDELINES.md section 3.

export const WIRE_MAGIC = 0x31494e55; // "UNI1"
export const WIRE_VERSION = 15;

export const KIND_STATE = 0;
// Reserved for per-track DSP scope feeds. The kind/feed bytes exist from the
// start so those can be added additively instead of re-versioning both sides.

const HEADER_BYTES = 136;  // ...+ lpb 16 + mixer 8 + counts 16 + loop 16 + load 8 + tempo 8 + song meter 4
const HARMONY_BYTES = 16;
const NAME_BYTES = 24;
const PATCHER_NODE_BYTES = 40;
const PATCHER_EDGE_BYTES = 20;
const MIXER_BYTES = 12;
const NOTE_BYTES = 40;
/** Mirrors daw_bridge::layout::UiClipExtent. */
const EXTENT_BYTES = 64;
/** UI_CLIP_EXTENT_AUDIO — the placement is an audio region (schema v3). */
export const EXTENT_AUDIO = 1 << 0;

/**
 * The clip's own musical grid, packed into the spare bits of `UiClipExtent.flags`
 * (kShmVersion 19). Mirrors `unpack_clip_grid` in ui/daw-bridge/src/layout.rs —
 * change the two together.
 *
 * `linesPerBeat === 0` is the sentinel for "this clip publishes no grid", which is
 * NOT the same as "this clip is in 4/4": it means fall back to the SONG meter. The
 * denominator is stored as a power-of-two exponent, because the only denominators
 * music uses are powers of two and three bits are all that was spare.
 */
const CLIP_GRID_LPB_SHIFT = 1, CLIP_GRID_LPB_MASK = 0x1f;
const CLIP_GRID_NUM_SHIFT = 6, CLIP_GRID_NUM_MASK = 0x1f;
const CLIP_GRID_DEN_EXP_SHIFT = 11, CLIP_GRID_DEN_EXP_MASK = 0x7;

/**
 * Unpack a clip's grid, or return null when it publishes none.
 *
 * Writes into a caller-owned record when given one: this is read per rail per
 * frame in the arrangement, and GUIDELINES 3 forbids an object per call there.
 */
export function unpackClipGrid(flags, out) {
  const lpb = (flags >>> CLIP_GRID_LPB_SHIFT) & CLIP_GRID_LPB_MASK;
  if (lpb === 0) return null;
  const o = out || { linesPerBeat: 0, numerator: 4, denominator: 4 };
  o.linesPerBeat = lpb;
  o.numerator = (flags >>> CLIP_GRID_NUM_SHIFT) & CLIP_GRID_NUM_MASK;
  o.denominator = 1 << ((flags >>> CLIP_GRID_DEN_EXP_SHIFT) & CLIP_GRID_DEN_EXP_MASK);
  return o;
}

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
    /** The version the decoded timeline was read at; see the sidecar's twin. */
    harmonyVersionSeen: -1,
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
    /** Per-track mixer state, published by the engine (SHM v12). Authoritative —
     *  before this existed the mixer drew what this client last sent, which was
     *  wrong after a load, an undo, or any other surface moving a fader. */
    mixGain: new Int32Array(16), mixPan: new Int32Array(16), mixFlags: new Uint8Array(16),
    mixCount: 0,
    /** Moves only when the mixer actually changes; the cache key for it. */
    mixerVersion: -1,
    /** Track names, published by the engine (SHM v13). Four surfaces were
     *  labelling lanes "T01" because nothing carried the real ones. */
    names: [],
    /** The harmony timeline: {tick, root, scaleId} per key change. */
    harmony: [],
    /** The loop region (SHM v15). Settable before this, never drawable. */
    loopStart: 0, loopEnd: 0,
    /** A load result: seq bumps per attempt, ok says whether it took. */
    loadSeq: 0, loadOk: 1,
    /**
     * The tempo AT THE PLAYHEAD in milli-BPM, and how many points the project's
     * tempo map has (1 = constant tempo, 0 = the engine has not said).
     *
     * Milli-BPM as an integer, deliberately. The chrome guards its readout on
     * this value having changed, and a float that jitters in its last digit
     * defeats the guard — it would rebuild the string every frame to print the
     * same number. The engine publishes an integer for exactly this reason.
     *
     * Seeded at 120000 rather than 0 because 120 is what a project with no tempo
     * of its own plays at, and a transport bar reading "0.00 BPM" for the first
     * frame after connecting is a worse lie than the one this replaces.
     */
    tempoMilliBpm: 120000, tempoPointCount: 0,
    /**
     * The SONG's time signature (kShmVersion 19), which is what bar NUMBERING
     * counts in — the time gutter and the arrangement ruler. A clip may run a
     * different meter inside one of those bars; that grid rides on the clip and is
     * read from `UiClipExtent.flags`, not from here. `meter.js` is the module for
     * this one, and the two must not be confused: they answer different questions.
     *
     * Seeded at 4/4 for the same reason the tempo is seeded at 120 — it is what
     * the code assumed silently in four separate files before the engine published
     * anything, and the first frame after connecting should not be the one that
     * divides by a zero denominator.
     */
    songTimeSigNum: 4, songTimeSigDen: 4,
    /**
     * Child-track structure (kShmVersion 20). `trackParent[t]` is 0 for a
     * top-level track, else its parent's track id; bit0 of `trackFlags[t]` is
     * collapsed.
     *
     * A child is an ORDINARY track in every other array — same flat index, same
     * name, same lanes, same mixer strip. Collapse is a decision about what to
     * DRAW and never about what exists, which is why a collapsed parent still
     * publishes its children's rails: hiding and dropping are different, and only
     * one of them is recoverable.
     *
     * Fixed arrays, sized to the engine's track cap and reused: this is read per
     * frame and a fresh array per frame is the allocation the renderer was built
     * to avoid.
     */
    trackParent: new Uint32Array(16), trackFlags: new Uint8Array(16),
    /** The patcher graph (SHM v14). One global graph today; the shape does not
     *  change when it becomes per-device. */
    patcherVersion: -1, patcherDevice: 0, patcherNodes: [], patcherEdges: [],
    /** Per-track lines_per_beat. Needed to render a lane on its own grid AND to
     *  compute the tick a write targets — both halves of the projection. */
    lpb: new Uint8Array(16),
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
  // Sixteen lanes' worth, matching what the renderer can draw. This was 8 until
  // kShmVersion 21 widened the engine's own array to 64: at 8 the lanes past the
  // eighth silently had NO grid and fell back to the zoom's, which is a wrong grid
  // that looks like a choice. Every offset below shifted by 8 when it widened —
  // GUIDELINES 2.3, and the reason the encoder asserts its own header length and a
  // Rust test reads these literals.
  for (let i = 0; i < 16; i++) store.lpb[i] = v.getUint8(60 + i);
  const mixerVersion = v.getUint32(76, true);
  const mixCount = v.getUint16(80, true);
  const harmonyCount = v.getUint16(82, true);
  const nameCount = v.getUint16(84, true);
  const patcherVersion = v.getUint32(86, true);
  const patcherDevice = v.getUint32(90, true);
  const nodeCount = v.getUint16(94, true);
  const edgeCount = v.getUint16(96, true);
  store.loopStart = Number(v.getBigUint64(100, true));
  store.loopEnd = Number(v.getBigUint64(108, true));
  store.loadSeq = v.getUint32(116, true);
  store.loadOk = v.getUint32(120, true);
  store.tempoMilliBpm = v.getUint32(124, true) || 120000;
  store.tempoPointCount = v.getUint32(128, true);
  // `|| 4` on both: the sidecar already defaults them, so a zero here means a
  // frame from something older or a field mislaid, and 4/4 is the assumption the
  // whole page made before v19 anyway. A zero denominator would divide by zero in
  // every bar computation downstream of this line.
  store.songTimeSigNum = v.getUint16(132, true) || 4;
  store.songTimeSigDen = v.getUint16(134, true) || 4;
  // The child-track block is LAST in the frame, so its offset is everything else
  // added up. Decoded after the sections it follows — see PARENT_AT below.

  const aggN = aggRows * aggTracks;
  const varBefore = harmonyCount * HARMONY_BYTES + nameCount * NAME_BYTES + nodeCount * PATCHER_NODE_BYTES
                  + edgeCount * PATCHER_EDGE_BYTES + mixCount * MIXER_BYTES;
  if (buf.byteLength < HEADER_BYTES + varBefore + peakCount * 4
      + noteCount * NOTE_BYTES + extentCount * EXTENT_BYTES + aggN * 8) return false;

  // Harmony, names, patcher, mixer — matching the encoder.
  {
    let o = HEADER_BYTES;
    // store.harmonyVersion was set from the header a few lines above, so it is
    // already the NEW value; `harmonyVersionSeen` is the one the decoded timeline
    // came from. Two fields, because comparing a value against itself is the
    // cache-key mistake this file has a table about.
    if (harmonyCount !== store.harmony.length
        || store.harmonyVersion !== store.harmonyVersionSeen) {
      store.harmonyVersionSeen = store.harmonyVersion;
      store.harmony.length = harmonyCount;
      for (let i = 0; i < harmonyCount; i++) {
        const b = o + i * HARMONY_BYTES;
        store.harmony[i] = { tick: Number(v.getBigUint64(b, true)),
                             root: v.getUint32(b + 8, true),
                             scaleId: v.getUint32(b + 12, true) };
      }
    }
    o += harmonyCount * HARMONY_BYTES;
    // Compared, not keyed on clipVersion. A rename changes a name and nothing
    // else, so keying this on the clip version made the engine accept the
    // command, the ack say ok, and the name never move — and I made the same
    // mistake on the sidecar side in the same commit. Names are 8x24 bytes;
    // comparing them costs less than being wrong about them.
    let namesChanged = nameCount !== store.names.length;
    if (!namesChanged) {
      for (let i = 0; i < nameCount && !namesChanged; i++) {
        const at = o + i * NAME_BYTES;
        const cur = store.names[i] || '';
        for (let k = 0; k < NAME_BYTES; k++) {
          const c = v.getUint8(at + k);
          if (k >= cur.length) { if (c !== 0) namesChanged = true; break; }
          if (c !== cur.charCodeAt(k)) { namesChanged = true; break; }
        }
      }
    }
    if (namesChanged) {
      store.names.length = nameCount;
      for (let i = 0; i < nameCount; i++) {
        let s = '';
        for (let k = 0; k < NAME_BYTES; k++) {
          const c = v.getUint8(o + i * NAME_BYTES + k);
          if (!c) break;
          s += String.fromCharCode(c);
        }
        store.names[i] = s;
      }
    }
    o += nameCount * NAME_BYTES;
    if (patcherVersion !== store.patcherVersion) {
      store.patcherVersion = patcherVersion;
      store.patcherDevice = patcherDevice;
      store.patcherNodes.length = nodeCount;
      for (let i = 0; i < nodeCount; i++) {
        const b = o + i * PATCHER_NODE_BYTES;
        const cfg = new Int32Array(8);
        for (let k = 0; k < 8; k++) cfg[k] = v.getInt32(b + 8 + k * 4, true);
        store.patcherNodes[i] = { id: v.getUint32(b, true), type: v.getUint8(b + 4),
                                  hasConfig: v.getUint8(b + 5) !== 0, config: cfg };
      }
      o += nodeCount * PATCHER_NODE_BYTES;
      store.patcherEdges.length = edgeCount;
      for (let i = 0; i < edgeCount; i++) {
        const b = o + i * PATCHER_EDGE_BYTES;
        store.patcherEdges[i] = { src: v.getUint32(b, true), srcPort: v.getUint32(b + 4, true),
                                  dst: v.getUint32(b + 8, true), dstPort: v.getUint32(b + 12, true),
                                  kind: v.getUint8(b + 16) };
      }
    }
  }

  const MIXER_AT = HEADER_BYTES + harmonyCount * HARMONY_BYTES + nameCount * NAME_BYTES
                 + nodeCount * PATCHER_NODE_BYTES + edgeCount * PATCHER_EDGE_BYTES;
  if (mixerVersion !== store.mixerVersion || mixCount !== store.mixCount) {
    for (let i = 0; i < mixCount && i < store.mixGain.length; i++) {
      const o = MIXER_AT + i * MIXER_BYTES;
      store.mixGain[i] = v.getInt32(o, true);
      store.mixPan[i] = v.getInt32(o + 4, true);
      store.mixFlags[i] = v.getUint8(o + 8);
    }
    store.mixCount = Math.min(mixCount, store.mixGain.length);
    store.mixerVersion = mixerVersion;
  }
  const AFTER_MIXER = MIXER_AT + mixCount * MIXER_BYTES;

  if (store.peaks.length < peakCount) store.peaks = new Float32Array(peakCount);
  for (let i = 0; i < peakCount; i++) store.peaks[i] = v.getFloat32(AFTER_MIXER + i * 4, true);
  store.peakCount = peakCount;

  // Notes only move on an edit; the sidecar re-serialises them only when
  // clipVersion changes, so skip the copy when it hasn't.
  if (clipVersion !== store.clipVersion || noteCount !== store.noteCount
      || rowGrid !== store.rowGrid) {
    let o = AFTER_MIXER + peakCount * 4;
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
    let o = AFTER_MIXER + peakCount * 4 + noteCount * NOTE_BYTES;
    let changed = extentCount !== store.extentCount;
    for (let i = 0; i < extentCount; i++, o += EXTENT_BYTES) {
      let e = store.extents[i];
      if (!e) e = store.extents[i] = { placementId: 0, clipId: 0, track: 0, flags: 0,
                                        startTick: 0, endTick: 0, name: '', audio: false,
                                        // The clip's own grid, or null when it publishes
                                        // none. The record is owned by the extent and
                                        // rewritten in place; `grid` points at it or at
                                        // null, so a consumer can test one field.
                                        _grid: { linesPerBeat: 0, numerator: 4, denominator: 4 },
                                        grid: null };
      const pid = v.getUint32(o, true), cid = v.getUint32(o + 4, true);
      const tr = v.getUint32(o + 8, true), fl = v.getUint32(o + 12, true);
      const st = Number(v.getBigUint64(o + 16, true)), en = Number(v.getBigUint64(o + 24, true));
      if (e.placementId !== pid || e.track !== tr || e.startTick !== st
          || e.endTick !== en || e.flags !== fl || e.clipId !== cid) changed = true;
      e.placementId = pid; e.clipId = cid; e.track = tr; e.flags = fl;
      e.startTick = st; e.endTick = en;
      // bit0: an audio region. It holds no note events, so the arrange view draws
      // it as a waveform slot rather than a lane of notes.
      e.audio = (fl & EXTENT_AUDIO) !== 0;
      // The clip's own meter and row grid, unpacked from the same word. Null when
      // the clip publishes none, which means "count it in the song's meter" and is
      // a different claim from "it is in 4/4".
      e.grid = unpackClipGrid(fl, e._grid);
      if (changed) {
        let s = '';
        for (let k = 0; k < 32; k++) { const c = v.getUint8(o + 32 + k); if (!c) break; s += String.fromCharCode(c); }
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
    let o = AFTER_MIXER + peakCount * 4 + noteCount * NOTE_BYTES + extentCount * EXTENT_BYTES;
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

  /**
   * Child-track structure (v20), last in the frame.
   *
   * Its offset is everything before it added up, which is exactly why it is last:
   * a section appended at the end cannot shift anything, and GUIDELINES 2.3 is a
   * list of the two times widening a record in the middle shifted the whole tail
   * and made every field after it decode as garbage.
   *
   * Bounds-checked rather than assumed. A frame that stops short here is a frame
   * from something older or a count that disagrees, and reading past the end of a
   * DataView throws — which would take down the socket handler and, with it, the
   * whole UI, to report a field nothing yet draws with.
   */
  {
    const at = AFTER_MIXER + peakCount * 4 + noteCount * NOTE_BYTES
             + extentCount * EXTENT_BYTES + aggN * 8;
    const tc = store.trackCount;
    const have = Math.max(0, Math.min(tc, (buf.byteLength - at) >> 3));
    if (store.trackParent.length < tc) {
      store.trackParent = new Uint32Array(tc);
      store.trackFlags = new Uint8Array(tc);
    }
    for (let t = 0; t < have; t++) {
      store.trackParent[t] = v.getUint32(at + t * 8, true);
      store.trackFlags[t] = v.getUint8(at + t * 8 + 4);
    }
    // Anything the frame did not carry reads as top-level and expanded, which is
    // what every track is until the engine starts creating children from a
    // multi-out plugin's aux buses.
    for (let t = have; t < tc; t++) { store.trackParent[t] = 0; store.trackFlags[t] = 0; }
  }

  store.ok = true;
  return true;
}

const NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];

/**
 * Every name a MIDI pitch can have, built once.
 *
 * This used to be `NAMES[p % 12] + Math.floor(p / 12 - 1)`, which is two string
 * allocations per call — and it is called once per visible note per frame by the
 * tracker's view-model and again by the piano roll. A busy screen is a few
 * hundred notes, so that was a few hundred short-lived strings every 16 ms to
 * spell out 128 values that never change. The domain is bounded and tiny; the
 * table costs 128 strings once and nothing thereafter.
 */
const PITCH_NAMES = new Array(128);
for (let p = 0; p < 128; p++) PITCH_NAMES[p] = NAMES[p % 12] + Math.floor(p / 12 - 1);

/** MIDI pitch to tracker notation, e.g. 60 -> "C-4". Allocation-free in range. */
export function pitchName(p) {
  return PITCH_NAMES[p] !== undefined ? PITCH_NAMES[p]
                                      : NAMES[((p % 12) + 12) % 12] + Math.floor(p / 12 - 1);
}

// --- waveform answers ------------------------------------------------------
//
// A separate frame on the command socket, not part of the state frame: it is a
// reply to a question, arrives at zoom/pan rate rather than at 86 Hz, and is up
// to 98 KB of int16. Its own magic so a mis-routed frame is refused rather than
// read as a state frame that happens to start with the wrong bytes.

export const WAVE_MAGIC = 0x57494e55; // "UNIW"
export const WAVE_WIRE_VERSION = 1;
const WAVE_HEADER_BYTES = 56;

/** status: what the engine could do with the request. */
export const WAVE_OK = 0, WAVE_TRUNCATED = 1, WAVE_NOTREADY = 2, WAVE_BAD = 3;

/**
 * Decode one waveform answer into `out`, returning it, or null if the buffer is
 * not one.
 *
 * `out.pairs` is an Int16Array VIEW over the incoming buffer, not a copy. The
 * buffer arrives fresh from the socket and nothing else refers to it, so a copy
 * would be 98 KB of pure waste per zoom step. It is channel-planar then column
 * then (min, max): for channel c column i, pairs[(c * columns + i) * 2] and +1.
 *
 * The 64-bit fields are split lo/hi on the wire because the reader is JavaScript
 * and getBigUint64 allocates a BigInt per call — see the store's tempo fields for
 * the same reasoning. Frame counts fit a double exactly well past any real file:
 * 2^53 frames is 6,500 years at 44.1 kHz.
 */
export function decodeWaveform(buf, out) {
  if (!buf || buf.byteLength < WAVE_HEADER_BYTES) return null;
  const v = new DataView(buf);
  if (v.getUint32(0, true) !== WAVE_MAGIC) return null;
  if (v.getUint16(4, true) !== WAVE_WIRE_VERSION) return null;
  if (v.getUint8(6) !== 1) return null;            // a kind we do not handle
  out.status = v.getUint8(7);
  out.requestSeq = v.getUint32(8, true);
  out.sourceId = v.getUint32(12, true);
  out.keyLo = v.getUint32(16, true);
  out.keyHi = v.getUint32(20, true);
  out.decimation = v.getUint32(24, true);
  out.columns = v.getUint32(28, true);
  out.channels = v.getUint32(32, true);
  out.firstFrame = v.getUint32(36, true) + v.getUint32(40, true) * 4294967296;
  out.frameCount = v.getUint32(44, true) + v.getUint32(48, true) * 4294967296;
  out.flags = v.getUint32(52, true);
  const want = out.columns * out.channels * 2;
  const have = (buf.byteLength - WAVE_HEADER_BYTES) >> 1;
  // A short payload means the header and the body disagree, which is the stride
  // bug this codebase has shipped twice. Refuse rather than read past the end and
  // draw whatever is there.
  if (have < want) { out.status = WAVE_BAD; out.pairs = null; return out; }
  out.pairs = new Int16Array(buf, WAVE_HEADER_BYTES, want);
  return out;
}

/** A reusable decode target for waveform answers. */
export function createWaveform() {
  return { status: WAVE_NOTREADY, requestSeq: 0, sourceId: 0, keyLo: 0, keyHi: 0,
           decimation: 0, columns: 0, channels: 0, firstFrame: 0, frameCount: 0,
           flags: 0, pairs: null };
}
