// Per-cell text entry.
//
// AGENTS.md: "Tracker accepts free-text tokens per cell for notes, degree notes,
// and chord tokens (e.g., C-4, 24-4, @3^7~80h20)." That is a text buffer with a
// parser, not a keymap — a keymap can only ever produce the note column, and it
// cannot express a degree, a chord, or an effect argument at all.
//
// The buffer is per-cell and transient: it exists while you are typing into one
// cell, commits on a resolving keystroke, and is discarded on Escape or on
// moving away. Nothing here touches the engine; the caller decides what a
// committed token means.

const NOTE_LETTERS = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };

/**
 * The QWERTY piano layout, kept because it is how a tracker is actually played.
 * It is a fast path INTO the buffer, not an alternative to it: pressing `q`
 * fills the cell with "C-4" and commits, which is exactly what typing C-4 by
 * hand would have done.
 */
export const NOTE_KEYS = {
  q: 0, '2': 1, w: 2, '3': 3, e: 4, r: 5, '5': 6, t: 7, '6': 8, y: 9, '7': 10, u: 11, i: 12,
  z: -12, s: -11, x: -10, d: -9, c: -8, v: -7, g: -6, b: -5, h: -4, n: -3, j: -2, m: -1,
};

const NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
export function pitchToToken(p) { return NAMES[p % 12] + Math.floor(p / 12 - 1); }

/**
 * Parse one cell token. Returns a typed result the caller turns into a command,
 * or `{ kind: 'invalid' }` — never a silent drop. A malformed token has to be
 * visible as malformed, because a cell that quietly ignores what you typed is
 * indistinguishable from one that accepted it.
 *
 *   C-4      absolute note
 *   c#3      absolute note, loose case and no dash
 *   24-4     degree 24 at octave 4  (degree notes, per AGENTS.md)
 *   ---      note off
 *   64       a bare number in a non-note column: raw value (velocity, fx arg)
 */
export function parseToken(text, column) {
  const s = text.trim().toLowerCase();
  if (!s) return { kind: 'empty' };
  if (s === '---' || s === 'off') return { kind: 'off' };

  // Note columns take pitch-shaped tokens; other columns take raw values.
  if (column === 0) {
    const m = /^([a-g])([#b]?)-?(-?\d)$/.exec(s);
    if (m) {
      let semis = NOTE_LETTERS[m[1]];
      if (m[2] === '#') semis += 1;
      if (m[2] === 'b') semis -= 1;
      const pitch = 12 * (parseInt(m[3], 10) + 1) + semis;
      return pitch >= 0 && pitch <= 127 ? { kind: 'note', pitch } : { kind: 'invalid' };
    }
    // Degree form: <degree>-<octave>, e.g. 24-4
    const d = /^(\d+)-(-?\d)$/.exec(s);
    if (d) return { kind: 'degree', degree: parseInt(d[1], 10), octave: parseInt(d[2], 10) };
    return { kind: 'invalid' };
  }

  if (/^\d+$/.test(s)) return { kind: 'value', value: parseInt(s, 10) };
  if (/^[0-9a-f]{1,2}$/.test(s)) return { kind: 'value', value: parseInt(s, 16) };
  return { kind: 'invalid' };
}

/** A transient buffer over the cell the cursor is on. */
export function createEntry() {
  return { active: false, row: -1, track: -1, col: -1, text: '' };
}

/** True if this cell is the one being typed into. */
export function editing(entry, row, track, col) {
  return entry.active && entry.row === row && entry.track === track && entry.col === col;
}

export function begin(entry, cursor) {
  entry.active = true;
  entry.row = cursor.row; entry.track = cursor.track; entry.col = cursor.col;
  entry.text = '';
}

export function cancel(entry) { entry.active = false; entry.text = ''; }

/**
 * Feed a keystroke. Returns what the caller should do:
 *   'consumed' — the buffer changed, redraw
 *   'commit'   — the token is complete; read entry.text and act
 *   'ignore'   — not for us
 */
export function feed(entry, key, cursor) {
  if (key === 'Escape') { cancel(entry); return 'consumed'; }
  if (key === 'Enter') return entry.active ? 'commit' : 'ignore';
  if (key === 'Backspace') {
    if (!entry.active || !entry.text) return 'ignore';
    entry.text = entry.text.slice(0, -1);
    return 'consumed';
  }
  if (key.length !== 1) return 'ignore';
  if (!/[0-9a-zA-Z#\-.]/.test(key)) return 'ignore';
  if (!entry.active) begin(entry, cursor);
  if (entry.text.length >= 8) return 'consumed';
  entry.text += key;
  return 'consumed';
}
