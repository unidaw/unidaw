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
/** @typedef {{id:number, track:number, startRow:number, endRow:number, name:string, active:boolean}} Clip */
/** @typedef {{id:number, name:string, columns:number}} Track */

export const ZOOM_LEVELS = [
  { index: 0, rowNanoticks: 240000, label: '1/16', aggregate: false },
  { index: 1, rowNanoticks: 480000, label: '1/8', aggregate: false },
  { index: 2, rowNanoticks: 960000, label: '1/4', aggregate: false },
  { index: 3, rowNanoticks: 3840000, label: '1 bar', aggregate: true },
  { index: 4, rowNanoticks: 15360000, label: '4 bars', aggregate: true },
];

const NOTES = ['C-4', 'D#4', 'F-4', 'G-4', 'A#4', 'C-5', 'E-4', 'OFF'];

const TICKS_PER_BAR = 3840000;

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
 * Build a view-model for a window of the timeline.
 * @param {{startRow:number, rowCount:number, tracks:number, columns:number,
 *          zoomIndex?:number, cursor?:{row:number,track:number,col:number},
 *          playheadRow?:number, selection?:any}} opts
 */
export function buildViewModel(opts) {
  const {
    startRow, rowCount, tracks: trackCount, columns = 3,
    zoomIndex = 2, cursor = { row: startRow, track: 0, col: 0 },
    playheadRow = startRow, selection = null,
  } = opts;

  const zoom = ZOOM_LEVELS[zoomIndex];
  const tickOf = (r) => r * zoom.rowNanoticks;

  /** @type {Track[]} */
  const tracks = Array.from({ length: trackCount }, (_, i) => ({
    id: i, name: `T${String(i + 1).padStart(2, '0')}`, columns,
  }));

  /** @type {Row[]} */
  const rows = [];
  for (let r = startRow; r < startRow + rowCount; r++) {
    /** @type {Cell[]} */
    const cells = [];
    const tick = tickOf(r);
    for (let t = 0; t < trackCount; t++) {
      for (let c = 0; c < columns; c++) {
        if (zoom.aggregate && c === 0) {
          // A coarse row spans many finer rows; count the events that fall in it.
          const span = zoom.rowNanoticks / ZOOM_LEVELS[0].rowNanoticks;
          let n = 0;
          for (let k = 0; k < span && k < 64; k++) {
            if (contentAt(tick + k * ZOOM_LEVELS[0].rowNanoticks, t, c).kind !== 'empty') n++;
          }
          if (n > 1) cells.push({ track: t, col: c, text: `[${n}x]`, kind: 'aggregate' });
          else { const { text, kind } = contentAt(tick, t, c); cells.push({ track: t, col: c, text, kind }); }
        } else {
          const { text, kind } = contentAt(tick, t, c);
          cells.push({ track: t, col: c, text, kind });
        }
      }
    }
    // Labels come from the tick too, so a row means the same musical position
    // at every zoom — that is the whole point of zoom being a projection.
    const bar = Math.floor(tick / TICKS_PER_BAR) + 1;
    const beatInBar = Math.floor((tick % TICKS_PER_BAR) / (TICKS_PER_BAR / 4)) + 1;
    const sub = Math.floor((tick % (TICKS_PER_BAR / 4)) / 60000);
    rows.push({
      index: r,
      label: zoom.rowNanoticks >= TICKS_PER_BAR ? `${bar}` : `${bar}:${beatInBar}${sub ? ':' + String(sub).padStart(2, '0') : ''}`,
      beat: tick % (TICKS_PER_BAR / 4) === 0,
      bar: tick % TICKS_PER_BAR === 0,
      cells,
    });
  }

  // Clips overlapping the window. They span rows, which is why the renderer
  // draws rails outside the recycled row band.
  /** @type {Clip[]} */
  const clips = [];
  const CLIP_LEN = Math.max(4, Math.round((TICKS_PER_BAR * 4) / zoom.rowNanoticks));
  const first = Math.floor(startRow / CLIP_LEN) - 1;
  for (let k = first; k <= Math.floor((startRow + rowCount) / CLIP_LEN); k++) {
    if (k < 0) continue;
    for (let t = 0; t < trackCount; t++) {
      if ((k + t) % 3 === 0) continue; // gaps
      clips.push({
        id: k * 100 + t, track: t,
        startRow: k * CLIP_LEN, endRow: k * CLIP_LEN + CLIP_LEN - 4,
        name: `clip ${k}.${t}`, active: (k + t) % 7 === 0,
      });
    }
  }

  return { window: { startRow, rowCount }, zoom, tracks, rows, clips, cursor, playhead: { row: playheadRow }, selection };
}
