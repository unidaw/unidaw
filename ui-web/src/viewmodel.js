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
export const ZOOM_LEVELS = [
  { index: 0, rowNanoticks: 240000, linesPerBeat: 4, label: '1/16', aggregate: false },
  { index: 1, rowNanoticks: 480000, linesPerBeat: 2, label: '1/8', aggregate: false },
  { index: 2, rowNanoticks: 960000, linesPerBeat: 1, label: '1/4', aggregate: false },
  { index: 3, rowNanoticks: 3840000, linesPerBeat: 1, label: '1 bar', aggregate: true },
  { index: 4, rowNanoticks: 15360000, linesPerBeat: 1, label: '4 bars', aggregate: true },
];

const NOTES = ['C-4', 'D#4', 'F-4', 'G-4', 'A#4', 'C-5', 'E-4', 'OFF'];

const TICKS_PER_BAR = 3840000;
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
    zoom: ZOOM_LEVELS[0], tracks: [], rows, clips: [],
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

  const rows = buf.rows;
  for (let ri = 0; ri < rowCount; ri++) {
    const r = startRow + ri;
    const cells = rows[ri].cells;
    let ci = 0;
    const tick = tickOf(r);
    for (let t = 0; t < trackCount; t++) {
      const inClip = engine ? true : clipIndexAt(tick, t) >= 0;
      for (let c = 0; c < columns; c++) {
        if (engine) { const cl = cells[ci++]; cl.text = ''; cl.kind = 'empty'; cl.aggCount = 0; continue; }
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
    const sub = Math.floor((tick % (TICKS_PER_BAR / 4)) / 60000);
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
    for (let i = 0; i < engine.noteCount; i++) {
      const n = engine.notes[i];
      if (n.tOn < winStart || n.tOn >= winEnd || n.track >= trackCount) continue;
      // n.row was computed by LaneGrid on the sidecar. Fall back to arithmetic
      // only for the fine grids where the two agree, so a mismatch is visible
      // rather than silently papered over.
      const ri = n.row >= startRow ? n.row - startRow : ((n.tOn - winStart) / span) | 0;
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
    for (let t = 0; t < Math.min(trackCount, engine.aggTracks); t++) {
      for (let ri = 0; ri < rowCount && ri < engine.aggRows; ri++) {
        const a = t * engine.aggRows + ri;
        const count = engine.aggCount[a];
        if (!count) continue;
        const cell = rows[ri].cells[t * columns];
        if (!cell) continue;
        cell.text = '';
        cell.kind = 'contour';
        cell.aggCount = count;
        cell.aggLo = engine.aggLo[a];
        cell.aggHi = engine.aggHi[a];
      }
    }
  }

  // Both revisions, because either can change what the cells say while every row
  // index stays put — and the renderer's identity check cannot see either.
  buf.contentRevision = engine
    ? engine.notesRevision * 1e6 + engine.aggRevision
    : zoomIndex;

  buf.window.startRow = startRow; buf.window.rowCount = rowCount;
  buf.zoom = zoom;
  buf.cursor.row = cursor.row; buf.cursor.track = cursor.track; buf.cursor.col = cursor.col;
  buf.playhead.row = playheadRow;
  buf.selection = selection;
  return buf;
}
