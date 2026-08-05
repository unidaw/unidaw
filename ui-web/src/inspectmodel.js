// What is IN this cell, in words.
//
// The tracker is a grid of three-character fields. That is the right density for writing
// music and the wrong density for reading it: a cell showing `C-4` says nothing about how
// long the note is, how hard it was struck, which of three columns it occupies, or which of
// seven per-row operations are riding on it. A chord cell is worse — `IV7` is a degree, a
// quality and an inversion resolved against the harmony timeline, plus a strum, and none of
// that fits in three characters or is inferable from them.
//
// The question this answers, asked while looking at a chord: "how do I know whether it's a
// strum?" You could not. The spread and the two humanize amounts cross the wire on every
// frame and were decoded away one layer below the interface (fixed in wire 28), and no
// surface displayed them even once they were not.
//
// A MODEL, not a renderer. It takes the cell and the things that might be in it and returns
// lines; it reads no DOM, holds no state and can be run against a hand-made note in a unit
// test. The view below it does nothing but write these strings into pooled nodes.
//
// ALLOCATION. `buildInspectModel` writes into a caller-owned buffer and returns it. The
// inspector redraws whenever the cursor moves or the pointer crosses a cell, which is a
// gesture and not a loop — but it also redraws on the frames after those, and a fresh
// object per frame is exactly the regression test/alloc.mjs exists to catch. The rows are
// pooled and the strings are only rebuilt when the CELL changes, keyed by `_key`.

import { OP_MASK, ROW_OPS, opsText } from './rowops.js';

/** Roman numerals for a degree, the way every other surface in this app writes them. */
const DEGREES = ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII'];
/** The engine's quality numbering: 0 = the degree alone, 1 = triad, 2 = seventh. */
const QUALITIES = ['degree', 'triad', 'seventh'];
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** MIDI pitch to the name the tracker draws. */
export function pitchName(p) {
  const n = Number(p);
  if (!Number.isFinite(n) || n < 0) return '--';
  return NOTE_NAMES[n % 12] + '-' + (Math.floor(n / 12) - 1);
}

/**
 * Nanoticks as something a person can act on.
 *
 * Both units, because both are the answer to a different question: "a 16th" is what you
 * meant, and the raw count is what you have to type into a command. Printing only the
 * musical name would make the value unusable from the console, and printing only the count
 * makes a reader do arithmetic to find out it is an ordinary sixteenth.
 */
export function ticksLabel(ticks, ticksPerBeat) {
  const t = Number(ticks) || 0;
  const q = Number(ticksPerBeat) || 960000;
  if (t === 0) return '0';
  const beats = t / q;
  const NAMED = [[4, 'whole'], [2, 'half'], [1, 'quarter'], [0.5, '8th'],
                 [0.25, '16th'], [0.125, '32nd'], [1 / 3, 'triplet 8th'],
                 [2 / 3, 'triplet quarter'], [1 / 6, 'triplet 16th']];
  for (const [v, name] of NAMED) {
    // Exact-ish: the triplet values are not representable, so a tolerance is required
    // rather than optional. Relative, so it does not become sloppy at large values.
    if (Math.abs(beats - v) < v * 1e-6) return `${name} · ${t}nt`;
  }
  return `${beats.toFixed(3)} beats · ${t}nt`;
}

/** A fresh buffer. One per inspector; reused for every cell it ever shows. */
export function createInspectBuffer(maxRows = 20) {
  const rows = new Array(maxRows);
  for (let i = 0; i < maxRows; i++) rows[i] = { label: '', value: '', kind: '' };
  return {
    title: '', subtitle: '', rows, count: 0, empty: true,
    /*
     * THE LAST INPUTS, field by field, so "has this cell changed" costs comparisons and
     * NOT A STRING.
     *
     * The first version joined every input into a key and compared that. It was correct
     * and it allocated a string per frame per cell — which put four tracker scenarios over
     * the allocation budget the moment the panel was mounted, at 903..1953 B/draw against
     * limits of 900 and 1200. Nothing about the panel was wrong; the CHANGE DETECTOR was
     * the cost. Allocating to find out whether you need to do any work is the shape
     * alloc.mjs exists to catch (GUIDELINES 3).
     */
    _c: {
      row: -1, track: -1, col: -1, field: -1, hovered: false,
      nId: -1, nPitch: -1, nVel: -1, nOn: -1, nOff: -1,
      nRet: -1, nProb: -1, nSound: -1, nSoundOff: -1, nDelay: -1, nRamp: -1, nCond: -1,
      cId: -1, cDeg: -1, cQual: -1, cInv: -1, cOct: -1,
      cSpread: -1, cHt: -1, cHv: -1, cDur: -1,
    },
  };
}

/** -1 when absent, so a missing field and a zero one are never confused. */
function num(v) { return v === undefined || v === null ? -1 : Number(v); }

/**
 * Has anything that decides the drawing moved? Updates the cache as it compares.
 *
 * One pass, no allocation, and it must list EVERY field the body below reads — a field
 * read but not compared is a panel that stops updating for that one value, which is
 * indistinguishable from a value that did not change.
 */
function changed(c, note, chord, k) {
  let diff = false;
  const set = (key, v) => { if (k[key] !== v) { k[key] = v; diff = true; } };
  set('row', c.row); set('track', c.track); set('col', c.col);
  set('field', c.field); set('hovered', !!c.hovered);
  set('nId', note ? num(note.id) : -1);
  set('nPitch', note ? num(note.pitch) : -1);
  set('nVel', note ? num(note.velocity) : -1);
  set('nOn', note ? num(note.tOn) : -1);
  set('nOff', note ? num(note.tOff) : -1);
  set('nRet', note ? num(note.retrigger) : -1);
  set('nProb', note ? num(note.probability) : -1);
  set('nSound', note ? num(note.sound) : -1);
  set('nSoundOff', note ? num(note.soundOffset) : -1);
  set('nDelay', note ? num(note.delayNanoticks) : -1);
  set('nRamp', note ? num(note.retrigRamp) : -1);
  set('nCond', note ? num(note.trigCondition) : -1);
  set('cId', chord ? num(chord.id) : -1);
  set('cDeg', chord ? num(chord.degree) : -1);
  set('cQual', chord ? num(chord.quality) : -1);
  set('cInv', chord ? num(chord.inversion) : -1);
  set('cOct', chord ? num(chord.octave) : -1);
  set('cSpread', chord ? num(chord.spread) : -1);
  set('cHt', chord ? num(chord.humanizeTiming) : -1);
  set('cHv', chord ? num(chord.humanizeVelocity) : -1);
  set('cDur', chord ? num(chord.duration) : -1);
  return diff;
}

/**
 * Describe one cell.
 *
 * @param {{row:number, track:number, col:number, field:number, trackName:string,
 *          note:object|null, chord:object|null, ticksPerBeat:number,
 *          tick:number, bar:number, beat:number, hovered:boolean}} c
 * @param {object} buf a buffer from createInspectBuffer
 *
 * `field` is which of the cell's parts the cursor is on — 0 note, 1 velocity, 2 ops — and
 * it only changes the HIGHLIGHT, never the content. An inspector that showed different
 * facts depending on which third of a cell you were standing in would be a puzzle.
 */
export function buildInspectModel(c, buf) {
  const note = c.note || null;
  const chord = c.chord || null;
  // Standing still costs a run of comparisons and nothing else. See `_c` above for why
  // this is not a string.
  if (!changed(c, note, chord, buf._c)) return buf;

  let n = 0;
  const put = (label, value, kind) => {
    if (n >= buf.rows.length) return;
    const r = buf.rows[n++];
    if (r.label !== label) r.label = label;
    const v = String(value);
    if (r.value !== v) r.value = v;
    const k = kind || '';
    if (r.kind !== k) r.kind = k;
  };

  /*
   * THE ROW NUMBER ONLY WHEN IT IS NOT ALREADY ON SCREEN.
   *
   * Following the cursor, the row is in the gutter two inches to the left and repeating it
   * costs a built string on every row the cursor crosses — which is every frame while
   * stepping, and it is what put the tracker over its allocation budget. Hovering is the
   * case where the panel is describing somewhere the cursor is NOT, and there the row is
   * the whole point, so it is spelled out and labelled.
   */
  buf.subtitle = c.hovered
    ? `row ${c.row} · ${c.trackName || 'track ' + (c.track + 1)} · hovered`
    : (c.trackName || '');

  if (chord) {
    const deg = DEGREES[chord.degree % 7] || String(chord.degree + 1);
    const qual = QUALITIES[chord.quality] || `quality ${chord.quality}`;
    buf.title = `${deg}${chord.quality === 2 ? '7' : ''} · chord`;
    buf.empty = false;
    put('degree', `${deg} (${chord.degree + 1})`);
    put('quality', qual);
    put('inversion', chord.inversion === 0 ? 'root position' : `inversion ${chord.inversion}`);
    put('octave', String(chord.octave));
    /*
     * THE ANSWER TO THE QUESTION THAT PROMPTED ALL OF THIS.
     *
     * A strum is a spread greater than zero: the voices are dealt out across that many
     * nanoticks instead of landing together. Said in words first and in numbers after,
     * because "is this a strum" is a yes/no and "how much" is the follow-up — and a
     * BLOCK CHORD is stated positively rather than by the absence of a line, so the
     * inspector never leaves you wondering whether it simply did not know.
     */
    if (chord.spread > 0) {
      put('strum', `yes — ${ticksLabel(chord.spread, c.ticksPerBeat)} across the voices`,
          'strong');
    } else {
      put('strum', 'no — a block chord, every voice together', 'muted');
    }
    if (chord.humanizeTiming > 0 || chord.humanizeVelocity > 0) {
      put('humanize', `timing ${chord.humanizeTiming} · velocity ${chord.humanizeVelocity}`);
    } else {
      put('humanize', 'none — exact', 'muted');
    }
    if (chord.duration > 0) put('length', ticksLabel(chord.duration, c.ticksPerBeat));
    put('id', String(chord.id), 'muted');
  } else if (note) {
    buf.title = `${pitchName(note.pitch)} · note`;
    buf.empty = false;
    put('pitch', `${pitchName(note.pitch)} (${note.pitch})`);
    put('velocity', String(note.velocity));
    const len = (Number(note.tOff) || 0) - (Number(note.tOn) || 0);
    put('length', ticksLabel(len, c.ticksPerBeat));
    put('column', String((note.column | 0) + 1));

    /*
     * THE ROW OPS, one line each and named. The tracker collapses all seven into a
     * three-character glyph run — `r4p60` — which is dense enough to write with and
     * impossible to read back. Only the ops that are PRESENT are listed: a table of seven
     * rows of "none" would bury the two that are set.
     */
    let ops = 0;
    for (const op of ROW_OPS) {
      const v = note[op.field];
      if (v === undefined || v === null || v === 0) continue;
      ops++;
      // The op's own canonical text, so the inspector and the cell agree literally.
      put(op.field, op.text ? String(op.text(v)) : String(v));
    }
    if (ops === 0) put('row ops', 'none', 'muted');
    else put('as typed', opsText(note, c.ticksPerBeat), 'muted');
    put('id', String(note.id), 'muted');
  } else {
    buf.title = 'empty';
    buf.empty = true;
    put('cell', 'nothing here', 'muted');
  }

  /*
   * Position last: it is context, not the answer. NOT SHOWN FOR AN EMPTY CELL — `String(tick)`
   * is a fresh string on every row the cursor crosses, and "the tick of a cell with nothing in
   * it" is a fact nobody has ever needed. On a note or a chord it is what you type into a
   * command, so it earns its allocation there.
   */
  if (!buf.empty) {
    put('tick', String(c.tick), 'muted');
    if (c.bar > 0) put('at', `bar ${c.bar} · beat ${c.beat}`, 'muted');
  }

  buf.count = n;
  return buf;
}
