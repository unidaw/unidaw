// One transient text field, used by all three places that need one.
//
// There are three: the cell's chord/token buffer, the browser's save-as, and the
// mixer's track rename. They were written separately and drifted, and the drift
// was always the same bug — a seeded value that the first keystroke should
// replace but instead appended to. It produced "BassBig Bass" in the rename field
// and "foofoo" in save-as, and I fixed the first without going back to the
// second. Three copies of a thing is how the third one keeps the bug.
//
// The vocabulary is shared too: 'consumed', 'commit', 'cancel', 'ignore'. Callers
// dispatch on it identically, and a second vocabulary would be another thing to
// remember for no gain.

/**
 * @param {{charset?:RegExp, max?:number, commitEmpty?:boolean,
 *          cancelWhenEmpty?:boolean}} opts
 *
 * charset          which single characters are accepted (default: printable-ish)
 * max              length cap
 * commitEmpty      whether Enter on an empty field is a commit (a cell CLEARS on
 *                  empty; a name must not be empty)
 * cancelWhenEmpty  whether backspacing past the start closes the field, so the
 *                  next keystroke goes back to being a shortcut rather than
 *                  silently more text
 */
export function createField(opts = {}) {
  return {
    active: false,
    text: '',
    fresh: false,
    charset: opts.charset || /[0-9a-zA-Z ._#\-^~@/]/,
    max: opts.max || 32,
    commitEmpty: opts.commitEmpty === true,
    cancelWhenEmpty: opts.cancelWhenEmpty === true,
  };
}

/**
 * Open the field. A seed starts SELECTED — the first character replaces it —
 * because that is what every rename field in every application does, and the
 * alternative is the bug this module exists to stop.
 */
export function begin(f, seed = '') {
  f.active = true;
  f.text = seed;
  f.fresh = seed.length > 0;
}

export function cancel(f) { f.active = false; f.text = ''; f.fresh = false; }

/** Feed a keystroke. Returns 'consumed' | 'commit' | 'cancel' | 'ignore'. */
export function feed(f, key) {
  if (!f.active) return 'ignore';
  if (key === 'Escape') { cancel(f); return 'cancel'; }
  if (key === 'Enter') {
    if (!f.text.trim() && !f.commitEmpty) return 'consumed';
    return 'commit';
  }
  if (key === 'Backspace') {
    f.fresh = false;
    f.text = f.text.slice(0, -1);
    if (!f.text && f.cancelWhenEmpty) { cancel(f); return 'cancel'; }
    return 'consumed';
  }
  if (key.length !== 1) return 'ignore';
  if (!f.charset.test(key)) return 'consumed';
  if (f.fresh) { f.text = ''; f.fresh = false; }
  if (f.text.length < f.max) f.text += key;
  return 'consumed';
}

/** What to show: the text plus a caret, so an empty field is still visible. */
export function display(f) { return f.text + '█'; }
