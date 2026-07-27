import { pitchName } from './wire.js';
// The view-model: plain data describing exactly what is on screen right now.
//
// This is the boundary the whole frontend is built around. The renderer consumes
// it and nothing else; tests assert on it; an agent reads it via window.__uni.
// Swapping the renderer (DOM -> canvas -> a native kit) means reimplementing one
// consumer of this shape, not re-deriving the projection.
//
// Uni's timeline is UNBOUNDED and zoom decides how much detail a row carries, so
// a view-model instance only ever describes the visible window plus the clips
// that overlap it. There is no "whole document" representation, by construction.

/** @typedef {{track:number, col:number, text:string, kind:'note'|'inst'|'fx'|'empty'|'off'|'aggregate', pending?:boolean}} Cell */
/** @typedef {{index:number, label:string, beat:boolean, bar:boolean, cells:Cell[]}} Row */
/**
 * Clips live in TIMELINE coordinates, never in rows. A row index is a function
 * of the current zoom, so storing one would bake this frame's zoom into durable
 * data and the clip would move when the user zoomed. The renderer projects
 * ticks to rows itself. endTick is exclusive.
 * @typedef {{id:number, track:number, startTick:number, endTick:number, name:string, active:boolean}} Clip
 */
/** @typedef {{id:number, name:string, columns:number}} Track */

// linesPerBeat is what LaneGrid takes, so the sidecar can build the same grid
// we are describing. It is not restricted to powers of two — 3 is a triplet
// grid, which is exactly the case JS tick/rowNanoticks division cannot express.
/** Smallest grid that can represent all of `lpbs` exactly. */
export function lcmGrid(lpbs) {
  const gcd = (a, b) => (b ? gcd(b, a % b) : a);
  let n = 1;
  for (const v of lpbs) if (v > 0) n = (n * v) / gcd(n, v);
  return Math.min(n, 24);   // 24/beat is already 96 rows a bar; past that, aggregate
}

export const ZOOM_LEVELS = [
  // The polyrhythm level: rows fine enough that a 4-, 3- and 6-per-beat lane can
  // each land exactly. A lane occupies every (grid / itsLpb)-th row and the rest
  // are off ITS grid — which is what makes the polymeter visible rather than
  // collapsed. See GUIDELINES 2 on rows being a projection.
  { index: 0, rowNanoticks: 80000, linesPerBeat: 12, label: '1/48', aggregate: false },
  { index: 1, rowNanoticks: 240000, linesPerBeat: 4, label: '1/16', aggregate: false },
  { index: 2, rowNanoticks: 480000, linesPerBeat: 2, label: '1/8', aggregate: false },
  { index: 3, rowNanoticks: 960000, linesPerBeat: 1, label: '1/4', aggregate: false },
  { index: 4, rowNanoticks: 3840000, linesPerBeat: 1, label: '1 bar', aggregate: true },
  { index: 5, rowNanoticks: 15360000, linesPerBeat: 1, label: '4 bars', aggregate: true },
];

const NOTES = ['C-4', 'D#4', 'F-4', 'G-4', 'A#4', 'C-5', 'E-4', 'OFF'];

const TICKS_PER_BAR = 3840000;
const TICKS_PER_BEAT = 960000;
const CLIP_TICKS = TICKS_PER_BAR * 4;                 // fixture: 4-bar clips
const CLIP_BODY = CLIP_TICKS - TICKS_PER_BAR / 4;     // ...with a beat of gap after

/**
 * Clips are the container for notes, not a decoration over them: wherever a
 * note exists there is a clip, no note exists outside one, and the space
 * between clips is genuinely empty. So coverage has to be decided BEFORE
 * content, or the grid shows notes floating in gaps with no rail around them.
 *
 * O(1) and allocation-free — the fixture lays clips on a regular grid, so
 * coverage is arithmetic rather than a search. Real placements will need an
 * interval lookup here, but it must stay allocation-free in the draw path.
 */
function clipIndexAt(tick, track) {
  if (tick < 0) return -1;
  const k = Math.floor(tick / CLIP_TICKS);
  if ((k + track) % 3 === 0) return -1;               // gap between clips
  return (tick - k * CLIP_TICKS) < CLIP_BODY ? k : -1;
}

/**
 * Deterministic pseudo-content for a point on the TIMELINE, keyed on the tick a
 * row represents rather than on its index.
 *
 * Keying on the row index instead was a real bug: content then never changed
 * with zoom, so "1 bar" and "4 bars" rendered pixel-identical and half the zoom
 * presses did nothing visible. It reads as lag — the UI appears not to respond —
 * when in fact the frame was delivered in 15 ms and simply had nothing new in it.
 * Rows are a projection of the timeline; content must follow the tick.
 */
function contentAt(tick, track, col) {
  const h = Math.abs(Math.sin((tick / 60000 + track * 101 + col * 7) * 0.61803) * 10000) | 0;
  if (h % 5 < 2) return { text: '', kind: 'empty' };
  if (h % 23 === 0) return { text: 'OFF', kind: 'off' };
  if (col === 0) return { text: NOTES[h % NOTES.length], kind: 'note' };
  if (col === 1) return { text: String(h % 100).padStart(2, '0'), kind: 'inst' };
  return { text: (h % 256).toString(16).toUpperCase().padStart(2, '0'), kind: 'fx' };
}

/**
 * Allocate a reusable view-model buffer. Call once per shape change, then pass
 * it to buildViewModel to be filled in place.
 *
 * The naive version allocated a fresh object graph every draw — ~74 rows and
 * ~1,776 cell objects, over 100k objects/second at 60 fps. That is enough GC
 * pressure to show up as an occasional two-frame stall on a held key, which is
 * exactly the tail we measured (ArrowDown p50 9.5 ms, p95 30.7 ms).
 *
 * Callers double-buffer: the renderer compares the previous view-model against
 * the current one to decide what to rebind, so `prev` and `vm` must be distinct
 * objects. Alternating two buffers keeps that comparison meaningful at zero
 * steady-state allocation.
 */
/**
 * Shared across buffers on purpose. The caller double-buffers so the renderer can
 * diff current against previous, which means a per-buffer signature would be
 * comparing this frame against TWO frames ago and miss a change that landed in
 * between. The signature describes the world, not a particular buffer.
 */
const SIG = { zoomIndex: -1, pendingCount: -1, overlayLen: -1,
              notesRevision: -2, aggRevision: -2, rowGrid: -2 };
let contentRevision = 0;

export function createBuffer(rowCount, trackCount, columns) {
  const rows = new Array(rowCount);
  for (let i = 0; i < rowCount; i++) {
    const cells = new Array(trackCount * columns);
    for (let t = 0, k = 0; t < trackCount; t++)
      for (let c = 0; c < columns; c++)
        cells[k++] = { track: t, col: c, text: '', kind: 'empty',
                       // Aggregate for this cell at coarse zoom. count 0 = none.
                       aggCount: 0, aggLo: 0, aggHi: 0 };
    rows[i] = { index: 0, label: '', beat: false, bar: false, cells };
  }
  return {
    window: { startRow: 0, rowCount },
    zoom: ZOOM_LEVELS[0], tracks: [], rows, clips: [], harmony: [], _harmonyPool: [],
    cursor: { row: 0, track: 0, col: 0 }, playhead: { row: 0 }, selection: null,
    _shape: `${rowCount}x${trackCount}x${columns}`,
    _clipPool: [],
  };
}

/**
 * Build a view-model for a window of the timeline.
 * @param {{startRow:number, rowCount:number, tracks:number, columns:number,
 *          zoomIndex?:number, cursor?:{row:number,track:number,col:number},
 *          playheadRow?:number, selection?:any}} opts
 */
export function buildViewModel(opts, buf) {
  const {
    startRow, rowCount, tracks: trackCount, columns = 3,
    zoomIndex = 2, cursor = { row: startRow, track: 0, col: 0 },
    playheadRow = startRow, selection = null,
    // When present, cells come from the engine instead of the fixture. The
    // fixture stays because goldens must render without a running engine.
    engine = null,
    // The cell currently being typed into, if any. Drawn over whatever the
    // engine says is there — you must see what you are typing before it commits.
    entryOverlay = null,
    // Optimistic edits, drawn over the engine's answer until it catches up.
    pending = null, pendingCount = 0,
    /**
     * The engine's harmony timeline, [{tick, root, scaleId}], for the column
     * between the time gutter and the first track.
     *
     * The tracker showed no harmony at all, which for a program whose whole
     * point is that notes are degrees against a field is a strange omission —
     * the key was in the chrome, once, for the playhead. What matters while you
     * type is the harmony HERE, on this row, which is a different question the
     * moment the playhead is somewhere else.
     */
    harmony = null,
    /** How a root pitch class and a scale id are named. Supplied by the caller
     *  so this module needs no opinion about note spelling. */
    nameHarmony = null,
  } = opts;

  const shape = `${rowCount}x${trackCount}x${columns}`;
  if (!buf || buf._shape !== shape) buf = createBuffer(rowCount, trackCount, columns);

  const zoom = ZOOM_LEVELS[zoomIndex];
  const tickOf = (r) => r * zoom.rowNanoticks;

  if (buf.tracks.length !== trackCount) {
    buf.tracks = Array.from({ length: trackCount }, (_, i) => ({
      id: i, name: `T${String(i + 1).padStart(2, '0')}`, columns,
    }));
  }

  /**
   * Harmony as SPANNING BLOCKS, not per-row cells.
   *
   * A field is a span, and the first version of this drew it as a label on the
   * row the change lands on. That reads correctly from the top of a song and
   * says NOTHING once you scroll into the middle of a field — which is most of
   * the time you are actually working. The block carries its own extent, and the
   * renderer keeps the label pinned to the visible top of it, so "what key am I
   * in" is answerable from anywhere in the span.
   *
   * Blocks live outside the recycled row band for the same reason clip rails do.
   */
  const blocks = buf.harmony;
  blocks.length = 0;
  if (harmony && harmony.length) {
    const winStart = tickOf(startRow);
    const winEnd = tickOf(startRow + rowCount);
    const hpool = buf._harmonyPool;
    let bn = 0;
    for (let i = 0; i < harmony.length; i++) {
      const ev = harmony[i];
      const from = ev.tick;
      // The last field runs to the end of time, not to the end of the timeline.
      const to = i + 1 < harmony.length ? harmony[i + 1].tick : Number.MAX_SAFE_INTEGER;
      if (to <= winStart || from >= winEnd) continue;
      const b = hpool[bn] || (hpool[bn] = { key: '', label: '', sub: '', foot: '',
                                            startTick: 0, endTick: 0 });
      bn++;
      const named = nameHarmony ? nameHarmony(ev) : { label: '', sub: '' };
      b.key = ev.tick + ':' + ev.root + ':' + ev.scaleId;
      b.label = named.label;
      b.sub = named.sub;
      // The design pins a root/quant line to the bottom of the span. Root is
      // the pitch class the engine published; quant is not published, so it is
      // not claimed.
      b.foot = 'root ' + ev.root;
      b.startTick = from;
      b.endTick = Math.min(to, winEnd + zoom.rowNanoticks);
      blocks.push(b);
    }
  }

  const rows = buf.rows;
  for (let ri = 0; ri < rowCount; ri++) {
    const r = startRow + ri;
    const cells = rows[ri].cells;
    let ci = 0;
    const tick = tickOf(r);


    for (let t = 0; t < trackCount; t++) {
      const inClip = engine ? true : clipIndexAt(tick, t) >= 0;
      for (let c = 0; c < columns; c++) {
        if (engine) {
          const cl = cells[ci++];
          cl.text = ''; cl.aggCount = 0;
          // A lane only has rows where its own grid lands. Elsewhere there is no
          // cell to write into — showing an empty one would imply you could.
          const laneLpb = engine.lpb[t] || zoom.linesPerBeat;
          const stride = zoom.linesPerBeat / laneLpb;
          cl.kind = (stride >= 1 && r % Math.round(stride) !== 0) ? 'offgrid' : 'empty';
          continue;
        }
        const cell = cells[ci++];
        cell.aggCount = 0;
        if (!inClip) { cell.text = ''; cell.kind = 'outside'; continue; }
        if (zoom.aggregate && c === 0) {
          // A coarse row spans many finer rows; count the events that fall in it.
          const span = zoom.rowNanoticks / ZOOM_LEVELS[0].rowNanoticks;
          let n = 0;
          for (let k = 0; k < span && k < 64; k++) {
            const sub = tick + k * ZOOM_LEVELS[0].rowNanoticks;
            if (clipIndexAt(sub, t) >= 0 && contentAt(sub, t, c).kind !== 'empty') n++;
          }
          if (n > 1) { cell.text = `[${n}x]`; cell.kind = 'aggregate'; }
          else { const g = contentAt(tick, t, c); cell.text = g.text; cell.kind = g.kind; }
        } else {
          const g = contentAt(tick, t, c);
          cell.text = g.text; cell.kind = g.kind;
        }
      }
    }
    // Labels come from the tick too, so a row means the same musical position
    // at every zoom — that is the whole point of zoom being a projection.
    const bar = Math.floor(tick / TICKS_PER_BAR) + 1;
    const beatInBar = Math.floor((tick % TICKS_PER_BAR) / (TICKS_PER_BAR / 4)) + 1;
    // The sub-beat index is the row's position within the beat ON THIS GRID, not
    // in fixed 16ths. Hardcoding a 16th (60000nt) made a 12-per-beat grid label
    // its rows 0,1,2,4,5,6,8 — skipping some numbers and reusing others, so two
    // different rows could read as the same musical position.
    const sub = Math.round((tick % (TICKS_PER_BAR / 4)) / zoom.rowNanoticks);
    const row = rows[ri];
    row.index = r;
    row.label = zoom.rowNanoticks >= TICKS_PER_BAR ? `${bar}` : `${bar}:${beatInBar}${sub ? ':' + String(sub).padStart(2, '0') : ''}`;
    row.beat = tick % (TICKS_PER_BAR / 4) === 0;
    row.bar = tick % TICKS_PER_BAR === 0;
  }

  // Clips overlapping the window. They span rows, which is why the renderer
  // draws rails outside the recycled row band.
  const clips = buf.clips;
  clips.length = 0;
  const pool = buf._clipPool;
  let cn = 0;
  const winStart = tickOf(startRow), winEnd = tickOf(startRow + rowCount);
  const kFirst = Math.max(0, Math.floor(winStart / CLIP_TICKS));
  const kLast = Math.floor(winEnd / CLIP_TICKS);
  for (let k = kFirst; k <= kLast; k++) {
    for (let t = 0; t < trackCount; t++) {
      if ((k + t) % 3 === 0) continue; // gaps — must match clipIndexAt
      const cl = pool[cn] || (pool[cn] = { id: 0, track: 0, startTick: 0, endTick: 0, name: '', active: false });
      cn++;
      cl.id = k * 100 + t; cl.track = t;
      cl.startTick = k * CLIP_TICKS;
      cl.endTick = (k + 1) * CLIP_TICKS - TICKS_PER_BAR / 4;   // exclusive
      cl.name = `clip ${k}.${t}`; cl.active = (k + t) % 7 === 0;
      clips.push(cl);
    }
  }

  // Place engine notes into the cleared grid. A note lands on the row whose tick
  // range contains it, so the projection follows zoom for free — the same reason
  // anything durable is stored in ticks rather than rows.
  if (engine) {
    const span = zoom.rowNanoticks;
    const winStart = tickOf(startRow);
    const winEnd = tickOf(startRow + rowCount);
    // 1 whenever the sidecar is already projecting at our grid, which is the
    // steady state; only a zoom in flight makes it anything else.
    const gridScale = engine.rowGrid > 0 ? zoom.linesPerBeat / engine.rowGrid : 1;
    // At an aggregate zoom the contour IS the representation: a row is bars
    // wide, so no individual note belongs in a cell. Drawing them anyway put
    // note names at beat positions on top of bar rows — every one of them in the
    // wrong place, and looking exactly like a tracker.
    for (let i = 0; !zoom.aggregate && i < engine.noteCount; i++) {
      const n = engine.notes[i];
      if (n.tOn < winStart || n.tOn >= winEnd || n.track >= trackCount) continue;
      // n.row was computed by LaneGrid on the sidecar, in the grid the sidecar
      // was last told about. Between a zoom keypress and the first frame built
      // at the new grid, that is the OLD grid — the rows are stale by exactly
      // the ratio of the two. Rescaling closes that window: the notes move with
      // the labels on the very next frame instead of a round-trip later, when
      // they were briefly sitting at wrong timecodes and looking authoritative.
      const scaled = gridScale === 1 ? n.row : Math.round(n.row * gridScale);
      const ri = scaled >= startRow ? scaled - startRow : ((n.tOn - winStart) / span) | 0;
      const row = rows[ri];
      if (!row) continue;
      const base = n.track * columns;
      const c0 = row.cells[base];
      if (c0) {
        // Two notes can legitimately land on one (row, track) once placements can
        // overlap — M3 allows it. Silently letting the second overwrite the first
        // is the contentAt bug again: a position key losing data while rendering
        // something plausible. Make the collision visible instead.
        if (c0.kind === 'note' && c0._row === ri) { c0.text = '**'; c0.kind = 'collide'; }
        else {
          c0.text = pitchName(n.pitch);
          // Muted base notes still ship — draw them struck out. Adds carry
          // provenance so an override reads differently from the shared clip.
          c0.kind = n.muted ? 'muted' : n.isAdd ? 'add' : 'note';
          c0._row = ri;
        }
      }
      if (columns > 1) {
        const c1 = row.cells[base + 1];
        if (c1) { c1.text = ('0' + n.velocity).slice(-2); c1.kind = 'inst'; }
      }
      if (columns > 2 && (n.retrigger || n.probability || n.delayTicks)) {
        const c2 = row.cells[base + 2];
        if (c2) {
          c2.text = n.retrigger ? 'R' + n.retrigger : n.probability ? 'P' + n.probability : 'D';
          c2.kind = 'fx';
        }
      }
    }
    // Real placements from the engine (v11). One per track today because the
    // engine plays the first placement; the rail code is the same when M3.3
    // brings several per track.
    const pool = buf._clipPool;
    clips.length = 0;
    for (let i = 0; i < engine.extentCount; i++) {
      const e = engine.extents[i];
      if (e.track >= trackCount) continue;
      const cl = pool[i] || (pool[i] = { id: 0, track: 0, startTick: 0, endTick: 0, name: '', active: false });
      cl.id = e.placementId; cl.track = e.track;
      cl.startTick = e.startTick; cl.endTick = e.endTick;
      cl.name = e.name; cl.active = e.placementId === cursor.placementId;
      clips.push(cl);
    }
  }

  // Bumped when the cells say something new for reasons the renderer's identity
  // check cannot see. Engine edits move notesRevision; the fixture is a pure
  // function of (tick, zoom) so it only changes when those do.
  // Coarse zoom: draw the engine's aggregate as a pitch-range mark rather than a
  // count. At four bars a row spans 64 sixteenths, so every count read 30-45 —
  // uniform noise down the column, carrying no information. pitch_min/pitch_max
  // says "this region is busy and rising", which a number cannot.
  if (engine && zoom.aggregate && engine.aggRows) {
    // The engine aggregates per BEAT (its grid cannot go coarser), so fold
    // `beatsPerRow` of its rows into each of ours. Counts add; the pitch range
    // is the union. Drawing its rows one-to-one against ours was wrong by a
    // factor of four at "1 bar" and sixteen at "4 bars", and looked fine.
    const beatsPerRow = Math.max(1, Math.round(zoom.rowNanoticks / TICKS_PER_BEAT));
    for (let t = 0; t < Math.min(trackCount, engine.aggTracks); t++) {
      for (let ri = 0; ri < rowCount; ri++) {
        let count = 0, lo = 127, hi = 0;
        for (let k = 0; k < beatsPerRow; k++) {
          const src = ri * beatsPerRow + k;
          if (src >= engine.aggRows) break;
          const a = t * engine.aggRows + src;
          const c = engine.aggCount[a];
          if (!c) continue;
          count += c;
          if (engine.aggLo[a] < lo) lo = engine.aggLo[a];
          if (engine.aggHi[a] > hi) hi = engine.aggHi[a];
        }
        if (!count) continue;
        const cell = rows[ri].cells[t * columns];
        if (!cell) continue;
        cell.text = '';
        cell.kind = 'contour';
        cell.aggCount = count;
        cell.aggLo = lo;
        cell.aggHi = hi;
      }
    }
  }

  // Both revisions, because either can change what the cells say while every row
  // index stays put — and the renderer's identity check cannot see either.
  for (let i = 0; i < pendingCount; i++) {
    const e = pending[i];
    const row = rows[e.row - startRow];
    if (!row) continue;
    const cell = row.cells[e.track * columns + e.col];
    if (cell) { cell.text = e.text; cell.kind = 'pending'; }
  }

  if (entryOverlay) {
    const ri = entryOverlay.row - startRow;
    const row = rows[ri];
    if (row) {
      const cell = row.cells[entryOverlay.track * columns + entryOverlay.col];
      if (cell) { cell.text = entryOverlay.text || '\u2588'; cell.kind = 'editing'; }
    }
  }

  // The renderer rebinds every visible cell when this changes, so it has to name
  // EVERY input that can alter cell text. It used to pack them into one number by
  // multiplying each by a power of ten; that silently broke twice — zoom was
  // never in the engine branch at all (so changing zoom left engine notes on
  // their old rows), and once notesRevision or rowGrid grew, the terms overlapped
  // past Number.MAX_SAFE_INTEGER and changes cancelled out. Comparing the inputs
  // costs the same and cannot alias.
  {
    const s = SIG;
    const overlayLen = entryOverlay ? entryOverlay.text.length + 1 : 0;
    if (s.zoomIndex !== zoomIndex || s.pendingCount !== pendingCount
        || s.overlayLen !== overlayLen
        || s.notesRevision !== (engine ? engine.notesRevision : -1)
        || s.aggRevision !== (engine ? engine.aggRevision : -1)
        || s.rowGrid !== (engine ? engine.rowGrid : -1)) {
      s.zoomIndex = zoomIndex; s.pendingCount = pendingCount; s.overlayLen = overlayLen;
      s.notesRevision = engine ? engine.notesRevision : -1;
      s.aggRevision = engine ? engine.aggRevision : -1;
      s.rowGrid = engine ? engine.rowGrid : -1;
      contentRevision++;
    }
  }
  buf.contentRevision = contentRevision;

  buf.window.startRow = startRow; buf.window.rowCount = rowCount;
  buf.zoom = zoom;
  buf.cursor.row = cursor.row; buf.cursor.track = cursor.track; buf.cursor.col = cursor.col;
  buf.playhead.row = playheadRow;
  buf.selection = selection;
  return buf;
}
