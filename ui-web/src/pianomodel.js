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
    notes[i] = { id: 0, pitch: 0, track: 0, x: 0, y: 0, w: 0, h: 0,
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
    contentRevision: 0,
    _shape: `${noteCapacity}x${keyCapacity}`,
  };
}

const SIG = { notesRevision: -2, zoomIndex: -1, startTick: -1, width: -1,
              lowPitch: -1, keyCount: -1, track: -1, selected: -1 };
let contentRevision = 0;

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
      d.velocity = src.velocity;
      d.muted = src.muted;
      d.isAdd = src.isAdd;
      d.selected = src.id === selectedNote;
      d.x = (src.tOn - startTick) / tpp;
      d.y = (highPitch - 1 - src.pitch) * keyHeight;
      d.h = keyHeight;
      // A zero-length note is a real thing the engine can hold and it must not
      // become invisible — the whole point of drawing it is so you can find it.
      d.w = Math.max(2, (src.tOff - src.tOn) / tpp);
    }
  }
  buf.noteCount = n;

  buf.playheadX = engine && engine.playheadTick >= startTick && engine.playheadTick < endTick
    ? (engine.playheadTick - startTick) / tpp
    : -1;

  if (SIG.notesRevision !== (engine ? engine.notesRevision : -1)
      || SIG.zoomIndex !== zoomIndex || SIG.startTick !== startTick
      || SIG.width !== width || SIG.lowPitch !== lowPitch
      || SIG.keyCount !== k || SIG.track !== (allTracks ? -2 : track)
      || SIG.selected !== selectedNote) {
    SIG.notesRevision = engine ? engine.notesRevision : -1;
    SIG.zoomIndex = zoomIndex; SIG.startTick = startTick; SIG.width = width;
    SIG.lowPitch = lowPitch; SIG.keyCount = k;
    SIG.track = allTracks ? -2 : track; SIG.selected = selectedNote;
    contentRevision++;
  }
  buf.contentRevision = contentRevision;
  return buf;
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
