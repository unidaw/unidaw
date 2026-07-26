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
export function pitchLabel(pitch) {
  return NAMES[((pitch % 12) + 12) % 12] + (Math.floor(pitch / 12) - 1);
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
                 velocity: 0, muted: false, isAdd: false, selected: false };
  }
  const keys = new Array(keyCapacity);
  for (let i = 0; i < keyCapacity; i++) {
    keys[i] = { pitch: 0, y: 0, h: 0, black: false, label: '', showLabel: false };
  }
  return {
    notes, noteCount: 0,
    keys, keyCount: 0,
    grid: new Float64Array(512), gridCount: 0, gridIsBar: new Uint8Array(512),
    ruler: new Float64Array(128), rulerBar: new Int32Array(128), rulerCount: 0,
    view: { startTick: 0, ticksPerPixel: 15000, width: 0, lowPitch: 36, keyHeight: 10 },
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
  const first = Math.floor(startTick / step) * step;
  for (let tick = first; tick < endTick && g < buf.grid.length; tick += step) {
    buf.grid[g] = (tick - startTick) / tpp;
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
    buf.ruler[r] = (tick - startTick) / tpp;
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
      d.selected = src.id === selectedNote
                 || (selection !== null && selection.has(noteKey(src)));
      d.x = (src.tOn - startTick) / tpp;
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
  buf.selectedCount = selection ? selection.size : 0;

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
export function noteKey(n) { return n.track + ':' + n.tOn; }

export function notesInRect(buf, x0, y0, x1, y1) {
  const lo = Math.min(x0, x1), hi = Math.max(x0, x1);
  const top = Math.min(y0, y1), bot = Math.max(y0, y1);
  const out = new Set();
  for (let i = 0; i < buf.noteCount; i++) {
    const n = buf.notes[i];
    if (n.x + n.w < lo || n.x > hi) continue;
    if (n.y + n.h < top || n.y > bot) continue;
    out.add(n.track + ':' + n.tick);
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

/** Pixel position back to (pitch, tick), for hit testing and writing. */
export function pitchAtY(view, y, visibleKeys) {
  const high = view.lowPitch + visibleKeys;
  return high - 1 - Math.floor(y / view.keyHeight);
}
export function tickAtX(view, x) {
  return view.startTick + x * view.ticksPerPixel;
}
