import { pitchName } from './wire.js';
import { DEFAULT_METER, createPosition, positionOf, sameMeter,
         ticksPerBar, NANOTICKS_PER_QUARTER } from './meter.js';
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

// The FIXTURE's clip geometry, pinned to 4/4 on purpose: it exists to lay out
// pseudo-clips for the goldens, and a fixture whose shape moved with the project's
// meter would make every golden a function of a setting the fixture does not have.
const FIXTURE_TICKS_PER_BAR = ticksPerBar(DEFAULT_METER);
const CLIP_TICKS = FIXTURE_TICKS_PER_BAR * 4;              // fixture: 4-bar clips
const CLIP_BODY = CLIP_TICKS - FIXTURE_TICKS_PER_BAR / 4;  // ...with a beat of gap after

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
/**
 * The two bounded string domains the fixture draws from, built once. `DEC2` is
 * the instrument column (00-99) and `HEX2` the effect column (00-FF); between
 * them they were `String(h % 100).padStart(2, '0')` and a `toString(16)
 * .toUpperCase().padStart(2, '0')` chain — five allocations per cell per frame,
 * for 1,776 cells, to produce one of 356 possible strings.
 */
const DEC2 = new Array(100);
for (let i = 0; i < 100; i++) DEC2[i] = (i < 10 ? '0' : '') + i;
const HEX2 = new Array(256);
for (let i = 0; i < 256; i++) HEX2[i] = (i < 16 ? '0' : '') + i.toString(16).toUpperCase();
/** The fixture's coarse-zoom count pill. A row can span at most 64 sixteenths,
 *  which is the loop bound below, so the domain is 0..64. */
const AGG_TEXT = new Array(65);
for (let i = 0; i < 65; i++) AGG_TEXT[i] = '[' + i + 'x]';

/**
 * Written into rather than returned fresh: this is called once per cell per
 * frame — 1,776 objects a frame at the default shape, over 100k/second — and the
 * caller copies both fields out immediately. The result is only valid until the
 * next call, which is true of every caller here by construction.
 */
const _content = { text: '', kind: 'empty' };

/** What a harmony block says when the caller supplied no speller. Frozen and
 *  shared rather than built per block per frame. */
const EMPTY_NAME = Object.freeze({ label: '', sub: '' });

/**
 * String interning for the effect column and the collision pill.
 *
 * These are per-note-per-frame paths whose domains are small and closed but not
 * dense enough to precompute up front — a retrigger count, a probability, a
 * count-and-pitch pair. Each table fills the first time a value appears and is
 * a plain lookup forever after, so the steady state allocates nothing while the
 * first frame after a project load allocates a handful of strings.
 *
 * Array index and Map.get on an existing key both allocate nothing.
 */
/**
 * Row labels, interned by the tick they name.
 *
 * The guard below skips the label loop entirely when nothing moved, which covers
 * playback and editing. Scrolling is the case it cannot help: every row takes a
 * new tick, so a one-row step rebuilt all 62 labels — ~2.3 KB a frame, measured.
 * But 61 of those 62 strings are the ones the previous frame already built, one
 * row further up. Interning turns a scroll into one new string per newly
 * revealed row, which is the same floor the renderer works to.
 *
 * Cleared when the zoom changes, because a tick means a different row label at
 * every zoom, and cleared again past a cap so a long scroll cannot grow it
 * without bound. Both are cheap: rebuilding a screenful of labels costs what one
 * frame of the old behaviour cost.
 */
const LABELS = new Map();
const LABEL_CAP = 8192;
let labelZoom = -1;
/** The meter the intern table was filled for. A tick names a different bar in a
 *  different meter, so the table belongs to one meter at a time — the same reason
 *  it belongs to one zoom at a time. */
let labelMeter = null;
/** positionOf's output. One record, reused: this runs per visible row per frame. */
const _pos = createPosition();

const R_TEXT = [], P_TEXT = [], EVTS = [], PILL_SAME = [];
function interned(table, prefix, n) {
  return table[n] !== undefined ? table[n] : (table[n] = prefix + n);
}
function pillEvts(n) {
  return EVTS[n] !== undefined ? EVTS[n] : (EVTS[n] = n + ' evts');
}
function pillSame(n, name) {
  const m = PILL_SAME[n] || (PILL_SAME[n] = new Map());
  let s = m.get(name);
  if (s === undefined) { s = n + '× ' + name; m.set(name, s); }
  return s;
}

function contentAt(tick, track, col) {
  const h = Math.abs(Math.sin((tick / 60000 + track * 101 + col * 7) * 0.61803) * 10000) | 0;
  const o = _content;
  if (h % 5 < 2) { o.text = ''; o.kind = 'empty'; return o; }
  if (h % 23 === 0) { o.text = 'OFF'; o.kind = 'off'; return o; }
  if (col === 0) { o.text = NOTES[h % NOTES.length]; o.kind = 'note'; return o; }
  if (col === 1) { o.text = DEC2[h % 100]; o.kind = 'inst'; return o; }
  o.text = HEX2[h % 256]; o.kind = 'fx'; return o;
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
const SIG = { zoomIndex: -1, pendingCount: -1, overlayLen: -1, badKey: -2,
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
                       aggCount: 0, aggLo: 0, aggHi: 0,
                       // The pitch this cell holds, for the contour ribbon. -1 =
                       // none. Separate from aggLo/aggHi because they answer a
                       // different question — a RANGE over many notes at coarse
                       // zoom, versus THE note here — and overloading one pair to
                       // mean both is how a renderer ends up drawing a range that
                       // is really a single note.
                       pitch: -1 };
    rows[i] = { index: 0, label: '', beat: false, bar: false, cells };
  }
  return {
    window: { startRow: 0, rowCount },
    zoom: ZOOM_LEVELS[0], tracks: [], rows, clips: [], harmony: [], _harmonyPool: [],
    cursor: { row: 0, track: 0, col: 0 }, playhead: { row: 0 }, selection: null,
    _shape: `${rowCount}x${trackCount}x${columns}`,
    // The same three numbers the shape string encodes. buildViewModel compares
    // THESE: building the string only to throw it away was one allocation per
    // frame to answer a question three integer comparisons answer for nothing.
    // The string stays because it reads well in a debugger and costs one alloc
    // per buffer, which happens on a shape change and never in a frame.
    _rows: rowCount, _trackCount: trackCount, _columns: columns,
    // What the row labels were last built for. Labels are a pure function of
    // (startRow, zoom, rowCount) — see the row loop.
    _labelStart: -1, _labelZoom: -1, _labelMeter: null,
    _clipPool: [],
  };
}

/**
 * Build a view-model for a window of the timeline.
 * @param {{startRow:number, rowCount:number, tracks:number, columns:number,
 *          zoomIndex?:number, cursor?:{row:number,track:number,col:number},
 *          playheadRow?:number, selection?:any}} opts
 */
/**
 * A velocity, as two hex digits.
 *
 * It was `('0' + v).slice(-2)` — the last two DECIMAL digits — so every velocity
 * from 100 up was silently mangled into a plausible low one: 100 read as "00",
 * 127 as "27". Wrong, and wrong in the way that does not announce itself, since
 * both are valid-looking velocities. Hex is what the design shows and what a
 * tracker's volume column has always been: two characters covering the whole
 * range instead of two that cover 0-99 and lie about the rest.
 */
const VELOCITY_TEXT = new Array(256);
for (let n = 0; n < 256; n++) VELOCITY_TEXT[n] = (n < 16 ? '0' : '') + n.toString(16);

export function velocityText(v) {
  return VELOCITY_TEXT[Math.max(0, Math.min(255, v | 0))];
}

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
    /**
     * The SONG's meter — what the time gutter counts in.
     *
     * Not the clip's. A clip owns its own time signature now and draws its own
     * accents, but bar NUMBERING stays global so that "where am I" has one answer
     * across tracks whose clips disagree. See meter.js.
     */
    meter = DEFAULT_METER,
    /**
     * A token that was typed and meant nothing, as {row, track, col, text, why}.
     *
     * Drawn OVER whatever the engine says is in that cell, like the entry
     * overlay, because it is the same kind of thing: what the user put there,
     * which has not become part of the music. Unlike the overlay it survives the
     * keystroke — that is the point of it.
     */
    badToken = null,
  } = opts;

  if (!buf || buf._rows !== rowCount || buf._trackCount !== trackCount
      || buf._columns !== columns) {
    buf = createBuffer(rowCount, trackCount, columns);
  }

  const zoom = ZOOM_LEVELS[zoomIndex];
  // Was `const tickOf = (r) => r * zoom.rowNanoticks`, which is a closure
  // allocated on every call — and this is called every frame. It is one
  // multiply; a name for it is not worth a heap object per frame.
  const rowTicks = zoom.rowNanoticks;

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
    const winStart = startRow * rowTicks;
    const winEnd = (startRow + rowCount) * rowTicks;
    const hpool = buf._harmonyPool;
    let bn = 0;
    for (let i = 0; i < harmony.length; i++) {
      const ev = harmony[i];
      const from = ev.tick;
      // The last field runs to the end of time, not to the end of the timeline.
      const to = i + 1 < harmony.length ? harmony[i + 1].tick : Number.MAX_SAFE_INTEGER;
      if (to <= winStart || from >= winEnd) continue;
      const b = hpool[bn] || (hpool[bn] = { key: '', label: '', sub: '', foot: '',
                                            startTick: 0, endTick: 0,
                                            _tick: -1, _root: -1, _scaleId: -1 });
      bn++;
      /**
       * The block's text, rebuilt only when the EVENT in this slot changes.
       *
       * Which pool slot an event lands in depends on the scroll position, so the
       * guard is keyed on the event's own identity — tick, root and scale — and
       * not on the slot. Those three are every input to all four strings: `key`
       * is literally their concatenation, `foot` is the root, and nameHarmony
       * spells root and scale. A different event in this slot fails the compare
       * and rebuilds.
       *
       * It was unconditional: four string allocations plus whatever nameHarmony
       * allocates, per visible block, per frame — to spell a chord symbol that
       * changes when the music changes, which is to say hardly ever.
       */
      if (b._tick !== ev.tick || b._root !== ev.root || b._scaleId !== ev.scaleId) {
        b._tick = ev.tick; b._root = ev.root; b._scaleId = ev.scaleId;
        const named = nameHarmony ? nameHarmony(ev) : EMPTY_NAME;
        b.key = ev.tick + ':' + ev.root + ':' + ev.scaleId;
        b.label = named.label;
        b.sub = named.sub;
        // The design pins a root/quant line to the bottom of the span. Root is
        // the pitch class the engine published; quant is not published, so it is
        // not claimed.
        b.foot = 'root ' + ev.root;
      }
      b.startTick = from;
      b.endTick = Math.min(to, winEnd + zoom.rowNanoticks);
      blocks.push(b);
    }
  }

  const rows = buf.rows;
  /**
   * Whether the row headings have to be rebuilt at all.
   *
   * `index`, `label`, `beat` and `bar` are a pure function of the row's TICK and
   * nothing else, and a row's tick is `(startRow + ri) * zoom.rowNanoticks`. So
   * the only two things that can change any of them are the scroll position and
   * the zoom — rowCount is the third, and a change to it recreates the buffer,
   * which resets `_labelStart` to -1 and forces a rebuild here.
   *
   * Without this the loop built 74 label strings — a template literal, often a
   * `String().padStart()` inside it — on every frame, to produce exactly the
   * strings that were already there. During playback, which is when frames are
   * scarce, the labels are precisely the part of the tracker that does NOT move.
   */
  // The meter is in this key because it is an input to every label: the same tick
  // is bar 5 in 4/4 and bar 6 in 7/8. Leaving it out is GUIDELINES 2.1 — content
  // changing while the key stands still — and it would show as bar numbers that
  // never update after a meter change, which reads as the meter not having taken.
  const relabel = buf._labelStart !== startRow || buf._labelZoom !== zoomIndex
                  || !sameMeter(buf._labelMeter, meter);
  buf._labelStart = startRow;
  buf._labelZoom = zoomIndex;
  buf._labelMeter = meter;
  const barTicks = ticksPerBar(meter);
  // A tick names a different row at every zoom, so the intern table belongs to
  // one zoom at a time. Keyed on the zoom rather than cleared by the caller,
  // because two buffers share it and either can be the one that notices.
  if (labelZoom !== zoomIndex || !sameMeter(labelMeter, meter)) {
    labelZoom = zoomIndex; labelMeter = meter; LABELS.clear();
  }
  for (let ri = 0; ri < rowCount; ri++) {
    const r = startRow + ri;
    const cells = rows[ri].cells;
    let ci = 0;
    /**
     * The row's absolute tick — computed only where it is used.
     *
     * There are two users: the fixture, which derives its content from it, and
     * the label rebuild. The engine branch never touches it, and computing it
     * anyway is not free: nanoticks run at 960,000 to the quarter, so a song
     * passes V8's small-integer range about nine minutes in, and from there
     * every one of these is a boxed double on the heap. Sixty-two of them a
     * frame is a kilobyte, and it appears only in long projects — which is the
     * worst way for a cost to arrive.
     */
    const tick = (engine && !relabel) ? 0 : r * rowTicks;


    for (let t = 0; t < trackCount; t++) {
      const inClip = engine ? true : clipIndexAt(tick, t) >= 0;
      for (let c = 0; c < columns; c++) {
        if (engine) {
          const cl = cells[ci++];
          cl.text = ''; cl.aggCount = 0; cl.pitch = -1;
          // A lane only has rows where its own grid lands. Elsewhere there is no
          // cell to write into — showing an empty one would imply you could.
          const laneLpb = engine.lpb[t] || zoom.linesPerBeat;
          const stride = zoom.linesPerBeat / laneLpb;
          cl.kind = (stride >= 1 && r % Math.round(stride) !== 0) ? 'offgrid' : 'empty';
          continue;
        }
        const cell = cells[ci++];
        cell.aggCount = 0;
        // ...and the pitch, or the ribbon leaks across a fixture switch. The
        // cells are POOLED and this branch never writes a pitch, so a cell that
        // held an engine note kept drawing that note's mark under fixture
        // content — visible as pitch marks in three goldens that have no engine
        // and therefore no pitches. Every field the other branch writes has to be
        // cleared here, or the pool remembers.
        cell.pitch = -1;
        if (!inClip) { cell.text = ''; cell.kind = 'outside'; continue; }
        if (zoom.aggregate && c === 0) {
          // A coarse row spans many finer rows; count the events that fall in it.
          const span = zoom.rowNanoticks / ZOOM_LEVELS[0].rowNanoticks;
          let n = 0;
          for (let k = 0; k < span && k < 64; k++) {
            const sub = tick + k * ZOOM_LEVELS[0].rowNanoticks;
            if (clipIndexAt(sub, t) >= 0 && contentAt(sub, t, c).kind !== 'empty') n++;
          }
          if (n > 1) { cell.text = AGG_TEXT[n]; cell.kind = 'aggregate'; }
          else { const g = contentAt(tick, t, c); cell.text = g.text; cell.kind = g.kind; }
        } else {
          const g = contentAt(tick, t, c);
          cell.text = g.text; cell.kind = g.kind;
        }
      }
    }
    if (relabel) {
      const row = rows[ri];
      row.index = r;
      let label = LABELS.get(tick);
      if (label === undefined) {
        // Labels come from the tick too, so a row means the same musical position
        // at every zoom — that is the whole point of zoom being a projection.
        positionOf(tick, meter, zoom.rowNanoticks, _pos);
        const bar = _pos.bar;
        const beatInBar = _pos.beat;
        // The sub-beat index is the row's position within the beat ON THIS GRID, not
        // in fixed 16ths. Hardcoding a 16th (60000nt) made a 12-per-beat grid label
        // its rows 0,1,2,4,5,6,8 — skipping some numbers and reusing others, so two
        // different rows could read as the same musical position.
        const sub = _pos.sub;
        label = zoom.rowNanoticks >= barTicks ? `${bar}` : `${bar}:${beatInBar}${sub ? ':' + DEC2[sub] : ''}`;
        if (LABELS.size >= LABEL_CAP) LABELS.clear();
        LABELS.set(tick, label);
      }
      row.label = label;
      row.beat = _pos.onBeat;
      row.bar = _pos.onBar;
    }
  }

  // Clips overlapping the window. They span rows, which is why the renderer
  // draws rails outside the recycled row band.
  //
  // Skipped outright when there is an engine: the block below clears `clips` and
  // refills it from the engine's real placements, so with a project loaded this
  // built a list of fixture clips — and a `clip k.t` name string for each — every
  // frame, for the next statement to throw away. Dead work is not free just
  // because it is correct.
  const clips = buf.clips;
  clips.length = 0;
  const pool = buf._clipPool;
  if (!engine) {
    let cn = 0;
    const winStart = startRow * rowTicks, winEnd = (startRow + rowCount) * rowTicks;
    const kFirst = Math.max(0, Math.floor(winStart / CLIP_TICKS));
    const kLast = Math.floor(winEnd / CLIP_TICKS);
    for (let k = kFirst; k <= kLast; k++) {
      for (let t = 0; t < trackCount; t++) {
        if ((k + t) % 3 === 0) continue; // gaps — must match clipIndexAt
        const cl = pool[cn] || (pool[cn] = { id: 0, track: 0, startTick: 0, endTick: 0,
                                             name: '', active: false, _k: -1, _t: -1 });
        cn++;
        cl.id = k * 100 + t; cl.track = t;
        cl.startTick = k * CLIP_TICKS;
        cl.endTick = (k + 1) * CLIP_TICKS - FIXTURE_TICKS_PER_BAR / 4;   // exclusive
        // Keyed on the two numbers the name spells. Scrolling changes which
        // (k, t) lands in a given pool slot, so the guard has to name the clip
        // rather than the slot.
        if (cl._k !== k || cl._t !== t) { cl._k = k; cl._t = t; cl.name = `clip ${k}.${t}`; }
        cl.active = (k + t) % 7 === 0;
        clips.push(cl);
      }
    }
  }

  // Place engine notes into the cleared grid. A note lands on the row whose tick
  // range contains it, so the projection follows zoom for free — the same reason
  // anything durable is stored in ticks rather than rows.
  if (engine) {
    const span = zoom.rowNanoticks;
    const winStart = startRow * rowTicks;
    const winEnd = (startRow + rowCount) * rowTicks;
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
        if ((c0.kind === 'note' || c0.kind === 'muted' || c0.kind === 'add'
             || c0.kind === 'collide') && c0._row === ri) {
          // More than one note on this (row, track). '**' said only "more than
          // one", which is the least useful true thing a cell can say — you had
          // to open the piano roll to find out whether it was a chord, a
          // flam, or two takes of the same note.
          //
          // A pill instead: "4x C-4" when every hit is the same pitch, "3 evts"
          // when they differ. Those are different musical situations and the
          // tracker should not make you leave to tell them apart.
          c0.aggCount = (c0.aggCount || 1) + 1;
          if (c0._same === undefined) c0._same = c0.text;
          if (c0._same !== null && c0._same !== pitchName(n.pitch)) c0._same = null;
          // Interned, because a chord makes this fire on every row that has one
          // and the cells are cleared each frame \u2014 so a per-cell guard cannot
          // work here and the string has to come from somewhere. Both domains
          // are small and closed: counts are how many notes share a row, names
          // are the 128 pitches. The tables fill once and never again.
          c0.text = c0._same !== null ? pillSame(c0.aggCount, c0._same)
                                      : pillEvts(c0.aggCount);
          // Several notes on one row: the ribbon shows the SPREAD, which is the
          // register collision this feature exists to make visible.
          if (n.pitch < c0.pitch) c0.pitch = n.pitch;
          if (c0._hiPitch === undefined || n.pitch > c0._hiPitch) c0._hiPitch = n.pitch;
          c0.kind = 'collide';
        } else {
          c0.text = pitchName(n.pitch);
          // The contour ribbon's datum. Set alongside the text because it is the
          // same fact — what note is here — read at a glance instead of read.
          c0.pitch = n.pitch;
          // Muted base notes still ship — draw them struck out. Adds carry
          // provenance so an override reads differently from the shared clip.
          c0.kind = n.muted ? 'muted' : n.isAdd ? 'add' : 'note';
          c0._row = ri;
          // Reset the pill's accumulators: this cell holds one note again.
          c0.aggCount = 0;
          c0._same = undefined;
          // ...and the ribbon's spread, or a cell that briefly held a chord would
          // keep drawing its range after the chord became one note.
          c0._hiPitch = undefined;
        }
      }
      if (columns > 1) {
        const c1 = row.cells[base + 1];
        if (c1) {
          // One velocity is a number; several are not, and printing the last
          // one to arrive would be a number nobody could act on.
          c1.text = c0 && c0.kind === 'collide' && c0._same === null
            ? 'mix' : velocityText(n.velocity);
          c1.kind = 'inst';
        }
      }
      if (columns > 2 && (n.retrigger || n.probability || n.delayTicks)) {
        const c2 = row.cells[base + 2];
        if (c2) {
          c2.text = n.retrigger ? interned(R_TEXT, 'R', n.retrigger)
                  : n.probability ? interned(P_TEXT, 'P', n.probability) : 'D';
          c2.kind = 'fx';
        }
      }
    }
    // Real placements from the engine (v11). One per track today because the
    // engine plays the first placement; the rail code is the same when M3.3
    // brings several per track.
    const pool = buf._clipPool;
    clips.length = 0;
    // Once, not twice per extent: past ~9 minutes of song this is a boxed
    // double, and it was being built afresh for each end of every clip's range.
    const cursorTick = cursor.row * rowTicks;
    for (let i = 0; i < engine.extentCount; i++) {
      const e = engine.extents[i];
      if (e.track >= trackCount) continue;
      const cl = pool[i] || (pool[i] = { id: 0, track: 0, startTick: 0, endTick: 0, name: '', active: false });
      cl.id = e.placementId; cl.track = e.track;
      cl.startTick = e.startTick; cl.endTick = e.endTick;
      cl.name = e.name;
      // The clip the CURSOR is in. It used to compare e.placementId against
      // cursor.placementId, which nothing ever set — so the active state could
      // not fire, and a selected clip never looked selected. "Where the cursor
      // is" is the question a tracker can actually answer.
      cl.active = e.track === cursor.track
               && cursorTick >= e.startTick && cursorTick < e.endTick;
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
    // The ENGINE's aggregate grid is per quarter note, not per meter beat — it
    // knows nothing about the time signature. Folding its rows by the meter's beat
    // would be right-looking and wrong by the denominator ratio in any meter that
    // is not x/4.
    const beatsPerRow = Math.max(1, Math.round(zoom.rowNanoticks / NANOTICKS_PER_QUARTER));
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

  // Before the entry overlay, so that typing into a cell that is already marked
  // shows what you are typing rather than what failed last time.
  if (badToken) {
    const row = rows[badToken.row - startRow];
    if (row) {
      const cell = row.cells[badToken.track * columns + badToken.col];
      if (cell) { cell.text = badToken.text; cell.kind = 'bad'; }
    }
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
    // The mark is an input to what a cell says, so it belongs in the signature.
    // Keyed on its position and text rather than on its presence: moving one bad
    // token to another cell changes two cells and no count.
    const badKey = badToken
      ? badToken.row * 1e6 + badToken.track * 1e3 + badToken.col + badToken.text.length
      : -1;
    if (s.zoomIndex !== zoomIndex || s.pendingCount !== pendingCount
        || s.overlayLen !== overlayLen || s.badKey !== badKey
        || s.notesRevision !== (engine ? engine.notesRevision : -1)
        || s.aggRevision !== (engine ? engine.aggRevision : -1)
        || s.rowGrid !== (engine ? engine.rowGrid : -1)) {
      s.zoomIndex = zoomIndex; s.pendingCount = pendingCount; s.overlayLen = overlayLen;
      s.badKey = badKey;
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
