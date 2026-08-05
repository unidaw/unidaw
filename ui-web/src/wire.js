// Decoder for the sidecar's binary frames. Mirrors the encoder in
// ui/daw-sidecar/src/main.rs — change both together and bump WIRE_VERSION.
//
// Allocation-free in the steady state: the decoder fills a caller-owned store
// whose note objects are pooled and mutated in place. Frames arrive at ~86 Hz,
// so allocating a fresh object graph per frame would reintroduce exactly the GC
// churn the renderer was built to avoid. See GUIDELINES.md section 3.

export const WIRE_MAGIC = 0x31494e55; // "UNI1"
export const WIRE_VERSION = 28;

export const KIND_STATE = 0;
// Reserved for per-track DSP scope feeds. The kind/feed bytes exist from the
// start so those can be added additively instead of re-versioning both sides.

const HEADER_BYTES = 296;  // ...+ lpb 64 + mixer 8 + counts 16 + loop 16 + load 8 + tempo 8 + song meter 4 + meter count 4 + quantize 8 + ops width 64
const HARMONY_BYTES = 16;
const NAME_BYTES = 24;
const PATCHER_NODE_BYTES = 40;
const PATCHER_EDGE_BYTES = 20;
const MIXER_BYTES = 12;
/*
 * 44, not 40 — the stride GREW for v26's per-note quantize deviation.
 *
 * It is load-bearing for every section after the notes (extents, aggregates,
 * track structure, chords, meters, quantize), which is why growing it on one side
 * only made the extents decode as garbage once before. Both sides move together
 * and WIRE_VERSION goes with them, so a mismatched page rejects the frame rather
 * than rendering nonsense.
 */
const NOTE_BYTES = 50;
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
    /** A track's chords, pooled like the notes. See the decode at the tail. */
    chords: [], chordCount: 0, chordsRevision: 0,
    /** Where the chord section ends, so the meters after it need not re-derive it. */
    chordsEnd: 0,
    /** Likewise for the section after the meters. */
    metersEnd: 0,
    /** ...and for the one after the quantize block. */
    quantizeEnd: 0,
    /**
     * v24 per-insert meters, pooled. Keyed by (track, device) — MATCHED ON
     * device id and never on position, because the engine's compacted insert
     * order skips patcher devices, so the Nth meter is not the Nth card.
     *
     * Levels are dBFS millibels: 0 is full scale, ordinary values negative, and
     * METER_SILENT is a real reading rather than a hole — an instrument has no
     * audio input and honestly reports its input silent forever.
     */
    meters: [], meterCount: 0,
    /**
     * v26 per-lane NON-DESTRUCTIVE quantize, pooled and keyed by TRACK ID.
     *
     * Only lanes that HAVE a grid arrive — an unquantized lane is the
     * overwhelming majority and has nothing to say — so an absent track means
     * "not quantized", which is also what grid 0 means.
     *
     * `swing` is PLAIN SIGNED. The command that sets it biases by +500 because
     * that payload field is unsigned; this does not.
     */
    quantize: [], quantizeCount: 0,
    /**
     * THE SONG’S MARKERS. A marker is a NAMED TICK and stores no length.
     *
     * `bars` and `endTick` are DERIVED below, from the NEXT marker — or from `songEnd` for the
     * last one — because a span is two adjacent markers and nothing stores a span. That
     * derivation is subtraction and is safe to do anywhere.
     *
     * `bar` and `beat` are NOT derived and must not be: bar numbering across a meter change is
     * a prefix sum through the meter map, not `tick / barLength`. The engine resolves them, and
     * a client that computed them would be right until the first 7/8 passage and then quietly
     * wrong — markers sitting between ruler numbers that do not match them.
     *
     * `markersTruncated` non-zero means the list is INCOMPLETE. Forwarded rather than dropped,
     * because a short list that says nothing reads as a complete one.
     */
    markers: [], markerCount: 0, markersTruncated: 0, markersEnd: 0,
    /**
     * The arrange region’s own generation, and the furthest placement end.
     *
     * The generation moves on a MARKER or a METER change and never on a note edit, so the
     * markers can be cached on it and survive typing. `songEnd` is not the last marker:
     * material can sit past every marker, and it plays and is unnamed.
     */
    arrangeVersion: 0, songEnd: 0,
    /**
     * The audio device's block size in frames and its rate in Hz.
     *
     * Both 0 until the engine has opened a device, and that is a STATE and not a default:
     * a latency readout of "0.0ms" says the machine is perfect, when what it means is that
     * nothing has started. The chrome draws nothing while they are zero.
     */
    blockSize: 0, sampleRateHz: 0,
    /**
     * WHICH PARAMETERS ARE AUTOMATED — the list, never the curves.
     *
     * `{track, target, points, discrete, param}` per lane. The points are fetched per lane on
     * request, because a song can hold far more automation than a frame should carry and only
     * the open lanes are ever drawn.
     *
     * `automationTruncated` non-zero means the list is INCOMPLETE, and an incomplete list that
     * says nothing reads as a complete one.
     */
    automation: [], automationCount: 0, automationTruncated: 0, automationVersion: 0,
    samplerKitVersion: 0,
    /**
     * Moves when a lane's quantize changes and NEVER when a note does — backend
     * kept it off the clip version deliberately, since quantize moves no authored
     * note and must not invalidate an in-flight edit. Cache the deviation layer on
     * this: keyed on clipVersion it would rebuild on every keystroke while missing
     * the one change it cares about.
     */
    quantizeVersion: 0,
    /** The patcher graph (SHM v14). One global graph today; the shape does not
     *  change when it becomes per-device. */
    patcherVersion: -1, patcherDevice: 0, patcherNodes: [], patcherEdges: [],
    /** Per-track lines_per_beat. Needed to render a lane on its own grid AND to
     *  compute the tick a write targets — both halves of the projection. */
    lpb: new Uint8Array(64),
    /** Per-track op-column width in GLYPHS (SHM v34), 0 = no note in that track carries
     *  an op. The tracker sizes the ops cell from it and hides the column entirely at 0,
     *  which is the difference between an empty column on every drum track forever and a
     *  column that appears the moment one is needed. */
    opsWidth: new Uint8Array(64),
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
          retrigger: 0, probability: 0, delayTicks: 0, devTicks: 0, row: 0,
          sound: 0, soundOffset: 0, retrigRamp: 0, trigCondition: 0,
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
  // SIXTY-FOUR lanes' worth — `kUiMaxTracks`, so the wire can carry every track the engine
  // can hold. This was 8, then 16, and each widening had the same cause: a lane past the end
  // silently had NO grid and fell back to the zoom's, which is a wrong grid that looks like a
  // choice. At 16 that was invisible because nothing could make a seventeenth track; the track
  // cap is what hid it, and the cap is going away.
  //
  // Every offset below shifted by 48 when it widened — GUIDELINES 2.3, and the reason the
  // encoder asserts its own header length and a Rust test reads these literals. The encoder
  // needs no offset edit at all: it writes the header in order, so growing the array moves
  // everything after it. Only this side spells offsets out, which is why only this side can
  // get them wrong.
  for (let i = 0; i < 64; i++) store.lpb[i] = v.getUint8(60 + i);
  // 232..296, appended after the last scalar rather than beside lpb — see the writer's note.
  for (let i = 0; i < 64; i++) store.opsWidth[i] = v.getUint8(232 + i);
  const mixerVersion = v.getUint32(124, true);
  const mixCount = v.getUint16(128, true);
  const harmonyCount = v.getUint16(130, true);
  const nameCount = v.getUint16(132, true);
  const patcherVersion = v.getUint32(134, true);
  const patcherDevice = v.getUint32(138, true);
  const nodeCount = v.getUint16(142, true);
  const edgeCount = v.getUint16(144, true);
  store.loopStart = Number(v.getBigUint64(148, true));
  store.loopEnd = Number(v.getBigUint64(156, true));
  store.loadSeq = v.getUint32(164, true);
  store.loadOk = v.getUint32(168, true);
  store.tempoMilliBpm = v.getUint32(172, true) || 120000;
  store.tempoPointCount = v.getUint32(176, true);
  // `|| 4` on both: the sidecar already defaults them, so a zero here means a
  // frame from something older or a field mislaid, and 4/4 is the assumption the
  // whole page made before v19 anyway. A zero denominator would divide by zero in
  // every bar computation downstream of this line.
  store.songTimeSigNum = v.getUint16(180, true) || 4;
  store.songTimeSigDen = v.getUint16(182, true) || 4;
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
                                  hasConfig: v.getUint8(b + 5) !== 0,
                                  /*
                                   * WHOSE GRAPH THIS NODE IS IN. 0 = a pool node, owned by no
                                   * device. The region publishes the ASSEMBLED POOL — every
                                   * device's graph unioned with re-id'd nodes — so the region
                                   * itself cannot say whose graph it is and only this can.
                                   *
                                   * It is the fact an edit needs before it can be addressed to
                                   * a device; without it every patcher command is pool-scoped,
                                   * and a pool node is not in the graph a project saves.
                                   */
                                  owner: v.getUint16(b + 6, true), config: cfg };
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

  /*
   * Notes only move on an edit, so the copy is skipped when nothing changed — but
   * QUANTIZE moves what a note carries WITHOUT moving the clip version, and that
   * exception cost a real bug on both sides of this wire.
   *
   * A note's `devTicks` — how far its lane shifts it — changes when the lane's
   * quantize changes, and SetLaneQuantize deliberately does not bump the clip
   * version, because it invalidates nobody's edit. So the deviations stayed at
   * their old values here until an unrelated note edit came along: right by
   * accident, stale the rest of the time.
   *
   * The sidecar had the same hole in its own re-read gate, and the engine had it
   * one layer below that. Three gates, one missing condition, and the quantize
   * suite passed over all three because it changed the ZOOM before reading — which
   * moves `rowGrid` and forces the copy for an unrelated reason.
   */
  const quantizeVersion = v.getUint32(188, true);
  if (clipVersion !== store.clipVersion || noteCount !== store.noteCount
      || rowGrid !== store.rowGrid
      || quantizeVersion !== store.quantizeVersion) {
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
      /*
       * How far this note's LANE moves it, signed, in nanoticks. 0 when the lane
       * is not quantized.
       *
       * The sounding tick is `tOn + devTicks + delayTicks` — quantize and the
       * note's own delay COMPOSE, because the scheduler quantizes the note start
       * and then adds the delay. One mark, from written to heard.
       */
      n.devTicks = v.getInt32(o + 40, true);
      /*
       * v32 THE SOUND ADDRESS, and the seek into it.
       *
       * `sound` is 0 on every row of an ordinary kit track — the keymap picks the slot from
       * pitch — so ZERO IS ABSENCE here, exactly as it is for probability and retrigger, and
       * `rowops.js` already treats a falsy value that way. Drawing "0" would claim the note
       * addresses slot zero, which is not a slot.
       *
       * `soundOffset` is a FRACTION of the slot's extent rather than a frame count: absolute
       * frames break when the slot's sample is swapped, and a slot can name a slice, so they
       * break on a re-chop too.
       */
      n.sound = v.getUint16(o + 44, true);
      n.soundOffset = v.getUint16(o + 46, true);
      // v33. SIGNED for the ramp — a crescendo is the same op with the other sign, and reading
      // it unsigned would turn -60 into 196 and draw a roll that gets louder as one that
      // vanishes. `getInt8` rather than a mask, so the sign is the decoder's business once.
      n.retrigRamp = v.getInt8(o + 48);
      n.trigCondition = v.getUint8(o + 49);
      // Offsets 30/31 are the note's two spare bytes; the stride is load-bearing
      // for every section after the notes.
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

  /*
   * CHORDS, the last section in the frame.
   *
   * A chord is not a note: it is (degree, quality, inversion) resolved against
   * the harmony timeline, which is what lets a chord track survive a key change.
   * The engine has always published them and this side never read them, so a
   * track of chords played and showed nothing at all.
   *
   * Bounds-checked like the block above and for the same reason: a frame from an
   * older sidecar simply stops before this, and reading past the end of a
   * DataView throws — which would take down the socket handler and the whole UI
   * with it, to report a field that is merely absent.
   */
  {
    const at = AFTER_MIXER + peakCount * 4 + noteCount * NOTE_BYTES
             + extentCount * EXTENT_BYTES + aggN * 8 + store.trackCount * 8;
    const want = v.getUint16(146, true);
    const have = Math.max(0, Math.min(want, (buf.byteLength - at) / CHORD_BYTES | 0));
    while (store.chords.length < have) {
      store.chords.push({ tick: 0, duration: 0, id: 0, track: 0, degree: 0,
                          quality: 0, inversion: 0, octave: 0, flags: 0, row: 0,
                          // The strum, in the engine's own units: spread is nanoticks
                          // between the first and last voice, the humanize pair are
                          // 0..255 amounts. All three zero IS a block chord, which is
                          // why they are read as numbers and not as a flag.
                          spread: 0, humanizeTiming: 0, humanizeVelocity: 0 });
    }
    let changed = store.chordCount !== have;
    for (let i = 0; i < have; i++) {
      const o = at + i * CHORD_BYTES;
      const c = store.chords[i];
      const tick = Number(v.getBigUint64(o, true));
      const dur = Number(v.getBigUint64(o + 8, true));
      const id = v.getUint32(o + 16, true);
      const track = v.getUint8(o + 20), degree = v.getUint8(o + 21);
      const quality = v.getUint8(o + 22), inversion = v.getUint8(o + 23);
      const octave = v.getUint8(o + 24);
      if (c.tick !== tick || c.duration !== dur || c.id !== id || c.track !== track
          || c.degree !== degree || c.quality !== quality
          || c.inversion !== inversion || c.octave !== octave) changed = true;
      c.tick = tick; c.duration = dur; c.id = id; c.track = track;
      c.degree = degree; c.quality = quality; c.inversion = inversion;
      c.octave = octave; c.flags = v.getUint32(o + 28, true);
      // The row LaneGrid put it on, computed by the sidecar in the grid it was
      // last told about — the same axis the notes ride. The frontend never
      // re-derives the projection; a client that computed its own put material
      // three beats out with no error anywhere.
      c.row = v.getUint32(o + 32, true);
      const spread = v.getUint32(o + 36, true);
      const ht = v.getUint16(o + 40, true);
      const hv = v.getUint16(o + 42, true);
      if (c.spread !== spread || c.humanizeTiming !== ht
          || c.humanizeVelocity !== hv) changed = true;
      c.spread = spread; c.humanizeTiming = ht; c.humanizeVelocity = hv;
    }
    store.chordCount = have;
    // A revision, like the notes have. What draws chords needs to know when they
    // moved without comparing the list, and the clip version does not separate a
    // note edit from a chord one.
    if (changed) store.chordsRevision++;
    store.chordsEnd = at + have * CHORD_BYTES;
  }

  /*
   * PER-INSERT METERS, and now THESE are the last section.
   *
   * No revision and no change detection, unlike the chords: these move EVERY
   * frame by design — that is what a meter is — so comparing them to decide
   * whether anything changed would be work done to always answer yes.
   *
   * Bounds-checked like the two blocks above and for the same reason: a frame
   * from an older sidecar simply stops before this, and reading past the end of
   * a DataView throws, which would take down the socket handler and the whole UI
   * with it to report a field that is merely absent.
   */
  {
    const at = store.chordsEnd;
    const want = v.getUint16(184, true);
    const have = Math.max(0, Math.min(want, (buf.byteLength - at) / METER_BYTES | 0));
    while (store.meters.length < have) {
      store.meters.push({ track: 0, device: 0, inPeak: 0, outPeak: 0, inRms: 0, outRms: 0 });
    }
    for (let i = 0; i < have; i++) {
      const o = at + i * METER_BYTES;
      const m = store.meters[i];
      // The track ID, not the slot the engine indexes its region by — the
      // sidecar translates. The two diverge the moment a track is removed, and
      // the page keys everything on ids.
      m.track = v.getUint32(o, true);
      m.device = v.getUint32(o + 4, true);
      m.inPeak = v.getInt16(o + 8, true);
      m.outPeak = v.getInt16(o + 10, true);
      m.inRms = v.getInt16(o + 12, true);
      m.outRms = v.getInt16(o + 14, true);
    }
    store.meterCount = have;
    store.metersEnd = at + have * METER_BYTES;
  }

  /*
   * PER-LANE QUANTIZE, and now THIS is the last section.
   *
   * Bounds-checked like every block above it and for the same reason: a frame from
   * an older sidecar simply stops before this, and reading past the end of a
   * DataView throws — which would take down the socket handler and the whole UI
   * with it, to report a field that is merely absent.
   */
  {
    const at = store.metersEnd;
    const want = v.getUint16(192, true);
    const have = Math.max(0, Math.min(want, (buf.byteLength - at) / QUANTIZE_BYTES | 0));
    while (store.quantize.length < have) {
      store.quantize.push({ track: 0, grid: 0, strength: 0, swing: 0 });
    }
    for (let i = 0; i < have; i++) {
      const o = at + i * QUANTIZE_BYTES;
      const q = store.quantize[i];
      q.track = v.getUint32(o, true);
      q.strength = v.getUint32(o + 4, true);
      q.swing = v.getInt32(o + 8, true);
      // A tick count, so a u64 — but Number is exact to 2^53 and a bar is
      // 3,840,000, so no song reaches the range where this would lose a tick.
      q.grid = Number(v.getBigUint64(o + 12, true));
    }
    store.quantizeCount = have;
    // Read at the top of this function, before the note gate compared it — assigned
    // here so the comparison above sees LAST frame's value, which is the whole
    // point of a version gate.
    store.quantizeVersion = quantizeVersion;
    store.quantizeEnd = at + have * QUANTIZE_BYTES;
  }

  /*
   * THE SECTION SPINE, and now THIS is the last section of the frame.
   *
   * Bounds-checked like every block before it: a frame from an older sidecar simply stops
   * here, and reading past a DataView throws — which would take the socket handler and the
   * whole UI down to report a field that is merely absent.
   *
   * Copied only when the arrange GENERATION moves. It moves on a spine or meter change and
   * never on a note edit, so this is one integer compare per frame while somebody types.
   */
  {
    const at = store.quantizeEnd;
    const want = v.getUint16(196, true);
    const generation = v.getUint32(200, true);
    const have = Math.max(0, Math.min(want, (buf.byteLength - at) / MARKER_BYTES | 0));
    store.songEnd = Number(v.getBigUint64(204, true));
    store.markersTruncated = v.getUint16(198, true);
    if (generation !== store.arrangeVersion || have !== store.markerCount) {
      store.arrangeVersion = generation;
      while (store.markers.length < have) {
        store.markers.push({ id: 0, bar: 1, beat: 1, color: 0, tick: 0, name: '',
                             endTick: 0, bars: 0 });
      }
      for (let i = 0; i < have; i++) {
        const o = at + i * MARKER_BYTES;
        const m = store.markers[i];
        m.id = v.getUint32(o, true);
        m.bar = v.getUint32(o + 4, true);
        m.beat = v.getUint32(o + 8, true);
        m.color = v.getUint32(o + 12, true);
        m.tick = Number(v.getBigUint64(o + 16, true));
        // 24..32 is reserved by the engine.
        // Nul-PADDED, not nul-terminated: a 24-character name carries no terminator, so the
        // scan is bounded by the field and stops at the first nul.
        let name = '';
        for (let k = 0; k < 24; k++) {
          const c = v.getUint8(o + 32 + k);
          if (c === 0) break;
          name += String.fromCharCode(c);
        }
        m.name = name;
      }
      /*
       * THE SPAN EACH MARKER BEGINS, in a second pass because it needs the NEXT marker.
       *
       * The last one runs to `songEnd` — the furthest placement — which is not marker-derived
       * and can sit BEFORE the marker if somebody put a marker past the music. Clamped, so a
       * span is never negative: a negative width draws as nothing, which reads as a missing
       * marker rather than as one past the end.
       */
      for (let i = 0; i < have; i++) {
        const m = store.markers[i];
        const end = i + 1 < have ? store.markers[i + 1].tick : store.songEnd;
        m.endTick = Math.max(m.tick, end);
        m.bars = i + 1 < have ? Math.max(0, store.markers[i + 1].bar - m.bar) : 0;
      }
      /*
       * ...AND BLANK THE TAIL. `markers` is a POOL: it grows and is never shrunk, because
       * shrinking allocates in the frame loop — so after a removal the array is longer than the
       * count and its last entry is the marker just deleted, name and position intact. Readers
       * must stop at `markerCount`; blanking makes forgetting VISIBLE (id 0, no name) rather
       * than plausible.
       */
      for (let i = have; i < store.markers.length; i++) {
        const m = store.markers[i];
        m.id = 0; m.bar = 1; m.beat = 1; m.color = 0; m.tick = 0; m.name = '';
        m.endTick = 0; m.bars = 0;
      }
      store.markerCount = have;
    }
    // Where the markers END, so the automation block after them needs no second scan.
    store.markersEnd = at + have * MARKER_BYTES;
  }

  /*
   * The automation LANES, after the sections. Same discipline as everything else on this wire:
   * the pool grows and is never shrunk, the count is the authority, and the TAIL IS BLANKED so
   * a reader that forgets the count sees an obviously-empty lane rather than a convincing
   * stale one.
   */
  {
    const at = store.markersEnd;
    const want = v.getUint16(220, true);
    const version = v.getUint32(224, true);
    /*
     * THE SAMPLER KIT'S VERSION — bumped when a kit CHANGES, not when one is requested.
     *
     * That distinction is what makes it usable: "did anyone ask recently" is not the question a
     * drawn kit has, "is what I drew still right" is. A counter that ticked on request would
     * make a polling reader re-fetch for ever and look correct while doing it.
     *
     * ZERO means the engine does not publish one — the counter starts at 1 — so an older engine
     * is distinguishable from an unchanged kit without a second field to key on.
     *
     * It is GLOBAL, not per track: it says something changed, not which device. With one
     * sampler that is exact; with several it costs a re-fetch of kits that did not move, which
     * is the trade backend offered to revisit when it can be measured hurting.
     */
    store.samplerKitVersion = v.getUint32(228, true);
    const have = Math.max(0, Math.min(want, (buf.byteLength - at) / AUTOMATION_BYTES | 0));
    store.automationTruncated = v.getUint16(222, true);
    if (version !== store.automationVersion || have !== store.automationCount) {
      store.automationVersion = version;
      while (store.automation.length < have) {
        store.automation.push({ track: 0, target: 0, points: 0, discrete: false, param: '' });
      }
      for (let i = 0; i < have; i++) {
        const o = at + i * AUTOMATION_BYTES;
        const l = store.automation[i];
        l.track = v.getUint32(o, true);
        l.target = v.getUint32(o + 4, true);
        l.points = v.getUint32(o + 8, true);
        l.discrete = (v.getUint32(o + 12, true) & 1) !== 0;
        // Nul-PADDED to 16, like every other name here: a 16-character id carries no
        // terminator, so the scan is bounded by the field.
        let name = '';
        for (let k = 0; k < 16; k++) {
          const c = v.getUint8(o + 16 + k);
          if (c === 0) break;
          name += String.fromCharCode(c);
        }
        l.param = name;
      }
      for (let i = have; i < store.automation.length; i++) {
        const l = store.automation[i];
        l.track = 0; l.target = 0; l.points = 0; l.discrete = false; l.param = '';
      }
      store.automationCount = have;
    }
  }

  store.ok = true;
  return true;
}

/** 40 bytes each; see the sidecar's encode. */
/*
 * 48 SINCE WIRE 28. It was 40, and the eight new bytes carry the STRUM: the spread and
 * the two humanize amounts. The engine has published all three on every frame since
 * UiClipChord had them, and this decoder dropped them — so "is this chord a strum or a
 * block chord" was unanswerable from the interface, while the sidecar could WRITE all
 * three the whole time. A field the engine sends, the model persists and nothing reads
 * is the same defect as one nothing can write; it just fails in the other direction.
 */
const CHORD_BYTES = 48;
/** 16 bytes each; see the sidecar's encode. */
const METER_BYTES = 16;
/** 24 bytes each; see the sidecar's encode. */
const QUANTIZE_BYTES = 24;
/** 56 bytes each; see the sidecar's encode. */
/** UiMarker on the wire: id, bar, beat, color, nanotick, reserved, name[24]. */
const MARKER_BYTES = 56;
/** UiAutomationLane on the wire: track, target, points, flags, paramId[16]. */
const AUTOMATION_BYTES = 32;
/** kUiMeterSilent — silent or below the floor, NOT "no reading". */
export const METER_SILENT = -32768;

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
export const WAVE_WIRE_VERSION = 2;
const WAVE_HEADER_BYTES = 60;

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
  /*
   * WHICH SAMPLER, when `flags` bit 1 says the source id is a sampler's LOCAL id.
   *
   * `(trackId << 16) | deviceId`. It is part of the cache KEY, not decoration: the answer
   * echoes the local id it was asked about, and a local id is a per-device counter, so two
   * samplers holding different files at local id 1 would be one entry and the second pad would
   * draw the first one's audio. Zero when the flag is clear, which is every clip answer.
   */
  out.samplerAddr = (out.flags & 2) !== 0 ? v.getUint32(56, true) : 0;
  const want = out.columns * out.channels * 2;
  const have = (buf.byteLength - WAVE_HEADER_BYTES) >> 1;
  // A short payload means the header and the body disagree, which is the stride
  // bug this codebase has shipped twice. Refuse rather than read past the end and
  // draw whatever is there.
  if (have < want) { out.status = WAVE_BAD; out.pairs = null; return out; }
  out.pairs = new Int16Array(buf, WAVE_HEADER_BYTES, want);
  return out;
}

/**
 * THE CACHE KEY FOR ONE WAVEFORM WINDOW, in one place.
 *
 * Three call sites need it — the requester, the answer handler, and the painter that looks a
 * window up — and it lived as three separate template strings. Two of them were updated when the
 * sampler address joined the key and the third was not, so the painter spent an evening looking
 * up `1:1024:0` while the answer sat in the cache under `1@131073:1024:0`. Every instrument said
 * "the window has not arrived"; it had.
 *
 * It belongs in this file because the key is a statement about the WIRE — it names exactly the
 * fields an answer carries that identify which window it is — and a fourth caller should have to
 * find it here rather than invent a fourth spelling.
 */
export function waveKey(sourceId, samplerAddr, decimation, firstFrame) {
  return sourceId + '@' + (samplerAddr | 0) + ':' + decimation + ':' + firstFrame;
}

/** A reusable decode target for waveform answers. */
export function createWaveform() {
  return { status: WAVE_NOTREADY, requestSeq: 0, sourceId: 0, samplerAddr: 0, keyLo: 0, keyHi: 0,
           decimation: 0, columns: 0, channels: 0, firstFrame: 0, frameCount: 0,
           flags: 0, pairs: null };
}
