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

/**
 * Deterministic pseudo-content for any absolute row index, so the fixture can
 * describe a 100,000-row timeline without materialising one. Same row index
 * always yields the same cell, which is what makes golden screenshots stable.
 */
function contentAt(row, track, col) {
  const h = Math.abs(Math.sin((row * 37 + track * 101 + col * 7) * 0.61803) * 10000) | 0;
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
  const rowsPerBar = Math.max(1, Math.round(3840000 / zoom.rowNanoticks));

  /** @type {Track[]} */
  const tracks = Array.from({ length: trackCount }, (_, i) => ({
    id: i, name: `T${String(i + 1).padStart(2, '0')}`, columns,
  }));

  /** @type {Row[]} */
  const rows = [];
  for (let r = startRow; r < startRow + rowCount; r++) {
    /** @type {Cell[]} */
    const cells = [];
    for (let t = 0; t < trackCount; t++) {
      for (let c = 0; c < columns; c++) {
        if (zoom.aggregate && c === 0) {
          // At coarse zoom a row covers many events; show how many collapsed into it.
          const n = 1 + (Math.abs(Math.sin((r * 13 + t) * 2.399) * 10) | 0) % 4;
          cells.push({ track: t, col: c, text: n > 1 ? `[${n}x]` : contentAt(r, t, c).text, kind: n > 1 ? 'aggregate' : 'note' });
        } else {
          const { text, kind } = contentAt(r, t, c);
          cells.push({ track: t, col: c, text, kind });
        }
      }
    }
    rows.push({
      index: r,
      label: `${Math.floor(r / rowsPerBar) + 1}:${(r % rowsPerBar) + 1}`,
      beat: r % Math.max(1, rowsPerBar / 4) === 0,
      bar: r % rowsPerBar === 0,
      cells,
    });
  }

  // Clips overlapping the window. They span rows, which is why the renderer
  // draws rails outside the recycled row band.
  /** @type {Clip[]} */
  const clips = [];
  const CLIP_LEN = 48;
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
