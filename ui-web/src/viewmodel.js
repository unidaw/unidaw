import { pitchName } from './wire.js';
import { nameChord } from './harmonymodel.js';
import { DEFAULT_METER, createPosition, positionOf, sameMeter,
         ticksPerBar, ticksPerBeat, NANOTICKS_PER_QUARTER } from './meter.js';
import { opsRun, opTokenAt } from './rowops.js';
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
/**
 * Reduce every clip extent to the four small numbers the row loop needs, ONCE per
 * frame, into typed arrays the loop can read without boxing anything.
 *
 * This exists because the obvious version — ask "which clip covers this tick on
 * this track" per (row, track) — costs 907 B/draw an hour into a song, measured.
 * The reason is GUIDELINES 3.13: nanoticks run at 960,000 to the quarter, so past
 * about nine minutes every tick is a heap double, and handing one to a function
 * 768 times a frame boxes it 768 times. Nothing about the arithmetic is expensive;
 * the SIZE of the numbers is.
 *
 * So the large numbers are collapsed here, `extentCount` times rather than
 * `rows x tracks` times, and what the loop sees is:
 *
 *   startRow / endRow  the extent's span in ROW indices — a few thousand, and
 *                      fractional only when a clip starts mid-row, which is why
 *                      these are Float64Array and not Int32Array;
 *   phase              the clip's start modulo a quarter note, which is all the
 *                      grid test needs of its anchor (see below);
 *   lpb                the clip's own lines-per-beat, 0 when it publishes none;
 *   rowsPerBar         how many display rows one of THIS clip's bars occupies;
 *   rowsPerBeat        the same for one of its beats;
 *   audio              1 when the region is audio, which has no authored meter.
 *
 * TWO SENSES OF "BEAT", and they are not interchangeable. `lines_per_beat` is
 * lines per QUARTER NOTE — `LaneGrid::row_nanoticks` is `960000 / lines_per_beat`
 * and never consults a time signature, which is why ZOOM_LEVELS above is
 * 960000/lpb at every non-aggregate index. `meter.js`'s `ticksPerBeat` is the
 * musical beat, the meter's denominator unit: in 6/8 that is an eighth. The row
 * SUBDIVISION comes from the first; the bar and beat NUMBERS come from the second.
 * Crossing them draws a 6/8 clip as 6/4.
 *
 * THE GRID TEST, and why phase is enough. A row is on a clip's grid when
 *
 *     (tick - clipStart) % (960000 / lpb) === 0
 *
 * Multiplying through by lpb removes the division, which matters because 960000 /
 * lpb is not an integer for every lpb the engine can publish — 7 gives
 * 137142.857, and an exact test on that is not exact:
 *
 *     ((tick - clipStart) * lpb) % 960000 === 0
 *
 * and modular arithmetic lets both operands be reduced mod 960000 first. The row
 * loop reduces `tick` once per row; this reduces `clipStart` once per frame. Every
 * value the inner test touches is then below 960000 and stays a small integer.
 */
/** A clip's meter, written in place per extent. One record, never per frame. */
const _clipMeter = { numerator: 4, denominator: 4 };

/**
 * Whether a track's bars are the SONG's bars.
 *
 * A lane only earns a readout when it disagrees with the gutter; otherwise the
 * column repeats what is already on screen, one column per track, forever. Two
 * ways to disagree, and both matter:
 *
 *   - the clip's METER differs, so its bars are a different length; or
 *   - the clip's ORIGIN is not on a song bar, so its bars are the same length and
 *     start somewhere else. A 4/4 clip dropped a sixteenth late is in the song's
 *     meter and still needs its own numbering, which is exactly the case a
 *     meter-only test would call "no deviation" and leave unreadable.
 *
 * Computed over EVERY extent on the track, not the visible ones. A track whose
 * bridge alone is in 7/8 must carry the column at every scroll position — a lane
 * that grows a column when you scroll into the bridge and loses it on the way out
 * shifts the layout under the pointer, which is worse than a redundant column.
 */
/** uiTrackFlags bit 1: this track's `parent_id` is meaningful. See the walk below. */
const HAS_PARENT = 1 << 1;

/**
 * uiTrackFlags bit 2 (kShmVersion 22): this slot is a TOMBSTONE — a track that
 * was removed. The slot stays in the array so the tracks after it keep their
 * ids, and `uiTrackCount` is now the EXTENT (highest live slot + 1) rather than
 * a dense count.
 *
 * Which is the collapse case exactly: a lane that must not be drawn and must not
 * renumber its neighbours. So it takes the same route — zero width — instead of
 * growing a second, parallel notion of "lane that isn't there". Renumbering
 * around a hole would make lane position and track id two different things, and
 * every command keyed on a track index would have to learn the difference.
 */
const ABSENT = 1 << 2;

/**
 * Which lanes are hidden because an ancestor is collapsed.
 *
 * A child track is an ORDINARY track (kShmVersion 20) — same flat index, same
 * name, same mixer strip — and collapse is a decision about what to DRAW, never
 * about what exists. A collapsed parent still has its children, still plays them,
 * still publishes their rails; they simply take no width.
 *
 * Hidden by giving the lane ZERO WIDTH rather than by renumbering. Track ids stay
 * the flat index the engine publishes, so the cursor, the selection's field
 * indices (`track * columns + col`) and every command keyed on a track index are
 * untouched. Renumbering would make lane position and track id two different
 * things, and every one of those would have to learn the difference.
 *
 * Walks ANCESTORS, not just the parent: a collapsed grandparent hides a child
 * whose own parent is expanded. Bounded by trackCount so a cycle in parent_id —
 * which the engine should never publish and which would otherwise hang the draw —
 * terminates instead.
 */
function computeLaneHidden(engine, buf, trackCount, override) {
  if (buf._laneHidden.length < trackCount) buf._laneHidden = new Uint8Array(trackCount * 2);
  buf._laneHidden.fill(0, 0, trackCount);
  const parent = engine.trackParent, flags = engine.trackFlags;
  if (!parent || !flags) return 0;
  for (let t = 0; t < trackCount; t++) {
    // A removed slot is hidden outright, before the ancestor walk: it has no
    // parent worth reading and nothing to draw. Checked first so a tombstone
    // whose stale parent_id still points somewhere cannot walk anywhere.
    if ((flags[t] & ABSENT) !== 0) { buf._laneHidden[t] = 1; continue; }
    /**
     * Walk up while the track HAS a parent, read from the flag and not from the
     * id.
     *
     * `parent_id 0` cannot mean "top-level": track 0 is a valid track and is the
     * likeliest parent there is — the first track, and the plugin whose aux buses
     * become stems. So "no parent" and "child of track 0" shared one value and a
     * whole multi-out tree read as flat. Backend added
     * `kUiTrackFlagHasParent` (bit 1) at my asking; the id is meaningful only
     * when it is set, and it is set only for a genuine child.
     *
     * Bounded by the track count, and the ancestor walk is kept even though the
     * multi-out invariant is depth 1 (a stem's parent is always its plugin's
     * track, never another stem). It costs nothing, and a cycle terminates
     * instead of hanging the draw.
     */
    let p = t, hops = 0;
    while ((flags[p] & HAS_PARENT) !== 0 && hops++ < trackCount) {
      p = parent[p];
      if (p >= trackCount) break;
      const collapsed = override ? override[p] : (flags[p] & 1) !== 0;
      if (collapsed) { buf._laneHidden[t] = 1; break; }
    }
  }
  let sig = 0;
  for (let t = 0; t < trackCount && t < 32; t++) sig |= buf._laneHidden[t] << t;
  return sig;
}

/** How many lanes a collapsed parent is standing in for. */
function countChildren(engine, trackCount, t) {
  const parent = engine.trackParent;
  if (!parent) return 0;
  let n = 0;
  for (let k = 0; k < trackCount; k++) if (parent[k] === t) n++;
  return n;
}

function computeLaneShow(engine, buf, meter, trackCount) {
  const songBar = ticksPerBar(meter);
  if (buf._laneShow.length < trackCount) buf._laneShow = new Uint8Array(trackCount * 2);
  buf._laneShow.fill(0, 0, trackCount);
  for (let i = 0; i < engine.extentCount; i++) {
    const e = engine.extents[i];
    if (e.track >= trackCount || e.audio) continue;
    const g = e.grid;
    const meterDiffers = !!g && (g.numerator !== meter.numerator
                                 || g.denominator !== meter.denominator);
    if (meterDiffers || e.startTick % songBar !== 0) buf._laneShow[e.track] = 1;
  }
  let sig = 0;
  for (let t = 0; t < trackCount && t < 32; t++) sig |= buf._laneShow[t] << t;
  return sig;
}

function reduceExtents(engine, buf, rowTicks, meter) {
  const n = engine.extentCount;
  if (buf._extStartRow.length < n) {
    // Grown, never per frame: extentCount changes on a project load.
    buf._extStartRow = new Float64Array(n * 2);
    buf._extEndRow = new Float64Array(n * 2);
    buf._extPhase = new Int32Array(n * 2);
    buf._extLpb = new Int32Array(n * 2);
    buf._extTrack = new Int32Array(n * 2);
    buf._extRowsPerBar = new Float64Array(n * 2);
    buf._extRowsPerBeat = new Float64Array(n * 2);
    buf._extAudio = new Int32Array(n * 2);
  }
  for (let i = 0; i < n; i++) {
    const e = engine.extents[i];
    const g = e.grid;
    buf._extTrack[i] = e.track;
    buf._extStartRow[i] = e.startTick / rowTicks;
    buf._extEndRow[i] = e.endTick / rowTicks;
    buf._extPhase[i] = e.startTick % NANOTICKS_PER_QUARTER;
    buf._extLpb[i] = g && g.linesPerBeat > 0 ? g.linesPerBeat : 0;
    buf._extAudio[i] = e.audio ? 1 : 0;
    // A clip with no published grid is counted in the SONG's meter — that is what
    // the sentinel means, and it is not the same claim as "this clip is in 4/4".
    // The numerator is guarded because positionOf divides by it; the engine caps
    // it above zero, so this is for a fixture or a future producer, and it costs
    // one test per extent per frame rather than one per cell.
    _clipMeter.numerator = (g && g.numerator) || meter.numerator || 4;
    _clipMeter.denominator = (g && g.denominator) || meter.denominator || 4;
    buf._extRowsPerBar[i] = ticksPerBar(_clipMeter) / rowTicks;
    buf._extRowsPerBeat[i] = ticksPerBeat(_clipMeter) / rowTicks;
  }
  return n;
}

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

/**
 * The clip-local readouts, interned on THEIR OWN CONTENT rather than on position.
 *
 * `LABELS` above has to be guarded by `labelZoom` and `labelMeter` and cleared
 * when either moves, because its key is an absolute tick — and a tick names a
 * different bar under a different zoom or meter. That guard is the whole reason
 * that table is delicate.
 *
 * This one is keyed on `bar * 64 + beat`, which IS the string: two lanes in
 * different meters that both land on "2:3" want the same "2:3". No zoom guard, no
 * meter guard, and no way for one lane to read another's label. `beat` is bounded
 * by the numerator, which the engine caps at 31, so 64 leaves no aliasing.
 */
const LANE_LABELS = new Map();
const LANE_LABEL_CAP = 8192;
function laneLabel(bar, beat) {
  // `| 0` on the key, not decoration. These arrive from Math.floor over Float64Array
  // reads, so they are DOUBLES even when their values are whole — and Map.get with
  // a non-Smi key boxes a HeapNumber for the lookup, every lane, every row, every
  // frame. Measured at 745 B/draw during playback against a 900 B budget that the
  // rest of the tracker also has to fit inside.
  const key = (bar * 64 + beat) | 0;
  let s = LANE_LABELS.get(key);
  if (s === undefined) {
    if (LANE_LABELS.size >= LANE_LABEL_CAP) LANE_LABELS.clear();
    s = bar + ':' + beat;
    LANE_LABELS.set(key, s);
  }
  return s;
}

const EVTS = [], PILL_SAME = [];
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
              notesRevision: -2, aggRevision: -2, rowGrid: -2,
              // -2, not -1: -1 is the real 'nothing selected' value, and a guard seeded
              // to a value it can legitimately hold never fires on the first frame.
              opSel: -2, opRow: -2, opTrack: -2, opCol: -2,
              // The RAILS, which are not decoration: a clip's own lines-per-beat
              // and meter are packed into UiClipExtent.flags, and `extentsRevision`
              // is what moves when they do (wire.js). Without it here, changing a
              // clip's meter or moving a clip changes which rows are off-grid, the
              // view-model computes the new marking correctly, and the renderer
              // never rebinds the rows whose INDEX did not move — so the old grid
              // stays on screen. GUIDELINES 2.1, from the inside: content changed
              // while every key the consumer watches stood still.
              extentsRevision: -2 };
let contentRevision = 0;

/**
 * Cells per note column: the note, its velocity, its effect. A track shows
 * `noteColumns` of these side by side.
 */
export const FIELDS_PER_NOTE = 3;

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
                       pitch: -1,
                       /**
                        * Where in this ROW the note actually sounds, in whole
                        * percent, or -1 for "exactly on the row".
                        *
                        * A tracker draws a note on a row, but a RECORDED note
                        * almost never lands on one — it lands near one. Showing
                        * only the row is the lie every tracker tells; showing only
                        * a "D" in the effect column, which is what this did
                        * before, says a deviation exists without saying which way
                        * or how far. ARCHITECTURE_REVIEW Movement 1 item 13 asks
                        * for the note kept exact and the deviation made visible,
                        * and this is the visible half.
                        *
                        * Quantised to percent on purpose: a cell is 76px, so a
                        * percent is under a pixel, and an integer keeps the
                        * renderer's guard an integer compare and its style write
                        * an interned string.
                        */
                       dev: -1,
                       /**
                        * Whether the sounding position is OUTSIDE this row: -1
                        * before it, +1 after, 0 inside.
                        *
                        * Quantize made this reachable. A grid coarser than the row
                        * pulls a note to a line that can be well before its own
                        * row, so the note is heard where this cell is not looking.
                        * `dev` pins to the edge in that case, and this is what
                        * stops the pin reading as "on time" — which is the one
                        * thing a deviation mark must never claim.
                        */
                       devOut: 0 };
    rows[i] = { index: 0, label: '', beat: false, bar: false, cells,
                /**
                 * The CLIP-LOCAL position of this row on each lane, and that
                 * lane's own accents.
                 *
                 * In the row literal rather than bolted on later: a field added to
                 * a pooled object after creation forces a hidden-class transition
                 * the first time it is written, on the first frame that has clip
                 * grids in it. `laneAcc` packs bit 0 = on this clip's bar, bit 1 =
                 * on its beat, because a Uint8Array of flags is one allocation for
                 * the buffer's life and an array of small objects is one per lane
                 * per row.
                 *
                 * Both are trackCount-sized, so the buffer's existing shape guard
                 * (_rows/_trackCount/_columns) already covers them.
                 */
                laneBar: new Array(trackCount).fill(''),
                laneAcc: new Uint8Array(trackCount) };
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
    /** The extents revision the per-lane readouts were built for. */
    _laneExtRev: -2,
    _clipPool: [],
    /**
     * Per track, the index of the clip extent that answered the previous row.
     *
     * Rows are built in ascending tick order and a clip spans many of them, so
     * the extent that covered the last row almost always covers this one too.
     * Checking it first turns the per-cell lookup into one comparison; only a
     * clip boundary costs a scan. Without the hint this is rows x tracks x
     * extents every frame, which is the shape that looks free in a fixture with
     * three clips and is not free in a real arrangement.
     */
    _gridCursor: new Int32Array(trackCount).fill(-1),
    /**
     * Clip extents reduced to small numbers, rebuilt once per frame by
     * `reduceExtents`. Grown on a project load, never in a frame. See that
     * function for why the row loop cannot be allowed to touch a raw tick.
     */
    _extStartRow: new Float64Array(0),
    _extEndRow: new Float64Array(0),
    _extPhase: new Int32Array(0),
    _extLpb: new Int32Array(0),
    _extTrack: new Int32Array(0),
    _extRowsPerBar: new Float64Array(0),
    _extRowsPerBeat: new Float64Array(0),
    _extAudio: new Int32Array(0),
    /** Per track, whether its bars differ from the song's. See computeLaneShow. */
    _laneShow: new Uint8Array(0),
    /** Per track, whether an ancestor is collapsed. See computeLaneHidden. */
    _laneHidden: new Uint8Array(0),
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
    startRow, rowCount, tracks: trackCount, columns = FIELDS_PER_NOTE,
    // How many note columns each track shows. `columns` is the total cell stride
    // per track and stays authoritative for indexing; this says how to divide it.
    noteColumns = Math.max(1, Math.floor(columns / FIELDS_PER_NOTE)),
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
    /*
     * How many characters the ops cell can show, measured by the caller.
     *
     * Passed in rather than derived because this file may not compute geometry
     * (GUIDELINES 3.11) — a second copy of the box model here is how a paint and a
     * hit test come to disagree. 0 means "never fits", which draws the glyph run:
     * that is the right default for a caller that has not measured, because the run
     * is the form that always fits.
     */
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
    /**
     * A per-track override of the engine's `collapsed` flag, or null to use it.
     *
     * Collapsing is a view decision and the engine has no command for it yet, so
     * the UI holds its own answer. Kept as an OVERRIDE rather than as the only
     * source: when a command exists the engine's flag becomes authoritative and
     * this becomes the optimistic half, which is the same shape every other edit
     * on this surface already has.
     */
    collapsedOverride = null,
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
  // Every clip extent collapsed to small numbers, before the row loop touches any
  // of them. Unconditional rather than guarded on extentsRevision: it is
  // extentCount iterations of arithmetic, and a guard that got the key wrong would
  // serve the row loop a stale clip map — GUIDELINES 2.1, and the failure would be
  // rows going dead in the wrong places, which reads as a grid bug.
  const extN = engine ? reduceExtents(engine, buf, rowTicks, meter) : 0;
  const relabel = buf._labelStart !== startRow || buf._labelZoom !== zoomIndex
                  || !sameMeter(buf._labelMeter, meter);
  /**
   * Whether the per-lane readouts need rebuilding.
   *
   * They are a pure function of four things: which absolute rows are on screen,
   * the zoom, the clip extents, and the song meter (which a clip with no grid of
   * its own is counted in). `relabel` already watches the first, second and
   * fourth; `extentsRevision` is the third, and it is the one a plain `relabel`
   * guard would have missed — a clip moving or changing meter at an unchanged
   * scroll position is exactly the case, and GUIDELINES 2.1 is a list of times
   * that shape has bitten.
   *
   * Worth guarding rather than recomputing: the readout is four Float64Array reads
   * and two divisions per lane per row, and doubles that come out of a typed array
   * are heap numbers. Unguarded it measured 745 B/draw during playback, against a
   * 900 B budget the whole tracker shares. Guarded, a frame that only advances the
   * playhead does none of it.
   */
  const extRev = engine ? engine.extentsRevision : -1;
  // Which lanes have earned a readout, as a bitmask the renderer can compare in
  // one integer. Recomputed with the extents; it changes on a load or a clip edit
  // and never inside a scroll.
  const laneShowSig = engine ? computeLaneShow(engine, buf, meter, trackCount) : 0;
  const laneHiddenSig = engine
    ? computeLaneHidden(engine, buf, trackCount, collapsedOverride) : 0;
  const laneStale = relabel || buf._laneExtRev !== extRev;
  buf._laneExtRev = extRev;
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
    const row = rows[ri];
    const cells = row.cells;
    let ci = 0;
    /**
     * The row's absolute tick.
     *
     * This used to be skipped on the engine path — `(engine && !relabel) ? 0 :
     * ...` — because nanoticks run at 960,000 to the quarter, so a song passes
     * V8's small-integer range about nine minutes in and every one of these
     * becomes a boxed double (GUIDELINES 3.13). The engine branch now needs it:
     * a lane's grid belongs to the CLIP under the row, and finding that clip is
     * a question about where the row IS. Measured by test/alloc.mjs rather than
     * assumed — the "hour into the song" scenario exists for exactly this.
     */
    const tick = r * rowTicks;
    /**
     * The row's position within a quarter note — the only part of it the per-lane
     * grid test needs, and small enough to stay a machine integer for the whole
     * length of a song. Reduced once per row rather than once per (row, track):
     * see `reduceExtents` for why a raw tick must not reach the inner loop.
     */
    const tickMod = tick % NANOTICKS_PER_QUARTER;


    for (let t = 0; t < trackCount; t++) {
      const inClip = engine ? true : clipIndexAt(tick, t) >= 0;
      /**
       * Whether this row exists on THIS lane's grid.
       *
       * A lane only has rows where its own grid lands; elsewhere there is no cell
       * to write into, and drawing an empty one implies you could. Computed once
       * per (row, track) rather than per column — all three columns of a cell
       * share a position, so asking three times was three answers to one question.
       *
       * THE GRID BELONGS TO THE CLIP, not to the track. Clips run in sequence and
       * may each carry their own lines-per-beat (kShmVersion 19), so a lane's grid
       * changes down the timeline — a verse in 4 and a bridge in 3 on one track is
       * the case this has to get right, and a per-track lines-per-beat cannot
       * express it. `engine.lpb[t]` is the fallback while the engine still
       * publishes it, and the zoom's own grid is the fallback below that, which is
       * the "everything is writable" behaviour from before any of this existed.
       *
       * Anchored at the CLIP's start, not at the song's origin. A clip's grid is a
       * property of the clip, so moving a clip must not resubdivide it; anchoring
       * at zero would make a clip's rows depend on where it happens to sit.
       */
      let offGrid = false;
      if (engine) {
        // The covering extent, found with the previous row's answer tried first.
        // A clip spans many rows, so that hint hits on all but the boundary rows
        // and this is one comparison rather than a scan. Both the hint check and
        // the scan compare ROW indices from the typed arrays — no tick crosses
        // this loop, which is the whole point of reduceExtents.
        let ei = -1;
        const hint = buf._gridCursor[t];
        if (hint >= 0 && hint < extN && buf._extTrack[hint] === t
            && r >= buf._extStartRow[hint] && r < buf._extEndRow[hint]) {
          ei = hint;
        } else {
          for (let i = 0; i < extN; i++) {
            if (buf._extTrack[i] === t && r >= buf._extStartRow[i] && r < buf._extEndRow[i]) {
              ei = i; break;
            }
          }
          buf._gridCursor[t] = ei;
        }
        // The clip's own grid; the per-track lines-per-beat while the engine still
        // publishes one; the zoom's own grid below that, which is the "every row
        // is writable" behaviour from before any of this existed.
        const lpb = (ei >= 0 && buf._extLpb[ei]) || engine.lpb[t] || zoom.linesPerBeat;
        const phase = ei >= 0 ? buf._extPhase[ei] : 0;
        // Exact for EVERY lines-per-beat, including the ones that do not divide a
        // quarter note. The row-stride version this replaces was
        // `Math.round(zoom.linesPerBeat / lpb)`, and `Math.round(4 / 3)` is 1 — so
        // `r % 1`, which is 0 for every row, and a triplet lane reported every row
        // as on-grid at every zoom but the finest. That is the bug in GUIDELINES
        // 2.1.1's table. Integers divide exactly or they do not; nothing rounds.
        offGrid = lpb > 0 && (((tickMod - phase) * lpb) % NANOTICKS_PER_QUARTER) !== 0;

        /**
         * This lane's own position in its own clip, in ROW space.
         *
         * Computed here rather than in the `relabel` block below, deliberately.
         * That guard names startRow, zoomIndex and the SONG meter — none of which
         * move when a clip does. Put the lane readout behind it and dragging a
         * clip leaves every lane number stale at an unchanged scroll position,
         * which is GUIDELINES 2.1 written out longhand.
         *
         * `ei < 0` is the ONLY signal for "no clip under this row" on the engine
         * path: `inClip` is hardcoded true above and the 'outside' kind is emitted
         * only by the fixture. Branch on the extent, never on the kind.
         *
         * Blank rather than the song's bars where there is no clip. The gutter on
         * the left already answers the song question, and repeating it per lane
         * would put a confident number under material that does not exist.
         *
         * Audio regions read blank too. The engine packs a grid for them — it
         * looks the clip up by id and writes linesPerBeat and the time signature
         * without checking the kind — so what arrives is the 4/4 default rather
         * than an authored meter, and printing bar numbers off a default is a
         * fabrication that looks like a fact.
         */
        if (!laneStale) { /* the readout below is still valid */ }
        else if (ei >= 0 && !buf._extAudio[ei]) {
          const perBar = buf._extRowsPerBar[ei];
          const perBeat = buf._extRowsPerBeat[ei];
          const d = r - buf._extStartRow[ei];
          const barIdx = Math.floor(d / perBar) | 0;
          const inBar = d - barIdx * perBar;
          const beatIdx = Math.floor(inBar / perBeat) | 0;
          row.laneBar[t] = laneLabel(barIdx + 1, beatIdx + 1);
          row.laneAcc[t] = (inBar === 0 ? 1 : 0)
                         | (inBar - beatIdx * perBeat === 0 ? 2 : 0);
        } else {
          row.laneBar[t] = '';
          row.laneAcc[t] = 0;
        }
      } else {
        /**
         * The fixture has clips too — regularly spaced pseudo-clips, four bars
         * each — and they get a readout for the same reason everything else here
         * does: a fixture that leaves a feature blank tests the width of a column
         * and nothing inside it. Every golden would show an empty 40px strip per
         * lane and pass, which is GUIDELINES 2.1.1 with the fixture pointed at a
         * feature it does not exercise.
         *
         * Written even when there is no clip, not skipped. The rows are POOLED and
         * this array is reused across fixtures and engines; leaving it alone lets
         * a lane keep the number a previous project wrote there — the same bug the
         * pitch ribbon had, in the same place, for the same reason.
         */
        const ki = laneStale ? clipIndexAt(tick, t) : -2;
        if (ki === -2) { /* still valid */ }
        else if (ki >= 0) {
          const d = (tick - ki * CLIP_TICKS) / rowTicks;
          const perBar = FIXTURE_TICKS_PER_BAR / rowTicks;
          const perBeat = perBar / DEFAULT_METER.numerator;
          const barIdx = Math.floor(d / perBar) | 0;
          const inBar = d - barIdx * perBar;
          const beatIdx = Math.floor(inBar / perBeat) | 0;
          row.laneBar[t] = laneLabel(barIdx + 1, beatIdx + 1);
          row.laneAcc[t] = (inBar === 0 ? 1 : 0)
                         | (inBar - beatIdx * perBeat === 0 ? 2 : 0);
        } else {
          row.laneBar[t] = '';
          row.laneAcc[t] = 0;
        }
      }
      for (let c = 0; c < columns; c++) {
        if (engine) {
          const cl = cells[ci++];
          cl.text = ''; cl.aggCount = 0; cl.pitch = -1; cl.dev = -1; cl.devOut = 0;
          cl.kind = offGrid ? 'offgrid' : 'empty';
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
        cell.dev = -1;
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
      row.index = r;
      // OUTSIDE the cache check, deliberately. `_pos` is one shared record and the
      // accent flags below are read from it unconditionally, so filling it only on
      // a miss published the flags of whatever tick last MISSED — a row that hit
      // the intern table got another row's bar and beat stripes. Scroll down past
      // the cached range and back and the accents move to the wrong rows, while
      // the labels stay right, because the labels are the thing the cache is for.
      //
      // Cheap to always do: this whole block runs only when `relabel` is set (the
      // window, the zoom or the song meter moved), and positionOf is arithmetic
      // into a caller-owned record — no allocation. The cache still saves what it
      // was built to save, which is the string construction below.
      positionOf(tick, meter, zoom.rowNanoticks, _pos);
      let label = LABELS.get(tick);
      if (label === undefined) {
        // Labels come from the tick too, so a row means the same musical position
        // at every zoom — that is the whole point of zoom being a projection.
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
      /**
       * The note's OWN column, not always the track's first.
       *
       * A track shows `noteColumns` of them, three cells each, and the engine has
       * carried a per-note `column` all along. Placing every note at the track's
       * base is what turned a three-note chord into one cell reading "3 evts"
       * with no way to see or edit what was in it.
       *
       * Clamped rather than trusted: `column` is a byte off the wire, and a note
       * claiming column 200 must land somewhere real instead of writing past the
       * row. It lands in the last column the track has, where it will collide
       * visibly — which is the honest outcome for data this file cannot place.
       */
      const nc = Math.min(noteColumns - 1, Math.max(0, n.column | 0));
      const base = n.track * columns + nc * FIELDS_PER_NOTE;
      const c0 = row.cells[base];
      if (c0) {
        // Two notes can still land on one cell — same row, same track, same
        // COLUMN. That is genuinely ambiguous data rather than a chord, so it
        // keeps the pill: a position key losing data while rendering something
        // plausible is the contentAt bug, and the pill is what makes it visible.
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
          if (c0._sameVel === undefined) c0._sameVel = c0._firstVel;
          if (c0._same !== null && c0._same !== pitchName(n.pitch)) c0._same = null;
          /*
           * ...and whether the VELOCITIES agree, which is a different question.
           *
           * The velocity cell printed a number whenever the PITCHES matched — so two
           * C-4s at 20 and 127 showed one of them, whichever arrived last, as though
           * it were the velocity of the cell. An arbitrary number presented as a
           * fact, and it changes when the wire reorders.
           *
           * Tracked separately rather than folded into `_same`: "4x C-4" at velocity
           * 100 each SHOULD print 100 — that is useful and true — and only a genuine
           * disagreement is a "mix".
           */
          if (c0._sameVel !== null && c0._sameVel !== n.velocity) c0._sameVel = null;
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
          /*
           * ...AND INTO THE FIELDS THE RENDERER READS.
           *
           * The ribbon picks its source on `aggCount`: a cell with one has it from
           * `pitch`, a cell with several from `aggLo`/`aggHi`. This branch sets
           * aggCount and then wrote its spread into pitch/_hiPitch alone — which
           * that very flag makes the renderer stop reading — so the ribbon for a
           * collided cell came from aggLo/aggHi with nothing in them: zero, the
           * bottom of the pitch scale, for exactly the cells the pill exists to
           * explain. And a golden had been blessed with it there.
           */
          c0.aggLo = c0.pitch;
          c0.aggHi = c0._hiPitch;
          c0.kind = 'collide';
          /*
           * AND NO DEVIATION MARK, because there is no single sounding position.
           *
           * This branch never touched `dev`, so the cell kept whatever the FIRST
           * note through it had written — one hairline, placed by one of several
           * notes, next to text reading "3 evts". A mark that names a position when
           * three notes have three different ones is worse than no mark, and
           * quantize made it common: at a quarter per row, three triplet notes
           * share a row and each moves a different distance.
           *
           * Saying nothing is the honest answer. The pill already says how many
           * there are, and the piano roll shows where each one is.
           */
          c0.dev = -1; c0.devOut = 0;
        } else {
          c0.text = pitchName(n.pitch);
          // The contour ribbon's datum. Set alongside the text because it is the
          // same fact — what note is here — read at a glance instead of read.
          c0.pitch = n.pitch;
          /*
           * ...and WHERE IN THE ROW IT ACTUALLY SOUNDS.
           *
           * TWO CAUSES, ONE MARK. A note's own delay pushes it late; its lane's
           * quantize pulls it toward a grid. The engine COMPOSES them — it
           * quantizes the note start on its scheduling copy and adds the delay
           * afterwards — so the sounding tick is `tOn + devTicks + delayTicks` and
           * there is exactly one place the note is heard. Two marks would invent a
           * distinction the audio does not make, and there is no second
           * pseudo-element to spend on it: `::after` is this mark and `::before`
           * is the collide pill.
           *
           * SIGNED, because quantize pulls notes EARLIER as often as later, and a
           * note pulled back before its own row sounds in the row above. Clamped
           * to the cell rather than drawn outside it: a mark in a cell that is not
           * showing the note is worse than no mark, so it pins to the edge and
           * says "at least this far", which is true.
           *
           * `devTicks` comes from the ENGINE (UiClipNote.reserved3), not from a
           * quantize function reimplemented here. That was the whole argument for
           * the field: one implementation, the one that schedules the audio, so
           * the mark cannot disagree with the sound.
           *
           * Only at zooms where a row is a POSITION. At "1 bar" per row the cell
           * holds a count, and a mark inside it would point at a note the cell is
           * not showing.
           */
          /*
           * FROM WHERE THE NOTE IS, not from the start of its row.
           *
           * The row is derived from the authored tick by DIVISION, so a note is
           * generally somewhere INSIDE its row rather than on its edge — and the
           * old formula measured only `delayTicks`, silently assuming every
           * authored note sits exactly on a row line. That was invisible while
           * delay was the only source of movement, because a delay really is
           * measured from the note's own position.
           *
           * Quantize made it wrong and visible in one step: a note authored at tick
           * 62400 that quantize pulls back to 0 has dev = -62400, and measuring
           * that from the row start gives -62400 — "sounds before this row", when
           * it sounds exactly ON it. The note's offset within its row is the
           * missing term.
           */
          const inRow = rowTicks > 0 ? (n.tOn % rowTicks) : 0;
          const soundsAt = inRow + n.delayTicks + (n.devTicks | 0);
          if (zoom.aggregate || soundsAt === 0 || rowTicks <= 0) {
            c0.dev = -1; c0.devOut = 0;
          } else {
            const pct = Math.round((soundsAt / rowTicks) * 100);
            c0.dev = Math.max(0, Math.min(99, pct));
            /*
             * SPILL. A quantize grid coarser than the row can pull a note to a
             * line well before its own row — so the note SOUNDS in a row this
             * cell is not. Pinned to the edge and flagged, rather than clamped
             * silently: a mark sitting at 0% that means "somewhere earlier" is a
             * mark claiming the note is on time, which is the one thing it must
             * never say. -1 is before this row, +1 after.
             */
            c0.devOut = pct < 0 ? -1 : pct > 99 ? 1 : 0;
          }
          // Muted base notes still ship — draw them struck out. Adds carry
          // provenance so an override reads differently from the shared clip.
          c0.kind = n.muted ? 'muted' : n.isAdd ? 'add' : 'note';
          c0._row = ri;
          // Reset the pill's accumulators: this cell holds one note again. aggLo/Hi
          // with it, or a cell that briefly held a chord keeps reporting its range.
          c0.aggCount = 0;
          c0.aggLo = 0; c0.aggHi = 0;
          c0._sameVel = undefined;
          // The first note's velocity, so a later collision can compare against it:
          // by the time the second note arrives this cell no longer knows the first
          // one's, and the collide branch needs both to answer "do they agree".
          c0._firstVel = n.velocity;
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
          // `mix` only when the velocities actually disagree — the old condition
          // asked whether the PITCHES did, which is a different question and left an
          // arbitrary one of several velocities on screen.
          c1.text = c0 && c0.kind === 'collide' && c0._sameVel === null
            ? 'mix' : velocityText(c0 && c0.kind === 'collide'
                                   ? (c0._sameVel === undefined ? n.velocity : c0._sameVel)
                                   : n.velocity);
          c1.kind = 'inst';
        }
      }
      if (columns > 2) {
        /*
         * EVERY OP THIS NOTE CARRIES, one character each.
         *
         * This used to be a priority chain — retrigger beat probability beat
         * delay — so a note carrying `ret3 p60 d1/6` drew `R3` and the other two
         * were invisible while the engine played all three. `parse_row_ops` has
         * always taken a whitespace-separated LIST, so the notation was never
         * single-op; only the display was.
         *
         * The run is interned by mask in rowops.js, so this is a pointer
         * assignment and allocates nothing.
         */
        const c2 = row.cells[base + 2];
        if (c2) {
          /*
           * ONE GLYPH PER OP, always — never the values.
           *
           * This drew the fullest form that FIT for a while: `p100` where there was room, `rpd`
           * where there was not. Two things killed it. It ALLOCATED — a canonical string per
           * cell per frame, because during a scroll every cell genuinely holds a different note
           * and no per-cell cache can help; alloc.mjs measured +200 B/draw and said so. And it
           * was INCONSISTENT: a note with one op showed a value and a note with three showed
           * glyphs, so scanning a column meant reading a mixture.
           *
           * The value is one keypress away — standing on the cell prints the canonical string —
           * which is a better answer than squeezing it in where it happens to fit, and it is
           * what "collapsed is one character for every op" meant in the first place.
           */
          /*
           * THE SELECTED OP IS SHOWN IN FULL; the rest of the cell stays collapsed.
           *
           * `cursor.op` is an index into the run, so the cell under the cursor draws that op's
           * TOKEN — `p60` rather than `p` — which is the expansion the collapsed form implies
           * and the only feedback that says which of forty glyphs `@` is about to open. One
           * text swap, no extra nodes: a caret per glyph would be a DOM node per op on every
           * row that has them.
           */
          const onCursor = cursor.op >= 0 && n.track === cursor.track
                           && scaled === cursor.row
                           && (nc * FIELDS_PER_NOTE + 2) === cursor.col;
          // `ticksPerBeat(meter)` rather than a constant: the delay is stored in ticks and
          // spelled as a fraction of a BEAT, so the token depends on the meter the row is in.
          const token = onCursor ? opTokenAt(n, cursor.op, ticksPerBeat(meter)) : '';
          const run = token || opsRun(n);
          if (run) { c2.text = run; c2.kind = 'fx'; }
        }
      }
    }
    /*
     * CHORDS, after the notes.
     *
     * A chord is not a note and never was: it is a scale DEGREE with a quality
     * and an inversion, resolved against the harmony timeline, which is what
     * lets a chord track survive a key change. The engine has published them all
     * along and nothing on this side ever read them — so a track of chords
     * played and drew an empty column, reported twice as "sound with no notes".
     *
     * In the track's FIRST note column. A chord occupies the whole track at that
     * moment by definition — there is no such thing as a chord in column 3 — so
     * placing it anywhere else would invent a distinction the document does not
     * have.
     *
     * AFTER the notes and only into a cell they left empty. A track with both is
     * ambiguous rather than rich: it would be two different things claiming one
     * position, and quietly drawing the chord over the note is exactly the kind
     * of silent loss the aggregate pill exists to prevent.
     */
    const chordCount = engine.chordCount | 0;
    for (let i = 0; i < chordCount; i++) {
      const ch = engine.chords[i];
      if (ch.tick < winStart || ch.tick >= winEnd || ch.track >= trackCount) continue;
      const scaled = gridScale === 1 ? ch.row : Math.round(ch.row * gridScale);
      const ri = scaled >= startRow ? scaled - startRow : ((ch.tick - winStart) / span) | 0;
      const row = rows[ri];
      if (!row) continue;
      const cell = row.cells[ch.track * columns];
      if (!cell) continue;
      if (cell.kind === 'note' || cell.kind === 'muted' || cell.kind === 'add'
          || cell.kind === 'collide' || cell.kind === 'aggregate') {
        // A note is already here. Said out loud rather than overwritten.
        cell.kind = 'collide';
        continue;
      }
      cell.text = nameChord(ch.degree, ch.quality, ch.inversion);
      cell.kind = 'chord';
      cell._row = ri;
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
    /*
     * THE SELECTED OP IS AN INPUT TO CELL TEXT, so it belongs here.
     *
     * Left out at first, and the symptom was exactly what this comment warns about: the cursor
     * state updated, the draw ran, and the cell kept its old text because nothing rebound it. It
     * read as a dead keybinding. Kept as four separate fields rather than one packed number for
     * the reason above — the packed version aliased and cancelled changes out.
     *
     * -1 when nothing is selected, so ordinary cursor movement does NOT rebind every visible
     * cell. Only stepping between ops does, which is the one case where a cell's text changed.
     */
    const opSel = cursor && cursor.op >= 0 ? cursor.op : -1;
    const opRow = opSel < 0 ? -1 : cursor.row;
    const opTrack = opSel < 0 ? -1 : cursor.track;
    const opCol = opSel < 0 ? -1 : cursor.col;
    if (s.zoomIndex !== zoomIndex || s.pendingCount !== pendingCount
        || s.overlayLen !== overlayLen || s.badKey !== badKey
        || s.opSel !== opSel || s.opRow !== opRow
        || s.opTrack !== opTrack || s.opCol !== opCol
        || s.notesRevision !== (engine ? engine.notesRevision : -1)
        || s.aggRevision !== (engine ? engine.aggRevision : -1)
        || s.rowGrid !== (engine ? engine.rowGrid : -1)
        || s.extentsRevision !== (engine ? engine.extentsRevision : -1)) {
      s.zoomIndex = zoomIndex; s.pendingCount = pendingCount; s.overlayLen = overlayLen;
      s.badKey = badKey;
      s.opSel = opSel; s.opRow = opRow; s.opTrack = opTrack; s.opCol = opCol;
      s.notesRevision = engine ? engine.notesRevision : -1;
      s.aggRevision = engine ? engine.aggRevision : -1;
      s.rowGrid = engine ? engine.rowGrid : -1;
      s.extentsRevision = engine ? engine.extentsRevision : -1;
      contentRevision++;
    }
  }
  buf.contentRevision = contentRevision;

  buf.laneShow = engine ? buf._laneShow : null;
  buf.laneShowSig = laneShowSig;
  buf.laneHidden = engine ? buf._laneHidden : null;
  buf.laneHiddenSig = laneHiddenSig;
  buf.window.startRow = startRow; buf.window.rowCount = rowCount;
  buf.zoom = zoom;
  buf.cursor.row = cursor.row; buf.cursor.track = cursor.track; buf.cursor.col = cursor.col;
  buf.playhead.row = playheadRow;
  buf.selection = selection;
  return buf;
}
