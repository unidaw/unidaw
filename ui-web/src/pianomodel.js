// Piano roll: the third projection of the same note data.
//
// The tracker puts time on Y and columns on X. Arrange puts time on X and tracks
// on Y. This puts time on X and PITCH on Y — and it reads the identical engine
// store, with no new decoding and no engine change, which is the third time that
// boundary has held.
//
// Unlike the tracker, this view has no grid of its own to be wrong about: a note
// is drawn at its tick, not at a row. Lane lines_per_beat still matters, but only
// for the snap when writing, never for where an existing note appears. That is
// why the polyrhythm that took four rounds to get right in the tracker is free
// here — there is no row to misproject onto.

import { trackName } from './arrangemodel.js';

const TICKS_PER_BAR = 3840000;
const TICKS_PER_BEAT = 960000;

/** Which pitches have a black key above them, for drawing the keyboard. */
const BLACK = [false, true, false, true, false, false, true, false, true, false, true, false];
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

export function isBlackKey(pitch) { return BLACK[((pitch % 12) + 12) % 12]; }

/**
 * Every label the key ladder can show, built once.
 *
 * The ladder relabels itself on every draw, and `NAMES[i] + (octave)` is two
 * allocations — the octave number's own string and the concatenation. Only the
 * Cs are labelled, so that was five or six short-lived strings every 16 ms to
 * spell out the same five names the ladder showed last frame, and it kept
 * spelling them while the playhead moved and nothing about the keyboard changed
 * at all. The domain is the 128 MIDI pitches and a pitch's name never changes,
 * so the table is the whole answer. Same fix, same reasoning as `PITCH_NAMES`
 * in wire.js, which the tracker needed for the same reason.
 */
const PITCH_LABELS = new Array(128);
for (let p = 0; p < 128; p++) PITCH_LABELS[p] = NAMES[p % 12] + (Math.floor(p / 12) - 1);

/** MIDI pitch to keyboard notation, e.g. 60 -> "C4". Allocation-free in range. */
export function pitchLabel(pitch) {
  return PITCH_LABELS[pitch] !== undefined
    ? PITCH_LABELS[pitch]
    : NAMES[((pitch % 12) + 12) % 12] + (Math.floor(pitch / 12) - 1);
}

/** Horizontal zoom, shared shape with arrange but finer — this is an editor. */
export const PIANO_ZOOM = [
  { index: 0, ticksPerPixel: 1875, label: 'beat/512px' },
  { index: 1, ticksPerPixel: 3750, label: 'beat/256px' },
  { index: 2, ticksPerPixel: 7500, label: 'beat/128px' },
  { index: 3, ticksPerPixel: 15000, label: 'beat/64px' },
  { index: 4, ticksPerPixel: 30000, label: 'beat/32px' },
  { index: 5, ticksPerPixel: 60000, label: 'beat/16px' },
];

export function createPianoBuffer(noteCapacity = 512, keyCapacity = 128) {
  const notes = new Array(noteCapacity);
  for (let i = 0; i < noteCapacity; i++) {
    notes[i] = { id: 0, pitch: 0, track: 0, tick: 0, x: 0, y: 0, w: 0, h: 0,
                 velocity: 0, muted: false, isAdd: false, selected: false,
                 // The selection key this slot's note has, and the three numbers
                 // it was built from. Declared here rather than added on first
                 // use so every slot keeps one shape. See `slotKey`.
                 _key: '', _kTrack: -1, _kTick: -1, _kPitch: -1 };
  }
  const keys = new Array(keyCapacity);
  for (let i = 0; i < keyCapacity; i++) {
    keys[i] = { pitch: 0, y: 0, h: 0, black: false, label: '', showLabel: false };
  }
  return {
    notes, noteCount: 0,
    keys, keyCount: 0,
    grid: new Float64Array(512), gridCount: 0, gridIsBar: new Uint8Array(512),
    gridFirst: 0,
    ruler: new Float64Array(128), rulerBar: new Int32Array(128), rulerCount: 0,
    // `scrollX` is the CONTENT x of the viewport's left edge, i.e. what the
    // renderer translates the scrolling bands by (negated). See the note on
    // content coordinates above `buildPianoModel`.
    view: { startTick: 0, ticksPerPixel: 15000, width: 0, lowPitch: 36, keyHeight: 10,
            scrollX: 0 },
    playheadX: -1,
    _shape: `${noteCapacity}x${keyCapacity}`,
  };
}

/**
 * These renderers deliberately have NO content revision.
 *
 * The tracker needs one because it binds ~1,700 cells and cannot afford to touch
 * them all every frame. Arrange, the piano roll and the mixer bind tens of
 * elements and guard every individual write, so a per-frame pass is already
 * cheap (0.1 ms measured) and a revision would buy nothing.
 *
 * They each HAD one, computed and then read by nobody. That is worse than not
 * having one: arrange's omitted the per-lane grids, so the first person to trust
 * it would have found lane labels going stale on a project load — this codebase's
 * signature bug, lying in wait behind something that looked like it was handled.
 * If one of these ever needs a revision, write it then, against what the model
 * actually reads at that point.
 */

/**
 * Two horizontal coordinate spaces, and which one each number is in.
 *
 * **Content x** is `tick / ticksPerPixel` — where a tick sits on an unbounded
 * strip that does not know the viewport exists. Gridlines, ruler ticks and notes
 * are laid out in content x, and the renderer slides the whole strip under the
 * viewport with one transform (GUIDELINES 3.3). Before that, every one of them
 * carried `- startTick` in its own position, so a horizontal pan moved a few
 * hundred elements individually and rebuilt a transform string for each: 3,726
 * bytes a frame inside `piano.js` `render`, measured, against 22 for the same
 * redraw standing still. Nothing about a note changes when you scroll past it,
 * and the layout now says so.
 *
 * **Viewport x** is content x minus `view.scrollX`: a pixel on the band as the
 * pointer sees it. Everything OUTSIDE the transformed strips is in viewport x —
 * the playhead (which moves against a still background, so it costs one string a
 * frame either way) and the marquee (which is drawn where the pointer is, and
 * would be wrong if it scrolled). So is every rectangle arriving from an event.
 *
 * The conversion is `view.scrollX`, and `notesInRect` is the one place that has
 * to apply it — it is asked in viewport x about notes stored in content x.
 *
 * Vertical is deliberately NOT the same shape. A pitch's y depends on
 * `lowPitch`, so the pitch window moves by rebinding; that is correct here
 * because moving it also relabels the key ladder, which no transform can do, and
 * because it is bound to a keystroke (an octave at a time) rather than to a
 * frame. `startTick` and `lowPitch` are independent, so a horizontal pan leaves
 * every y untouched and its guard unfired.
 *
 * @param {{startTick:number, width:number, height:number, zoomIndex:number,
 *          lowPitch:number, keyHeight:number, engine:object|null,
 *          track:number, allTracks:boolean, selectedNote:number}} opts
 */
export function buildPianoModel(opts, buf) {
  const {
    startTick = 0, width = 1200, height = 600, zoomIndex = 3,
    lowPitch = 36, keyHeight = 10, engine = null,
    track = 0, allTracks = false, selectedNote = -1,
    // A Set of "track:tick" keys, NOT note ids.
    //
    // The engine assigns a new id when it rewrites a note, so an id-keyed
    // selection empties itself the moment you transpose it — the selection
    // survives the edit but stops matching what it selected. (track, tOn) is
    // stable across a rewrite and disappears on a delete, which is exactly the
    // behaviour a selection should have.
    //
    // Separate from selectedNote (the click target) because "the note I clicked"
    // and "the notes an operation applies to" are different questions.
    selection = null,
    marquee = null,
    dragId = undefined,
  } = opts;

  const zoom = PIANO_ZOOM[Math.max(0, Math.min(PIANO_ZOOM.length - 1, zoomIndex))];
  const tpp = zoom.ticksPerPixel;
  const endTick = startTick + width * tpp;
  const visibleKeys = Math.min(buf.keys.length, Math.ceil(height / keyHeight) + 1);
  const highPitch = lowPitch + visibleKeys;

  buf.view.startTick = startTick;
  buf.view.ticksPerPixel = tpp;
  buf.view.width = width;
  buf.view.lowPitch = lowPitch;
  buf.view.keyHeight = keyHeight;
  // Content x of the left edge. Computed from the same `tpp` the layout below
  // uses, so the two cannot disagree about which content space they are in: a
  // zoom changes both in the same pass or neither.
  buf.view.scrollX = startTick / tpp;
  buf.zoom = zoom;

  // Keys, top to bottom: high pitch at the top, as a keyboard stands.
  let k = 0;
  for (let p = highPitch - 1; p >= lowPitch && k < buf.keys.length; p--) {
    const key = buf.keys[k];
    key.pitch = p;
    key.y = (highPitch - 1 - p) * keyHeight;
    key.h = keyHeight;
    key.black = isBlackKey(p);
    key.showLabel = p % 12 === 0;                 // label the Cs only
    key.label = key.showLabel ? pitchLabel(p) : '';
    k++;
  }
  buf.keyCount = k;

  let g = 0;
  const beatPx = TICKS_PER_BEAT / tpp;
  const step = beatPx >= 8 ? TICKS_PER_BEAT / 4 : beatPx >= 4 ? TICKS_PER_BEAT : TICKS_PER_BAR;
  // The ABSOLUTE index of the first line — how many steps from tick 0 it is,
  // not where it lands in the array. The renderer recycles the gridline pool as
  // a ring on this number (GUIDELINES 3.4), so a scroll of one step rebinds the
  // one line that crossed the edge instead of shifting all ninety along by one.
  const firstGrid = Math.floor(startTick / step);
  buf.gridFirst = firstGrid;
  for (let tick = firstGrid * step; tick < endTick && g < buf.grid.length; tick += step) {
    buf.grid[g] = tick / tpp;                     // content x; see above
    buf.gridIsBar[g] = tick % TICKS_PER_BAR === 0 ? 1 : 0;
    g++;
  }
  buf.gridCount = g;

  const barPx = TICKS_PER_BAR / tpp;
  const every = barPx >= 48 ? 1 : barPx >= 24 ? 2 : 4;
  let r = 0;
  for (let bar = Math.floor(startTick / TICKS_PER_BAR); r < buf.ruler.length; bar++) {
    const tick = bar * TICKS_PER_BAR;
    if (tick >= endTick) break;
    if (bar % every !== 0) continue;
    buf.ruler[r] = tick / tpp;                    // content x; see above
    buf.rulerBar[r] = bar + 1;
    r++;
  }
  buf.rulerCount = r;

  let n = 0;
  if (engine) {
    for (let i = 0; i < engine.noteCount && n < buf.notes.length; i++) {
      const src = engine.notes[i];
      if (!allTracks && src.track !== track) continue;
      if (src.pitch < lowPitch || src.pitch >= highPitch) continue;
      if (src.tOff <= startTick || src.tOn >= endTick) continue;
      const d = buf.notes[n++];
      d.id = src.id;
      d.pitch = src.pitch;
      d.track = src.track;
      d.tick = src.tOn;
      d.velocity = src.velocity;
      d.muted = src.muted;
      d.isAdd = src.isAdd;
      // After track, tick and pitch are set: `slotKey` reads them off the slot.
      d.selected = src.id === selectedNote
                 || (selection !== null && selection.has(slotKey(d)));
      d.x = src.tOn / tpp;                        // content x; see above
      d.y = (highPitch - 1 - src.pitch) * keyHeight;
      d.h = keyHeight;
      // A zero-length note is a real thing the engine can hold and it must not
      // become invisible — the whole point of drawing it is so you can find it.
      d.w = Math.max(2, (src.tOff - src.tOn) / tpp);
    }
  }
  buf.noteCount = n;

  buf.trackName = allTracks ? 'all tracks' : trackName(engine, track);
  buf.marquee = marquee;
  buf.dragId = dragId;
  buf.selectedCount = selection ? selection.size : 0;

  // VIEWPORT x, not content x: the playhead is not on a scrolling strip. It has
  // to be drawn against the still background because it moves while the strip
  // does not, so putting it on the strip would buy nothing — it would still be
  // one transform a frame — and would cost it the ability to be clamped to the
  // visible band.
  buf.playheadX = engine && engine.playheadTick >= startTick && engine.playheadTick < endTick
    ? (engine.playheadTick - startTick) / tpp
    : -1;
  return buf;
}

/**
 * Note ids inside a pixel rectangle. Rectangle in the band's coordinates, the
 * same ones the model lays notes out in, so this is a plain overlap test rather
 * than a second projection that could disagree with the first.
 */
/** The stable identity of a note for selection purposes. See `selection` above. */
/**
 * A note's identity for selection purposes: track, start, PITCH.
 *
 * Not the note id, which the engine reassigns when it rewrites a track. Not
 * (track, start) either, which was the first fix and is not unique — a CHORD is
 * several notes sharing a start, so they collapsed to one key and a selection
 * containing a chord operated on whichever one the Set happened to answer for.
 * It survived a long time because the fixtures had no chords in the marquee.
 *
 * The cost is that the key MOVES when a note is transposed, so an edit that
 * changes pitch has to remap the selection by the same amount. That is what
 * `transposedKey` is for, and it is the honest trade: a key that is unique and
 * moves predictably beats a key that is stable and ambiguous.
 */
export function noteKey(n) { return n.track + ':' + n.tOn + ':' + n.pitch; }

/**
 * The same key as `noteKey`, for the note currently in a view-model slot, built
 * only when the slot changes hands.
 *
 * The model asks "is this note selected?" once per visible note per frame, and
 * a `Set` of strings can only be asked in strings — so with a marquee up this
 * was three concatenations per note per frame, a few hundred short-lived strings
 * every 16 ms to spell out keys that were nearly all the same as last frame's.
 *
 * The key is a pure function of (track, tOn, pitch) and the slot carries all
 * three, so caching it beside them is exact: the string cannot go stale without
 * one of those numbers moving, and when one moves the guard rebuilds it. Note
 * that this is keyed on the note's identity and NOT on the slot index — a scroll
 * hands a slot to a different note, which changes the numbers, which is caught.
 */
function slotKey(d) {
  if (d._kTrack !== d.track || d._kTick !== d.tick || d._kPitch !== d.pitch) {
    d._kTrack = d.track; d._kTick = d.tick; d._kPitch = d.pitch;
    d._key = d.track + ':' + d.tick + ':' + d.pitch;
  }
  return d._key;
}

/** The key a note WILL have once it is transposed by `semitones`. */
export function transposedKey(n, semitones) {
  return n.track + ':' + n.tOn + ':' + (n.pitch + semitones);
}

export function notesInRect(buf, x0, y0, x1, y1) {
  // The rectangle arrives in VIEWPORT x — it was drawn by a pointer — and the
  // notes are laid out in CONTENT x. One conversion, at the boundary, so the
  // overlap test below stays a plain compare in one space.
  const sx = buf.view.scrollX || 0;
  const lo = Math.min(x0, x1) + sx, hi = Math.max(x0, x1) + sx;
  const top = Math.min(y0, y1), bot = Math.max(y0, y1);
  const out = new Set();
  for (let i = 0; i < buf.noteCount; i++) {
    const n = buf.notes[i];
    if (n.x + n.w < lo || n.x > hi) continue;
    if (n.y + n.h < top || n.y > bot) continue;
    // `slotKey`, not a hand-rolled key. This built a TWO-part `track:tick` while
    // `noteKey` — the only thing that ever reads the result — built the
    // three-part `track:tOn:pitch`. The pitch was added to `noteKey` by the
    // chord fix documented there and never reached here, so no marquee key ever
    // matched an engine note: selected notes drew unselected and every
    // `pianoEdit` on a marquee selection took the "select notes first" path.
    // The slot carries the same three numbers under the name `tick`, which is
    // exactly what `slotKey` reads, and it is cached on the slot besides.
    out.add(slotKey(n));
  }
  return out;
}

/**
 * A pitch window that actually contains the material.
 *
 * Opening a piano roll onto empty sky is the default failure of this view: MIDI
 * is 128 notes tall and almost no music uses more than two octaves of it, so a
 * fixed window is wrong nearly always. Returns the low pitch that centres what
 * is there, or the supplied fallback when there is nothing to centre on.
 */
export function fitLowPitch(engine, visibleKeys, track, allTracks, fallback = 48) {
  if (!engine || !engine.noteCount) return fallback;
  let lo = 128, hi = -1;
  for (let i = 0; i < engine.noteCount; i++) {
    const n = engine.notes[i];
    if (!allTracks && n.track !== track) continue;
    if (n.pitch < lo) lo = n.pitch;
    if (n.pitch > hi) hi = n.pitch;
  }
  if (hi < 0) return fallback;
  const centre = (lo + hi) / 2;
  return Math.max(0, Math.min(127 - visibleKeys + 1, Math.round(centre - visibleKeys / 2)));
}

/**
 * Pixel position back to (pitch, tick), for hit testing and writing.
 *
 * `x` is VIEWPORT x — what a pointer event yields once the band's own rect is
 * subtracted — which is why `startTick` appears here and not in the layout. The
 * inverse, content x, is `tick / view.ticksPerPixel`; the two differ by
 * `view.scrollX` and mixing them lands clicks on the wrong notes.
 */
export function pitchAtY(view, y, visibleKeys) {
  const high = view.lowPitch + visibleKeys;
  return high - 1 - Math.floor(y / view.keyHeight);
}
export function tickAtX(view, x) {
  return view.startTick + x * view.ticksPerPixel;
}
