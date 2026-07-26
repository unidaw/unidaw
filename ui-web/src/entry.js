// Cell entry.
//
// Three different things happen depending on the column, and conflating them is
// what made typing unpredictable — `w` sometimes produced `D-4` and sometimes a
// literal `w`, because a piano key and the first character of a typed token are
// the same keystroke and whichever branch ran first won.
//
//   note column      a piano key IS the edit. It commits on the keydown; there
//                    is no buffer to be in, so nothing can be half-typed.
//   value columns    digits shift into a fixed-width field and commit on EVERY
//                    keystroke. Nothing to confirm — the field is always whole.
//   chord tokens     genuinely need free text (`@3^7~80h20`), so they get the
//                    buffer, opened explicitly with `@` and closed with Enter.
//                    This is the ONLY thing Enter is for.
//
// The rule that keeps it predictable: the buffer is never entered implicitly.
// If you did not type `@`, no keystroke is ever waiting for a confirmation.

const NOTE_LETTERS = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };

/**
 * The QWERTY piano layout. Two rows, an octave apart, as every tracker has it:
 * `z` is C in the current octave and `q` is C an octave above, so `z`=C-3 while
 * `q`=C-4, `b`=G-3, `u`=B-4, `i`=C-5, `m`=B-3.
 *
 * `a` is deliberately absent — it is note-off, which is the one edit in this
 * column that is not a pitch.
 */
export const NOTE_KEYS = {
  q: 0, '2': 1, w: 2, '3': 3, e: 4, r: 5, '5': 6, t: 7, '6': 8, y: 9, '7': 10, u: 11, i: 12,
  z: -12, s: -11, x: -10, d: -9, c: -8, v: -7, g: -6, b: -5, h: -4, n: -3, j: -2, m: -1,
};

/** Note-off. Its own key because it is an event, not a pitch. */
export const NOTE_OFF_KEY = 'a';

/** Opens the token buffer. The only route into free-text entry. */
export const TOKEN_KEY = '@';

const NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
export function pitchToToken(p) { return NAMES[p % 12] + Math.floor(p / 12 - 1); }

/** Pitch for a piano key in a given octave, or -1 if out of MIDI range. */
export function pitchOf(key, octave) {
  const semis = NOTE_KEYS[key];
  if (semis === undefined) return -1;
  const p = 12 * (octave + 1) + semis;
  return p >= 0 && p <= 127 ? p : -1;
}

/** A hex digit's value, or -1. Value columns are hex, as trackers have them. */
export function hexValue(key) {
  const c = key.toLowerCase();
  if (c >= '0' && c <= '9') return c.charCodeAt(0) - 48;
  if (c >= 'a' && c <= 'f') return c.charCodeAt(0) - 87;
  return -1;
}

/**
 * Shift a digit into a fixed-width field, tracker style: the field holds the
 * last `width` digits typed, so 6 then 4 reads 06 then 64.
 */
export function shiftDigit(current, digit, width = 2) {
  const max = Math.pow(16, width) - 1;
  return Math.min(max, (current * 16 + digit) % Math.pow(16, width));
}

/**
 * Parse a chord token: `@3^7~80h20`.
 *
 *   @<degree>   scale degree, 1-based as musicians write it (I, II, III…)
 *   ^<n>        quality: ^1 single note, ^3 triad (default), ^7 seventh
 *   i<n>        inversion
 *   o<n>        base octave
 *   ~<n>        strum spread, in nanoticks
 *   h<n>        humanize (timing and velocity together)
 *   /<n>        duration in nanoticks; omitted means until the next event
 *
 * A chord is (degree, quality, inversion) against the harmony timeline's scale,
 * NOT absolute semitones — which is the point: the chord track survives a key
 * change. Degrees are 1-based here and 0-based on the wire, converted once, at
 * this boundary, so neither side has to remember which convention it is in.
 */
export function parseChord(text) {
  const s = text.trim().toLowerCase();
  if (s[0] !== '@') return null;
  const m = /^@(\d+)/.exec(s);
  if (!m) return { kind: 'invalid', why: 'chord needs a degree, e.g. @3' };
  const degree = parseInt(m[1], 10);
  if (degree < 1 || degree > 64) return { kind: 'invalid', why: 'degree out of range: ' + degree };
  const pick = (re, dflt) => { const x = re.exec(s); return x ? parseInt(x[1], 10) : dflt; };
  const qual = pick(/\^(\d+)/, 3);
  const quality = qual === 1 ? 0 : qual === 7 ? 2 : 1;    // 0 single, 1 triad, 2 seventh
  const h = pick(/h(\d+)/, 0);
  return {
    kind: 'chord',
    degree: degree - 1,                                   // wire is 0-based
    quality,
    inv: pick(/i(\d+)/, 0),
    oct: pick(/o(\d+)/, 4),
    spread: pick(/~(\d+)/, 0),
    ht: h, hv: h,
    dur: pick(/\/(\d+)/, 0),
  };
}

/**
 * Parse one typed token. Returns a typed result the caller turns into a command,
 * or `{ kind: 'invalid' }` — never a silent drop. A malformed token has to be
 * visible as malformed, because a cell that quietly ignores what you typed is
 * indistinguishable from one that accepted it.
 *
 *   C-4      absolute note
 *   c#3      absolute note, loose case and no dash
 *   24-4     degree 24 at octave 4  (degree notes, per AGENTS.md)
 *   ---      note off
 */
export function parseToken(text, column) {
  const chord = parseChord(text);
  if (chord) return chord;
  const s = text.trim().toLowerCase();
  if (!s) return { kind: 'empty' };
  if (s === '---' || s === 'off') return { kind: 'off' };

  if (column === 0) {
    const m = /^([a-g])([#b]?)-?(-?\d)$/.exec(s);
    if (m) {
      let semis = NOTE_LETTERS[m[1]];
      if (m[2] === '#') semis += 1;
      if (m[2] === 'b') semis -= 1;
      const pitch = 12 * (parseInt(m[3], 10) + 1) + semis;
      return pitch >= 0 && pitch <= 127 ? { kind: 'note', pitch } : { kind: 'invalid' };
    }
    const d = /^(\d+)-(-?\d)$/.exec(s);
    if (d) return { kind: 'degree', degree: parseInt(d[1], 10), octave: parseInt(d[2], 10) };
    return { kind: 'invalid' };
  }

  if (/^\d+$/.test(s)) return { kind: 'value', value: parseInt(s, 10) };
  if (/^[0-9a-f]{1,2}$/.test(s)) return { kind: 'value', value: parseInt(s, 16) };
  return { kind: 'invalid' };
}

/** The transient token buffer. Only ever active for chord/degree entry. */
export function createEntry() {
  return { active: false, row: -1, track: -1, col: -1, text: '' };
}

/** True if this cell is the one being typed into. */
export function editing(entry, row, track, col) {
  return entry.active && entry.row === row && entry.track === track && entry.col === col;
}

export function begin(entry, cursor, initial = '') {
  entry.active = true;
  entry.row = cursor.row; entry.track = cursor.track; entry.col = cursor.col;
  entry.text = initial;
}

export function cancel(entry) { entry.active = false; entry.text = ''; }

/**
 * Feed a keystroke to an ALREADY-OPEN token buffer. Never opens one: the caller
 * decides that, on `@` alone. Returns 'consumed', 'commit', 'cancel' or 'ignore'.
 */
export function feed(entry, key) {
  if (!entry.active) return 'ignore';
  if (key === 'Escape') { cancel(entry); return 'cancel'; }
  if (key === 'Enter') return 'commit';
  if (key === 'Backspace') {
    entry.text = entry.text.slice(0, -1);
    // Backspacing past the opening `@` leaves the buffer, so the next keystroke
    // is a piano key again rather than silently more text.
    if (!entry.text) { cancel(entry); return 'cancel'; }
    return 'consumed';
  }
  if (key.length !== 1) return 'ignore';
  // `/` is part of the chord grammar (duration), so it must reach the buffer.
  if (!/[0-9a-zA-Z#\-.^~@/]/.test(key)) return 'ignore';
  if (entry.text.length < 12) entry.text += key;
  return 'consumed';
}
