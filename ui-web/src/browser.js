// Browser rail: the design's browser, carrying the one category that is real.
//
// A rail rather than a view — it sits beside whichever surface is showing,
// because opening a project is something you do FROM somewhere, not instead of
// it.
//
// The project list does not come from the engine. The engine publishes no index
// and a browser cannot read a filesystem, so the sidecar answers `{"type":"list"}`
// with names only. Names, not paths: handing the client a path invites it to send
// one back, and the engine resolves names against its own project directory.
//
// WHAT IS REAL AND WHAT IS NOT.
// The design (design/redesign/Uni.dc.html) draws eight categories — plugins,
// presets, samples, clips, patches, tunings, favourites — and rows carrying a
// vendor, a format and a parameter count. Exactly one of those things crosses
// the wire today: the names of the projects on disk. The rest is not "empty",
// it is UNPUBLISHED, and the two must not look the same (GUIDELINES 4.5). So
// the structure is built once, in full, and every category with no feed behind
// it is drawn as unavailable and refuses the pointer, with the reason on the
// chip and in the footer. When a feed lands, `live` flips and the chip works;
// nothing else here has to move.
//
// The same rule decides the secondary line. "u-he · VST3 · 412 params" has no
// counterpart for a project, so the line says only what the sidecar actually
// established: that the file is a `.uniproj`, which one is loaded, and which is
// the most recently written (its list is ordered newest first). Inventing a
// track count nobody published would be the silent-plausible-wrongness of
// GUIDELINES 2.1 wearing a nicer font.
//
// Rendering discipline is chain.js's (GUIDELINES 3): pooled rows hidden rather
// than removed, every write guarded by a cached value, `.nodeValue` rather than
// `.textContent`, and the filter recomputed only when its inputs change — never
// once per draw, because render() runs with the rest of the app.

import { createField, begin as fieldBegin, cancel as fieldCancel,
         feed as fieldFeed, display } from './textfield.js';

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

function text(parent) {
  parent.appendChild(document.createTextNode(''));
  return parent.firstChild;
}

/**
 * The categories the rail offers, in the design's order.
 *
 * `live` is the whole point of this table: it says whether anything on the wire
 * can fill the category. Only `all` and `proj` are, and today they resolve to
 * the same rows — which will stop being true the moment a second feed lands,
 * and is not a reason to hide the distinction now.
 *
 * `badge` is the row's type mark. Non-live categories have one so the table
 * reads as a plan rather than a hole, but no row ever wears it.
 */
const KINDS = [
  { key: 'all',  label: 'ALL',      badge: '',     live: true },
  { key: 'proj', label: 'PROJECTS', badge: 'PROJ', live: true },
  { key: 'plug', label: 'PLUGINS',  badge: 'PLUG', live: false },
  { key: 'pset', label: 'PRESETS',  badge: 'PSET', live: false },
  { key: 'smpl', label: 'SAMPLES',  badge: 'SMPL', live: false },
  { key: 'clip', label: 'CLIPS',    badge: 'CLIP', live: false },
  { key: 'ptch', label: 'PATCHES',  badge: 'PTCH', live: false },
  { key: 'scal', label: 'TUNINGS',  badge: 'SCAL', live: false },
  { key: 'fav',  label: '★ FAVES', badge: '', live: false },
];

// Every string the draw path can write, interned. A template literal per row per
// frame is the regression alloc.mjs exists to catch; three constants are not.
const META_LOADED = 'uniproj · loaded';
const META_RECENT = 'uniproj · most recent';
const META_PLAIN = 'uniproj';
const MARK_LOADED = '▸';
const MARK_NONE = '';
const BADGE_PROJECT = 'PROJ';

const HINT_SAVING = 'Enter saves · Esc cancels';
const HINT_FINDING = 'typing filters · ↑↓ ⏎ still move and open · Esc clears';
const HINT_LIST = 'Enter opens · F finds · S saves as · B closes';

const EMPTY_NONE = 'no projects on disk';
const EMPTY_FILTERED = 'no project matches';

// The footer's standing caveat. Named categories rather than a count, because
// "seven kinds" is a number you have to go and check and this is a list you can
// read against the chips directly above it.
const FOOT_NOTE = 'no plugins, presets, samples, clips, patches or tunings cross '
                + 'the wire yet — those chips are unavailable, not empty';

export class Browser {
  /**
   * @param {{onOpen?:function, onSave?:function, onRescan?:function,
   *          onClose?:function}} opts
   *
   * `onRescan` and `onClose` are optional and the affordances for them appear
   * ONLY when they are wired. A ✕ that does not close and a `rescan` that does
   * not rescan are controls indistinguishable from ones that worked, which is
   * the failure GUIDELINES 4.5 names; a host that has nothing to hand them gets
   * a label instead of a button.
   */
  constructor(host, { onOpen, onSave, onRescan, onClose } = {}) {
    this.host = host;
    this.host.className = 'br';
    this.onOpen = onOpen; this.onSave = onSave;
    this.onRescan = onRescan; this.onClose = onClose;

    const head = div('br-head', host);
    const title = div('br-title', head);
    title.appendChild(document.createTextNode('BROWSER'));
    this.closeEl = div('br-close', head);
    this.closeText = text(this.closeEl);
    // The key is true whether or not anyone wired a handler; the ✕ is not.
    this.closeText.nodeValue = onClose ? '⌘B ✕' : '⌘B';
    if (onClose) {
      this.closeEl.classList.add('on');
      this.closeEl.addEventListener('pointerdown', () => this.onClose());
    }

    // Search.
    //
    // A real <input>, but NOT one the platform types into. index.html registers
    // its keydown handler on `window` with `{capture: true}`, so it runs before
    // any element listener and calls preventDefault on everything it claims —
    // which is every key, once the rail has focus. stopPropagation on a field
    // listener cannot help: capture on an ancestor happens first. Measured, not
    // assumed: with the app loaded, typing into this field, into the dock's
    // field, and into a fresh <input> the app has never heard of all leave the
    // value empty.
    //
    // So the field is a DISPLAY of the query and the keys arrive the way every
    // other rail key does — through the host's one hook into this rail, which is
    // feedSave(). Focusing it (by pointer or by `f`) is what puts the rail in
    // find mode; `searching` mirrors DOM focus so the pointer path and the key
    // path cannot disagree about which one is on (GUIDELINES 2.15).
    this.searchRow = div('br-search', host);
    const glass = document.createElement('i');
    glass.className = 'ph ph-magnifying-glass br-glass';
    this.searchRow.appendChild(glass);
    this.input = document.createElement('input');
    this.input.className = 'br-input';
    this.input.spellcheck = false;
    this.input.autocomplete = 'off';
    // Not the design's "search everything…". It searches what is published, and
    // promising everything is the promise this rail is built to avoid making.
    this.input.placeholder = 'search projects…';
    this.searchRow.appendChild(this.input);

    this.chipsEl = div('br-chips', host);
    this.chips = [];
    for (let i = 0; i < KINDS.length; i++) {
      const k = KINDS[i];
      const el = document.createElement('button');
      el.className = 'br-chip';
      el.type = 'button';
      el.dataset.cat = k.key;
      el.appendChild(document.createTextNode(k.label));
      if (!k.live) {
        // Disabled at the platform level, not merely painted grey: an inert
        // control cannot be clicked by a person, a test or an agent, so none of
        // the three can mistake "nothing published" for "nothing found".
        el.disabled = true;
        el.classList.add('off');
        el.title = k.label + ' are not published by the engine yet';
      }
      this.chipsEl.appendChild(el);
      this.chips.push(el);
    }
    this._cat = null;

    this.listEl = div('br-list', host);
    this.emptyEl = div('br-empty', host);
    this.emptyText = text(this.emptyEl);
    this._empty = null;

    // Save-as. An inline field rather than a dialog: the rail already owns the
    // keyboard while it is open, so a second focus owner would be one too many.
    this.saveRow = div('br-save', host);
    this.savePrompt = div('br-saveprompt', this.saveRow);
    this.savePrompt.appendChild(document.createTextNode('save as'));
    this.saveName = div('br-savename', this.saveRow);
    this.saveNameText = text(this.saveName);
    // Names go to the filesystem, so the charset matches what the sidecar
    // accepts — a name cannot be typed here and refused two hops away.
    this.field = createField({ charset: /[A-Za-z0-9._-]/, max: 28 });

    const foot = div('br-foot', host);
    const scan = div('br-scan', foot);
    this.statText = text(div('br-stat', scan));
    this.rescanEl = div('br-rescan', scan);
    this.rescanEl.appendChild(document.createTextNode(onRescan ? 'rescan' : ''));
    if (onRescan) {
      this.rescanEl.classList.add('on');
      this.rescanEl.addEventListener('pointerdown', () => this.onRescan());
    }
    const note = div('br-note', foot);
    note.appendChild(document.createTextNode(FOOT_NOTE));
    this.hint = div('br-hint', foot);
    this.hintText = text(this.hint);

    this.pool = [];
    this.items = [];          // every project the sidecar listed, in its order
    this.lower = [];          // ...lowercased once, for the filter
    this.view = [];           // indices into items, after category and search
    this.selected = 0;        // indexes view, not items
    this.current = '';
    this.query = '';
    this.queryLower = '';
    this.category = 'all';
    this.searching = false;   // mirrors the input's DOM focus; see the listeners

    this.listEl.addEventListener('pointerdown', (e) => {
      const row = e.target.closest('.br-item');
      if (!row) return;
      const at = Number(row.dataset.index);
      if (at >= this.view.length) return;
      this.selected = at;
      this.onOpen && this.onOpen(this.items[this.view[at]]);
    });

    this.chipsEl.addEventListener('pointerdown', (e) => {
      const chip = e.target.closest('.br-chip');
      // A disabled button swallows the event on its own; this is the second
      // guard, for the case where something dispatches straight at it.
      if (!chip || chip.disabled) return;
      this.setCategory(chip.dataset.cat);
    });

    // Still wired, and deliberately: paste, drag-and-drop text and an IME commit
    // are insertions the app's handler never sees. When the handler DID claim
    // the key there is no native insertion, so this does not fire and cannot
    // double-type. There is no keydown listener here on purpose — one would run
    // after the host's capture handler and act on the same key twice.
    this.input.addEventListener('input', () => this.setQuery(this.input.value));
    // `searching` IS focus. Deriving it instead of tracking it separately is
    // what stops "the field looks active but the keys go elsewhere".
    // The class is written here as well as in render() because a pointer focus
    // schedules no frame in the host — the mode has to be visible on the click,
    // not on whatever draw happens next.
    this.input.addEventListener('focus', () => { this.searching = true; this._syncSearch(); });
    this.input.addEventListener('blur', () => { this.searching = false; this._syncSearch(); });
  }

  /** Begin save-as, seeded with the loaded project's name. */
  beginSave(seed) { fieldBegin(this.field, seed || ''); }
  cancelSave() { fieldCancel(this.field); }

  /**
   * THE RAIL'S KEYBOARD, all of it. Returns 'consumed' | 'commit' | 'cancel' |
   * 'ignore' — the four words textfield.js returns, so callers dispatch alike.
   *
   * The name is narrower than the job and that is deliberate. index.html's key
   * route has exactly ONE hook into this rail (`const act = browser.feedSave(k)`
   * — everything it returns 'ignore' for the host then handles itself), so the
   * rail's second field has to arrive through the same door. Renaming the door
   * would need a change in index.html; widening it does not, and a second hook
   * the host does not call would be a field nobody could type into. `feedKey` is
   * the name it should have, for anything written after this.
   *
   * What it claims, in order:
   *
   *   save-as open   everything, exactly as before
   *   f / F          opens find, which is why a query cannot begin with one
   *   finding        every printable character, Backspace, and Escape
   *   otherwise      nothing — Escape, the arrows, Enter and S stay the host's
   *
   * Note what find mode buys beyond typing: while it is on, `b` types a `b`
   * instead of closing the rail out from under you. That is the same bug the
   * host's own comment at 2.05 describes, one field further down.
   */
  feedSave(key) {
    if (this.field.active) {
      const act = fieldFeed(this.field, key);
      if (act === 'commit') {
        const name = this.field.text.trim();
        fieldCancel(this.field);
        if (name) this.onSave && this.onSave(name);
      }
      return act;
    }
    if (!this.searching) {
      if (key === 'f' || key === 'F') { this.focusSearch(); return 'consumed'; }
      return 'ignore';
    }
    // Escape leaves find mode AND clears, so the list you are looking at matches
    // the field you can no longer see a caret in. A second Escape is the host's,
    // which hands the keyboard back to the centre.
    if (key === 'Escape') { this.setQuery(''); this.blurSearch(); return 'cancel'; }
    // The arrows and Enter stay the host's even while finding: narrowing and
    // then picking is one gesture, and a search field that swallowed ↑↓⏎ would
    // make you leave it to use the result.
    return this.feedSearch(key);
  }

  /** The name feedSave should have had. Same method; see the note there. */
  feedKey(key) { return this.feedSave(key); }

  /**
   * Feed a keystroke to the SEARCH field alone. Split out from the router so an
   * agent can drive the filter without also driving the mode.
   */
  feedSearch(key) {
    if (key === 'Escape') { this.setQuery(''); return 'cancel'; }
    if (key === 'Backspace') { this.setQuery(this.query.slice(0, -1)); return 'consumed'; }
    if (key.length === 1 && key >= ' ' && key !== '\x7f') {
      this.setQuery(this.query + key);
      return 'consumed';
    }
    return 'ignore';
  }

  focusSearch() { this.input.focus(); }
  blurSearch() { this.input.blur(); }

  setQuery(q) {
    const next = q || '';
    if (next === this.query) return;
    this.query = next;
    this.queryLower = next.toLowerCase();
    if (this.input.value !== next) this.input.value = next;
    // Typing re-aims at the top row. Keeping the index would leave the highlight
    // on whatever now happens to sit there, which is a different project than
    // the one you were looking at.
    this.selected = 0;
    this._refilter();
  }

  /**
   * Choose a category. Returns false for one nothing publishes — a rail that
   * accepted `plug` and then showed an empty list would be claiming there are no
   * plugins, which is a different and false statement.
   */
  setCategory(key) {
    const k = KINDS.find((x) => x.key === key);
    if (!k || !k.live) return false;
    if (this.category === key) return true;
    this.category = key;
    this.selected = 0;
    this._refilter();
    return true;
  }

  /** Whether a chip has a feed behind it. */
  available(key) {
    const k = KINDS.find((x) => x.key === key);
    return !!(k && k.live);
  }

  /**
   * The names the sidecar listed, and which one is loaded.
   *
   * Called once per draw with the same array, so it compares the CONTENT before
   * doing anything: there is no version for this list, and a reference check
   * alone would miss a rename that kept the array (GUIDELINES 2.1). Everything
   * downstream — the lowercased copies, the filter — is derived here and nowhere
   * else, so the draw path never recomputes it.
   */
  setItems(names, current) {
    const cur = current || '';
    const moved = !this._sameItems(names);
    if (moved) this._adopt(names);
    if (this.current !== cur) this.current = cur;
    if (moved) this._refilter();
    if (this.selected >= this.view.length) this.selected = Math.max(0, this.view.length - 1);
  }

  _sameItems(names) {
    const n = names ? names.length : 0;
    if (n !== this.items.length) return false;
    for (let i = 0; i < n; i++) if (this.items[i] !== names[i]) return false;
    return true;
  }

  _adopt(names) {
    const n = names ? names.length : 0;
    this.items.length = n;
    this.lower.length = n;
    for (let i = 0; i < n; i++) {
      this.items[i] = names[i];
      this.lower[i] = names[i].toLowerCase();
    }
  }

  /**
   * Rebuild the visible set. Holds SOURCE indices, not names: the row's meta
   * line depends on where a project sits in the sidecar's newest-first order,
   * and a filtered position cannot answer that.
   */
  _refilter() {
    const v = this.view;
    v.length = 0;
    if (this.category === 'all' || this.category === 'proj') {
      const q = this.queryLower;
      for (let i = 0; i < this.items.length; i++) {
        if (!q || this.lower[i].indexOf(q) >= 0) v.push(i);
      }
    }
    if (this.selected >= v.length) this.selected = Math.max(0, v.length - 1);
  }

  move(delta) {
    if (!this.view.length) return;
    this.selected = Math.max(0, Math.min(this.view.length - 1, this.selected + delta));
  }

  openSelected() {
    if (!this.view.length) return null;
    const name = this.items[this.view[this.selected]];
    this.onOpen && this.onOpen(name);
    return name;
  }

  /** The name a visible row holds, or null. Row indices are view indices. */
  nameAt(at) {
    return at >= 0 && at < this.view.length ? this.items[this.view[at]] : null;
  }

  render() {
    const n = this.view.length;
    while (this.pool.length < n) {
      const el = div('br-item', this.listEl);
      // Written once at construction: the slot a row occupies never changes, so
      // it has no business being reassigned on every draw.
      el.dataset.index = String(this.pool.length);
      el._badge = text(div('br-kind', el));
      const main = div('br-main', el);
      el._name = text(div('br-name', main));
      el._meta = text(div('br-meta', main));
      el._mark = text(div('br-mark', el));
      el._n = null; el._m = null; el._k = null; el._mk = null;
      el._sel = null; el._cur = null;
      this.pool.push(el);
    }
    for (let i = 0; i < this.pool.length; i++) {
      const el = this.pool[i];
      const on = i < n;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      // Cleared even while hidden. A pooled row keeps whatever class it last
      // wore, so a filter that hides the selected row would leave a second
      // `.br-item.sel` in the document for anything counting them to find.
      if (!on) {
        if (el._sel !== false) { el._sel = false; el.classList.remove('sel'); }
        if (el._cur !== false) { el._cur = false; el.classList.remove('cur'); }
        // And it stops advertising a project. `[data-name="…"]` is how a test or
        // an agent finds a row; a hidden one answering to it is a row nobody can
        // see reporting that it is there.
        if (el._n !== null) { el._n = null; el.removeAttribute('data-name'); }
        continue;
      }
      const src = this.view[i];
      const name = this.items[src];
      if (el._n !== name) {
        el._n = name;
        el._name.nodeValue = name;
        // For the agent loop: assert on the project a row holds, not its slot.
        el.dataset.name = name;
      }
      if (el._k !== BADGE_PROJECT) { el._k = BADGE_PROJECT; el._badge.nodeValue = BADGE_PROJECT; }
      const cur = name === this.current;
      // Newest first is the sidecar's ordering, so source 0 is the last one
      // written. It is the only other thing about a project this side knows.
      const meta = cur ? META_LOADED : (src === 0 ? META_RECENT : META_PLAIN);
      if (el._m !== meta) { el._m = meta; el._meta.nodeValue = meta; }
      const mark = cur ? MARK_LOADED : MARK_NONE;
      if (el._mk !== mark) { el._mk = mark; el._mark.nodeValue = mark; }
      const sel = i === this.selected;
      if (el._sel !== sel) { el._sel = sel; el.classList.toggle('sel', sel); }
      if (el._cur !== cur) { el._cur = cur; el.classList.toggle('cur', cur); }
    }

    if (this._cat !== this.category) {
      this._cat = this.category;
      for (let i = 0; i < this.chips.length; i++) {
        this.chips[i].classList.toggle('on', KINDS[i].key === this.category);
      }
    }

    const total = this.items.length;
    if (this._n !== total || this._shown !== n) {
      this._n = total; this._shown = n;
      // Two shapes, because a count that ignores the filter is a count you have
      // to reconcile against the rows yourself.
      this.statText.nodeValue = n === total
        ? total + (total === 1 ? ' project' : ' projects')
        : n + ' of ' + total + ' projects';
      const empty = total === 0 ? EMPTY_NONE : EMPTY_FILTERED;
      if (this._empty !== empty) { this._empty = empty; this.emptyText.nodeValue = empty; }
      this.emptyEl.style.display = n ? 'none' : '';
    }

    if (this._saving !== this.field.active) {
      this._saving = this.field.active;
      this.saveRow.classList.toggle('on', this.field.active);
    }
    // `display()` concatenates a caret onto the text, so calling it every draw
    // allocates a string every draw for a field that is almost always closed.
    // Compare the field's own state; build the string only when it moved.
    if (this._saveRaw !== this.field.text) {
      this._saveRaw = this.field.text;
      this._saveText = display(this.field);
      this.saveNameText.nodeValue = this._saveText;
    }
    this._syncSearch();
    const hint = this.field.active ? HINT_SAVING : (this.searching ? HINT_FINDING : HINT_LIST);
    if (this._hint !== hint) { this._hint = hint; this.hintText.nodeValue = hint; }
  }

  _syncSearch() {
    if (this._find === this.searching) return;
    this._find = this.searching;
    this.searchRow.classList.toggle('on', this.searching);
  }

  probe() {
    const chips = [];
    for (let i = 0; i < KINDS.length; i++) {
      chips.push({ key: KINDS[i].key, label: KINDS[i].label,
                   available: KINDS[i].live, selected: KINDS[i].key === this.category });
    }
    const shown = [];
    for (let i = 0; i < this.view.length; i++) shown.push(this.items[this.view[i]]);
    return { items: this.items.slice(), selected: this.selected,
             current: this.current, saving: this.field.active, saveText: this.field.text,
             query: this.query, searching: this.searching,
             category: this.category, shown,
             selectedName: this.nameAt(this.selected), chips,
             unavailable: chips.filter((c) => !c.available).map((c) => c.key),
             note: FOOT_NOTE,
             domNodes: this.pool.length };
  }
}
