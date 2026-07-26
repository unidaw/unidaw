// The keymap, as data, and the overlay that renders it.
//
// There are six surfaces now and nothing on screen said which one you were on or
// what it responded to. That is a real usability failure and also an agent one:
// requirement (d) is that the app be legible, and a keymap that exists only as
// branches in a handler is legible to neither.
//
// CAVEAT, stated rather than hidden: this table and the keydown handler are two
// descriptions of the same thing and nothing forces them to agree. The handler
// remains authoritative — if they disagree, the handler is right and this is a
// stale document. Keep them in the same commit. The agent-facing route that
// cannot drift is the dock's command grammar, which routes through the same
// functions the keys do.

export const GLOBAL_KEYS = [
  ['Tab', 'next surface (shift+Tab previous)'],
  ['/', 'agent dock'],
  ['B', 'browser rail'],
  ['?', 'this help'],
  ['Space', 'play / pause'],
  ['Cmd+Z', 'undo (shift to redo)'],
  ['Esc', 'cancel / close'],
];

export const SURFACE_KEYS = {
  tracker: {
    title: 'TRACKER',
    keys: [
      ['z s x d c…', 'lower octave piano row (z = C in the current octave)'],
      ['q 2 w 3 e…', 'upper octave piano row'],
      ['a', 'note off — truncates the note sounding here'],
      ['Backspace', 'delete the note at the cursor'],
      ['0-9 a-f', 'velocity digits in the volume column, commit on each key'],
      ['@', 'chord/degree token — the only thing Enter is for'],
      ['p', 'open the piano roll here (** = notes a cell cannot show apart)'],
      ['[ ]', 'octave down / up'],
      [', .', 'edit step down / up (0 stays put, for stacking a chord)'],
      ["; '", 'default velocity down / up'],
      ['- =', 'zoom out / in'],
      ['shift+up/down', 'extend the selection'],
      ['alt+c / alt+v', 'copy / paste (relative to the cursor)'],
      ['alt+x', 'cut'],
      ['alt+q / alt+a', 'transpose a semitone up / down'],
      ['alt+w / alt+s', 'transpose an octave up / down'],
      ['arrows', 'move cursor (drops the selection)'],
      ['PgUp/PgDn', 'scroll a page'],
    ],
  },
  arrange: {
    title: 'ARRANGE',
    keys: [
      ['click lane', 'seek'],
      ['click clip', 'select placement'],
      ['left/right', 'scroll time'],
      ['up/down', 'change track'],
      ['- =', 'zoom out / in'],
      ['Home', 'back to the start'],
      ['Backspace', 'clip edits — not implemented, needs engine commands'],
    ],
  },
  piano: {
    title: 'PIANO ROLL',
    keys: [
      ['click', 'write a note, snapped to the lane grid'],
      ['click note', 'select'],
      ['drag a note', 'move it (snapped to the lane grid)'],
      ['drag its right edge', 'change its length'],
      ['shift+drag', 'marquee-select notes'],
      ['alt+q / alt+a', 'transpose the selection a semitone'],
      ['alt+w / alt+s', 'transpose it an octave'],
      ['Backspace', 'delete the selected note'],
      ['left/right', 'scroll time'],
      ['up/down', 'shift the pitch window an octave'],
      ['f', 'fit the pitch window to the material'],
      ['a', 'all tracks / this track only'],
      ['[ ]', 'previous / next track'],
      ['s', 'seek to the window start'],
      ['- =', 'zoom out / in'],
    ],
  },
  patcher: {
    title: 'PATCHER',
    keys: [
      ['click node', 'select'],
      ['Backspace', 'node edits — not wired yet, this surface is read-only'],
      ['', 'one global graph; the engine does not run per-device graphs yet'],
    ],
  },
  mixer: {
    title: 'MIXER',
    keys: [
      ['drag fader', 'gain'],
      ['click pan', 'pan right (shift for left)'],
      ['M / S', 'mute / solo'],
      ['click name', 'rename the track (r renames the cursor track)'],
      ['', 'values come from the engine; edits settle on its next mixer version'],
    ],
  },
};

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

function section(parent, title, rows) {
  const sec = div('hp-sec', parent);
  const h = div('hp-h', sec);
  h.appendChild(document.createTextNode(title));
  for (const [k, what] of rows) {
    const row = div('hp-row', sec);
    const kd = div('hp-k', row);
    kd.appendChild(document.createTextNode(k));
    const vd = div('hp-v', row);
    vd.appendChild(document.createTextNode(what));
  }
  return sec;
}

/** Built once per surface and cached — the overlay is not in the draw path. */
export function createHelp(host) {
  host.className = 'hp';
  const inner = div('hp-inner', host);
  let shown = null;

  return {
    show(surface) {
      if (shown === surface) return;
      shown = surface;
      inner.textContent = '';
      const s = SURFACE_KEYS[surface] || SURFACE_KEYS.tracker;
      section(inner, s.title, s.keys);
      section(inner, 'ANYWHERE', GLOBAL_KEYS);
      const note = div('hp-note', inner);
      note.appendChild(document.createTextNode(
        'Every key here also exists as a dock command — press / and type help.'));
    },
    probe: () => ({ surface: shown, sections: inner.querySelectorAll('.hp-sec').length,
                    rows: inner.querySelectorAll('.hp-row').length }),
  };
}
