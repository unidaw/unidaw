// Command palette (⌘K): the grammar the dock already speaks, reached by name.
//
// The dock is a console — arguments, history, a log you read back. That shape is
// wrong for "I know roughly what it is called, run it": you have to remember the
// exact word, spelled exactly, before anything on screen helps you. The palette
// is the other half. It shows the grammar while you narrow it, and gets out of
// the way.
//
// It does NOT own a command list. `createCommands` comes from the dock, so a
// command added to the grammar turns up here without anyone remembering to add
// it, and the palette can never offer something the console cannot run. Two
// descriptions of one thing with nothing forcing them to agree is the bug this
// project keeps having (GUIDELINES 2.1); a second list would be another one.
//
// It is a LAUNCHER, not a second console. It runs one command and closes;
// anything conversational — reading `help` back, scrolling a session — is the
// dock's job and is one keystroke away.
//
// The palette listens on its own input and its own list, and nowhere else. ⌘K
// belongs to the app's keydown handler, like every other app-level key; a
// surface that installs a window listener is a surface that fights the handler
// for the same keys and wins at random.

import { createCommands } from './dock.js';

/**
 * Ranking, written down because a search that ranks mysteriously is worse than
 * one that ranks plainly. A candidate lands in the first tier it satisfies:
 *
 *   0  the name IS the query                      `note` → note
 *   1  the name starts with it                    `tr`   → transpose
 *   2  the name contains it                       `ode`  → addnode
 *   3  the name contains its letters, in order    `trn`  → transpose
 *   4  only the help text does
 *
 * Within a tier: the tightest run of letters first (last matched − first), then
 * the earliest first letter, then the shorter name, then alphabetically. With an
 * empty query nothing is ranked at all — the list is the registry's own order,
 * which groups related commands the way whoever wrote them meant.
 */
const TIER_EXACT = 0, TIER_PREFIX = 1, TIER_SUBSTR = 2, TIER_FUZZY = 3, TIER_HELP = 4;

// Scratch for the scan's two outputs. Returning `{at, end}` would allocate once
// per candidate per keystroke; one shared record does not.
const HIT = { at: 0, end: 0 };

/**
 * Every letter of `q` in `s`, in order but not necessarily adjacent. Compares
 * char codes rather than `q[k]`, which allocates a one-character string per
 * candidate per letter for nothing.
 */
function subsequence(s, q) {
  let i = 0, first = -1;
  for (let k = 0; k < q.length; k++) {
    const c = q.charCodeAt(k);
    while (i < s.length && s.charCodeAt(i) !== c) i++;
    if (i >= s.length) return false;
    if (first < 0) first = i;
    i++;
  }
  HIT.at = first;
  HIT.end = i - 1;
  return true;
}

/** Fills the entry's rank fields. False means it does not match at all. */
function rank(e, q) {
  if (e.lower === q) { e.tier = TIER_EXACT; e.at = 0; e.span = q.length; return true; }
  if (e.lower.startsWith(q)) { e.tier = TIER_PREFIX; e.at = 0; e.span = q.length; return true; }
  const at = e.lower.indexOf(q);
  if (at >= 0) { e.tier = TIER_SUBSTR; e.at = at; e.span = q.length; return true; }
  if (subsequence(e.lower, q)) {
    e.tier = TIER_FUZZY; e.at = HIT.at; e.span = HIT.end - HIT.at + 1; return true;
  }
  if (subsequence(e.helpLower, q)) {
    // Help text is long, so its span is meaningless as a tiebreak — everything in
    // this tier sorts by name instead, which at least does not shuffle.
    e.tier = TIER_HELP; e.at = HIT.at; e.span = 0; return true;
  }
  return false;
}

function byRank(a, b) {
  if (a.tier !== b.tier) return a.tier - b.tier;
  if (a.span !== b.span) return a.span - b.span;
  if (a.at !== b.at) return a.at - b.at;
  if (a.name.length !== b.name.length) return a.name.length - b.name.length;
  return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
}

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

const NO_ARGS = [];

export class Palette {
  constructor(host, api) {
    this.host = host;
    this.host.className = 'pl';
    this.api = api;
    this.commands = createCommands(api);

    // Built once. The registry is fixed for the life of the page, so the only
    // per-keystroke work is deciding which of these are in and where they sit.
    // `lower` is precomputed for the same reason: lowercasing 30 names on every
    // keystroke is 30 strings nobody reads.
    this.entries = [];
    for (const name of Object.keys(this.commands)) {
      const help = this.commands[name].help || '';
      this.entries.push({ name, help, lower: name.toLowerCase(),
                          helpLower: help.toLowerCase(), tier: 0, span: 0, at: 0 });
    }

    this.card = div('pl-card', host);
    const head = div('pl-head', this.card);
    const mark = div('pl-mark', head);
    mark.appendChild(document.createTextNode('›'));
    this.input = document.createElement('input');
    this.input.className = 'pl-input';
    this.input.spellcheck = false;
    this.input.autocomplete = 'off';
    this.input.placeholder = 'transpose · loop · addnode …';
    head.appendChild(this.input);
    this.countEl = div('pl-count', head);
    this.countEl.appendChild(document.createTextNode(''));

    this.listEl = div('pl-list', this.card);
    this.emptyEl = div('pl-empty', this.card);
    this.emptyEl.appendChild(document.createTextNode('no command matches'));
    this.statusEl = div('pl-status', this.card);
    this.statusEl.appendChild(document.createTextNode(''));
    const foot = div('pl-foot', this.card);
    for (const hint of ['↑↓ move', '⏎ run', 'esc close',
                        'a space starts the arguments']) {
      const s = div('pl-hint', foot);
      s.appendChild(document.createTextNode(hint));
    }

    this.pool = [];
    this.matches = [];        // reused; refilter empties and refills it in place
    this.opened = false;
    this.selected = 0;
    this.query = '';
    this.token = '';          // the first word: what the list filters on
    this.argText = '';        // everything after it: what the command receives
    this.status = '';
    this.result = '';
    this._dirty = true;
    this._rowH = 0;

    this.input.addEventListener('keydown', (e) => {
      // Stops here rather than bubbling to the app, for the reason the dock stops
      // it: a field that also plays notes and switches views while you type into
      // it is unusable. The consequence is that nothing outside can see these
      // keys, so everything the palette answers to has to be answered here.
      e.stopPropagation();
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
        // ⌘K is the app's key, but the app can no longer hear it — without this
        // the shortcut that opens the palette would be a one-way door.
        e.preventDefault();
        this.close();
        this.render();
        return;
      }
      // A printable character is the input's own business: it edits the text and
      // the `input` event below syncs the query. Only the keys that mean
      // something to the LIST go through feed(), which is also how the app and an
      // agent drive it, so there is one implementation of each of them.
      if (e.key.length === 1 && !e.metaKey && !e.ctrlKey) return;
      if (this.feed(e.key) !== 'ignore') e.preventDefault();
      this.render();
    });
    this.input.addEventListener('input', () => {
      this.setQuery(this.input.value);
      // Nothing else will draw this: the app schedules a frame from its own
      // keydown handler, and these keys never reach it. render() is idempotent,
      // so the app's call later in the same frame costs one comparison.
      this.render();
    });

    this.listEl.addEventListener('pointerdown', (e) => {
      const row = e.target.closest('.pl-item');
      if (!row) return;
      this.selected = Number(row.dataset.index);
      this._dirty = true;
      this.run();
      this.render();
    });
    this.host.addEventListener('pointerdown', (e) => {
      // Only the scrim itself — the card is a child, so a click inside it names
      // the card as its target and never gets here.
      if (e.target !== this.host) return;
      this.close();
      this.render();
    });
  }

  /** Open, optionally seeded with a query. The app owns `host.hidden`. */
  open(seed = '') {
    this.opened = true;
    this.applyQuery(seed || '');
  }

  close() {
    this.opened = false;
    this.applyQuery('');
  }

  toggle() { if (this.opened) this.close(); else this.open(); }

  focus() { this.input.focus(); }

  /**
   * Feed a keystroke. Returns 'consumed' | 'commit' | 'cancel' | 'ignore' — the
   * same four words `textfield.js` returns, because a second vocabulary for the
   * same dispatch is one more thing to remember for no gain.
   *
   * 'commit' means a command ran and the palette closed; a command that REFUSED
   * returns 'consumed', because the palette is still up showing you why.
   */
  feed(key) {
    if (!this.opened) return 'ignore';
    if (key === 'Escape') { this.close(); return 'cancel'; }
    // Whether it ran is read off `opened`, not off the return value: a command
    // that succeeds with nothing to say (`clear`) returns an empty string, and
    // that must not report as a refusal.
    if (key === 'Enter') { this.run(); return this.opened ? 'consumed' : 'commit'; }
    if (key === 'ArrowDown') { this.move(1); return 'consumed'; }
    if (key === 'ArrowUp') { this.move(-1); return 'consumed'; }
    if (key === 'Backspace') { this.setQuery(this.query.slice(0, -1)); return 'consumed'; }
    // Printable characters only. `key.length === 1` alone is true of the control
    // characters too — a stray \b or \x1b would be typed INTO the query and then
    // match nothing, which reads as a filter that has broken rather than a key
    // that was not for us.
    if (key.length === 1 && key >= ' ' && key !== '\x7f') {
      this.setQuery(this.query + key);
      return 'consumed';
    }
    return 'ignore';
  }

  move(delta) {
    if (!this.matches.length) return;
    const to = Math.max(0, Math.min(this.matches.length - 1, this.selected + delta));
    if (to === this.selected) return;
    this.selected = to;
    this._dirty = true;
  }

  setQuery(q) {
    if (q === this.query) return;
    this.applyQuery(q);
  }

  applyQuery(q) {
    this.query = q;
    if (this.input.value !== q) this.input.value = q;
    // The query is a command line, split the way the dock splits one: the first
    // word chooses the command, the rest are its arguments. Without this the
    // palette could only reach the commands that take none, and `transpose` —
    // which refuses without a number — would be permanently unrunnable from here.
    const sp = q.indexOf(' ');
    this.token = (sp < 0 ? q : q.slice(0, sp)).toLowerCase();
    this.argText = sp < 0 ? '' : q.slice(sp + 1);
    // Typing re-aims at the top match. Keeping the index would leave the
    // highlight on whatever now happens to sit there, which is a different
    // command than the one you were looking at.
    this.selected = 0;
    this.status = '';
    // The list underneath the scroll position is a different list now, so the
    // next draw re-aligns it rather than leaving you looking at row 12 of a
    // three-row result.
    this._scrollAt = -1;
    this.refilter();
    this._dirty = true;
  }

  refilter() {
    const m = this.matches;
    m.length = 0;
    const q = this.token;
    for (let i = 0; i < this.entries.length; i++) {
      const e = this.entries[i];
      if (!q) { m.push(e); continue; }
      if (rank(e, q)) m.push(e);
    }
    if (q) m.sort(byRank);
  }

  /** The selected command's arguments, from the query's tail. */
  args() {
    const t = this.argText.trim();
    return t ? t.split(/\s+/) : NO_ARGS;
  }

  /**
   * Run the selection. Returns the command's output on success — '' when it had
   * none — and null when it refused.
   *
   * A refusal keeps the palette OPEN and shows the message: the grammar's errors
   * are written to be read — "transpose by how much?" is the answer to what you
   * just did — and a launcher that closes on one has thrown it away, which is the
   * silent-plausible-wrongness this codebase is about. Success closes; the output
   * goes to probe(), and to `api.log` when the host wired one, so an answer from
   * `help` or `nodes` lands somewhere a person can read it.
   */
  run() {
    const e = this.matches[this.selected];
    if (!e) { this.setStatus('no command matches'); return null; }
    const cmd = this.commands[e.name];
    try {
      const out = cmd.run(this.args(), this);
      this.result = out === null || out === undefined ? '' : String(out);
      this.close();
      if (this.result && this.api && typeof this.api.log === 'function') {
        this.api.log('out', this.result);
      }
      return this.result;
    } catch (err) {
      this.setStatus(String(err && err.message ? err.message : err));
      return null;
    }
  }

  /**
   * The `clear` command's hook — the registry hands each command its host, and
   * `help` reads `commands` off it. The dock's log is not ours to clear; ours is
   * the one line at the foot.
   */
  clear() { this.status = ''; this.result = ''; this._dirty = true; }

  setStatus(text) {
    // One line: the card sits over the surface and a stack trace's worth of text
    // would push the list off the screen.
    const s = String(text).split('\n')[0].slice(0, 120);
    if (s === this.status) return;
    this.status = s;
    this._dirty = true;
  }

  render() {
    // A closed palette draws nothing, and an unchanged one touches nothing: this
    // is called from the app's draw loop, so anything unguarded below it happens
    // sixty times a second for no reason (GUIDELINES 3).
    if (!this.opened || !this._dirty) return;
    this._dirty = false;

    const n = this.matches.length;
    while (this.pool.length < n) {
      const el = div('pl-item', this.listEl);
      // Written once, at construction: an index that never changes has no
      // business being reassigned on every draw.
      el.dataset.index = String(this.pool.length);
      const name = div('pl-name', el);
      name.appendChild(document.createTextNode(''));
      const help = div('pl-help', el);
      help.appendChild(document.createTextNode(''));
      el._nameText = name.firstChild;
      el._helpText = help.firstChild;
      el._name = null; el._help = null; el._sel = null;
      this.pool.push(el);
    }
    for (let i = 0; i < this.pool.length; i++) {
      const el = this.pool[i];
      const on = i < n;
      // Hidden, never removed: creating and destroying these as the filter
      // narrows is an allocation and a layout per keystroke, and the pool
      // high-water-marks at the size of the registry anyway.
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const e = this.matches[i];
      if (el._name !== e.name) {
        el._name = e.name;
        el._nameText.nodeValue = e.name;
        // For the agent loop: assert on the command a row holds, not on its slot.
        el.dataset.cmd = e.name;
      }
      if (el._help !== e.help) { el._help = e.help; el._helpText.nodeValue = e.help; }
      const sel = i === this.selected;
      if (el._sel !== sel) { el._sel = sel; el.classList.toggle('sel', sel); }
    }

    if (this._shown !== n || this._total !== this.entries.length) {
      this._shown = n;
      this._total = this.entries.length;
      this.countEl.firstChild.nodeValue = n + '/' + this.entries.length;
      this.emptyEl.style.display = n ? 'none' : '';
    }
    if (this._status !== this.status) {
      this._status = this.status;
      this.statusEl.firstChild.nodeValue = this.status;
      this.statusEl.style.display = this.status ? '' : 'none';
    }

    // Keep the selection in view by arithmetic rather than scrollIntoView, which
    // reads and writes layout on every call. The row height is a CSS fact that
    // does not change, so it is measured once (3.11) and only when there is a
    // laid-out row to measure; clientHeight is read only when the selection
    // actually moved.
    if (this._scrollAt !== this.selected && n) {
      this._scrollAt = this.selected;
      if (!this._rowH) this._rowH = this.pool[0].offsetHeight;
      const h = this._rowH;
      if (h) {
        const top = this.selected * h;
        const view = this.listEl.clientHeight;
        const at = this.listEl.scrollTop;
        if (top < at) this.listEl.scrollTop = top;
        else if (top + h > at + view) this.listEl.scrollTop = top + h - view;
      }
    }
  }

  probe() {
    const m = [];
    for (let i = 0; i < this.matches.length; i++) m.push(this.matches[i].name);
    const sel = this.matches[this.selected];
    return {
      open: this.opened,
      query: this.query,
      matches: m,
      selected: this.selected,
      selectedName: sel ? sel.name : null,
      total: this.entries.length,
      status: this.status,
      result: this.result,
      domNodes: this.pool.length,
    };
  }
}
