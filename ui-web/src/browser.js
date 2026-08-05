// Browser rail: the design's browser, carrying the categories that are real.
//
// A rail rather than a view — it sits beside whichever surface is showing,
// because opening a project is something you do FROM somewhere, not instead of
// it.
//
// Neither list comes from the engine. The engine publishes no index and a
// browser cannot read a filesystem, so the sidecar answers `{"type":"list"}`
// with project names and `{"type":"plugins"}` with the catalogue the engine's
// scanner already wrote to disk. Names, not paths, for projects: handing the
// client a path invites it to send one back, and the engine resolves names
// against its own project directory. Plugins are the other way round — a plugin
// IS its path, vendor, name and uid16 together, and that quadruple is the
// durable identity a saved project has to carry.
//
// WHAT IS REAL AND WHAT IS NOT.
// The design (design/redesign/Uni.dc.html) draws eight categories — plugins,
// presets, samples, clips, patches, tunings, favourites — and rows carrying a
// vendor, a format and a parameter count. TWO of those things cross the wire
// today: the names of the projects on disk, and the plugin catalogue. The rest
// is not "empty", it is UNPUBLISHED, and the two must not look the same
// (GUIDELINES 4.5). So the structure is built once, in full, and every category
// with no feed behind it is drawn as unavailable and refuses the pointer, with
// the reason on the chip and in the footer.
//
// PLUGINS is live only once the catalogue has actually ARRIVED. Three states,
// three different sentences: nobody has answered yet / the engine has not
// scanned / here are the plugins. A chip that accepted a click and then showed
// an empty list would be saying "you own no plugins", which is a different and
// false statement from "nobody has told me yet" — and the reason for each is
// written where it can be read without hovering.
//
// The same rule decides the secondary line. A project's says only what the
// sidecar actually established: that the file is a `.uniproj`, which one is
// loaded, and which is the most recently written (its list is ordered newest
// first). A plugin's says what the scanner established — vendor and format when
// it worked, and the scanner's own error when it did not. Failed entries are
// SHOWN, greyed, never filtered: a plugin you own and cannot see is worse than
// one you can see and cannot use, and "why is Zebra not in the list" is a
// question the list itself should answer.
//
// Rendering discipline is chain.js's (GUIDELINES 3): pooled rows hidden rather
// than removed, every write guarded by a cached value, `.nodeValue` rather than
// `.textContent`, and the filter recomputed only when its inputs change — never
// once per draw, because render() runs with the rest of the app. Every string a
// row can show is either interned below or built ONCE, when the feed it came
// from arrived. Nothing here builds a string per row per frame; test/alloc.mjs
// measures the rail with the catalogue loaded, which is 57 rows of two kinds.

import { createField, begin as fieldBegin, cancel as fieldCancel,
         feed as fieldFeed, display } from './textfield.js';
// The engine's device enum, imported rather than restated. chainmodel's own comment
// says why: a hand-maintained mirror of an engine enum drifts, and that one already
// had — the sampler landed as kind 5 and every sampler on screen read "kind 5 #9".
import { DEVICE_KINDS } from './chainmodel.js';

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
 * can fill the category. `feed` marks the ones whose liveness is not a constant
 * — `plug` is live exactly while a catalogue is in hand, which the table cannot
 * know and `available()` decides.
 *
 * `badge` is the row's type mark. Non-live categories have one so the table
 * reads as a plan rather than a hole, but no row ever wears it.
 */
const KINDS = [
  { key: 'all',  label: 'ALL',      badge: '',     live: true,  feed: '' },
  { key: 'proj', label: 'PROJECTS', badge: 'PROJ', live: true,  feed: '' },
  { key: 'plug', label: 'PLUGINS',  badge: 'PLUG', live: false, feed: 'plug' },
  /*
   * THE DEVICES THE ENGINE HAS BUILT IN, and the reason this category exists.
   *
   * Jaakko: "how do I add the built-in Sampler to a track's device chain?" The answer
   * was that you could not. Six device kinds exist; the rack's "+" was hard-coded to
   * `patcher event`, and this rail listed only projects and plugins — so five of the
   * six, the sampler among them, were reachable from the console and from nowhere on
   * screen. `dock.js` even carried a comment claiming the console and the "+" card
   * call the same function "so the console and the pointer cannot come to mean
   * different things by 'add a device'". They call it with different fixed kinds, so
   * they meant different things anyway.
   *
   * `live: true` with no feed: unlike plugins these are not scanned, they are the
   * engine's own enum, so the category is always available and never empty.
   */
  { key: 'devs', label: 'DEVICES',  badge: 'DEV',  live: true,  feed: '' },
  { key: 'pset', label: 'PRESETS',  badge: 'PSET', live: false, feed: '' },
  { key: 'smpl', label: 'SAMPLES',  badge: 'SMPL', live: false, feed: '' },
  { key: 'clip', label: 'CLIPS',    badge: 'CLIP', live: false, feed: '' },
  { key: 'ptch', label: 'PATCHES',  badge: 'PTCH', live: false, feed: '' },
  { key: 'scal', label: 'TUNINGS',  badge: 'SCAL', live: false, feed: '' },
  { key: 'fav',  label: '★ FAVES', badge: '', live: false, feed: '' },
];

/** Which chip the catalogue lights up. Found once; it is a constant table. */
const PLUG_CHIP = KINDS.findIndex((k) => k.feed === 'plug');

/** A row is one of three things, and which one decides everything about it. */
export const KIND_PROJECT = 'project';
export const KIND_PLUGIN = 'plugin';
/** A device kind the engine has built in — added by NAME, not by a scan index. */
export const KIND_DEVICE = 'device';

/** Where the catalogue stands. Not a boolean: "not yet" is not "none". */
export const PLUG_PENDING = 'pending';
export const PLUG_OK = 'ok';
export const PLUG_ERROR = 'error';

// Every string the draw path can write, interned. A template literal per row per
// frame is the regression alloc.mjs exists to catch; a handful of constants are
// not. Anything not here is built when a FEED arrives — once per reply, never
// once per frame.
const META_LOADED = 'uniproj · loaded';
const META_RECENT = 'uniproj · most recent';
const META_PLAIN = 'uniproj';
const META_UNSCANNED = 'the scan failed and gave no reason';
const MARK_LOADED = '▸';
const MARK_NONE = '';
const MARK_INSTRUMENT = 'INST';
const MARK_EFFECT = 'FX';
const MARK_FAILED = '!';
const BADGE_PROJECT = 'PROJ';
const BADGE_PLUGIN = 'PLUG';
const BADGE_DEVICE = 'DEV';
/*
 * What each built-in kind IS, in the one line the rail has room for. The engine's enum
 * names them ("patcher event", "sampler"); it does not say what they do, and a list of
 * six nouns is not a thing anyone can choose from.
 *
 * Indexed by DeviceKind, so this is positional against DEVICE_KINDS — which is imported
 * rather than restated for the reason chainmodel's own comment gives: a hand-maintained
 * mirror of an engine enum drifts, and this one already had once.
 */
const DEVICE_META = [
  'a patcher graph for notes and control',
  'a patcher graph that makes sound',
  'a patcher graph for audio effects',
  'a plugin instrument — pick one from PLUGINS instead',
  'a plugin effect — pick one from PLUGINS instead',
  'the built-in sampler: load a file, chop it, play it across the keys',
];
const SEP = ' · ';

const HINT_SAVING = 'Enter saves · Esc cancels';
const HINT_FINDING = 'typing filters · ↑↓ ⏎ still move and open · Esc clears';
// `⌘B`, not `B`. Plain B has never closed the rail — it types a note when the tracker has the
// keyboard — and a footer naming a key that does nothing is a footer that teaches the wrong
// thing to the one person reading it.
const HINT_LIST = 'Enter opens · F finds · S saves as · ⌘B closes';

const EMPTY_NONE = 'no projects on disk';
const EMPTY_FILTERED = 'no project matches';
const EMPTY_ANY_FILTERED = 'nothing matches';
const EMPTY_PLUG_FILTERED = 'no plugin matches';
const EMPTY_PLUG_NONE = 'the catalogue is empty — the engine scanned and found nothing';
const EMPTY_PLUG_PENDING = 'the plugin catalogue has not been read yet';

// The stat line's noun, as a pair so the singular is not a string built per
// change. Which one is used follows what is actually in the list.
const NOUN_PROJECT = { one: ' project', many: ' projects' };
const NOUN_PLUGIN = { one: ' plugin', many: ' plugins' };
const NOUN_ITEM = { one: ' item', many: ' items' };

const PH_PROJECTS = 'search projects…';
const PH_PLUGINS = 'search plugins…';
const PH_BOTH = 'search projects · plugins…';

// The footer's standing caveat, in two shapes. Named categories rather than a
// count, because "six kinds" is a number you have to go and check and this is a
// list you can read against the chips directly above it. The first shape is the
// truth before a catalogue arrives; the second is the truth after one does, and
// a note that kept saying "no plugins cross the wire" beside 52 of them would be
// the most confidently wrong thing on the screen.
const FOOT_NOTE = 'no plugins, presets, samples, clips, patches or tunings cross '
                + 'the wire yet — those chips are unavailable, not empty';
const FOOT_NOTE_PLUGINS = 'no presets, samples, clips, patches or tunings cross '
                + 'the wire yet — those chips are unavailable, not empty';

const TITLE_PLUG_LIVE = 'PLUGINS from the engine\'s own scan';
const TITLE_PLUG_PENDING = 'the plugin catalogue has not arrived yet';

/** A pooled row record. Grown once per shape change, mutated in place after. */
export function makeRow() {
  return { kind: KIND_PROJECT, name: '', meta: '', badge: '', mark: '',
           lower: '', ok: true, recent: false, instrument: false, plugin: null,
           // Where this plugin sits in the ENGINE's scan, which is how the engine
           // is asked to insert one. Stored explicitly rather than inferred from
           // the row's position: the rail filters and the view is a list of
           // indices into a list, so "the third row on screen" is not the third
           // plugin, and an insert keyed on a position that moves is the bug in
           // GUIDELINES 2.1 with a plugin on the end of it. -1 = not a plugin.
           pluginIndex: -1,
           // Which DeviceKind a DEVICES row makes. -1 = not a device row. Declared
           // here rather than assigned when a device row is filled: the pool's records
           // are mutated in place forever, and a record that grows a property later
           // changes its hidden class on a path that runs per frame.
           deviceKind: -1 };
}

/**
 * The built-in kinds this rail offers, as DeviceKind numbers.
 *
 * NOT all six. `vst instrument` and `vst effect` are deliberately absent: adding one
 * without naming a plugin makes a device with an empty vstRef, which is precisely the
 * card Jaakko asked about — "what's the VST instrument on track 1/Bass that doesn't
 * have anything loaded". The engine keeps it in the chain, nothing can load into it,
 * and the rack draws a box with a kind name and no plugin.
 *
 * A plugin device is made by choosing the PLUGIN, in the PLUGINS category, which
 * carries the vendor/name/path/uid the engine needs. Offering the bare kind here would
 * be a control whose only outcome is a broken device.
 */
const ADDABLE_DEVICE_KINDS = [0, 1, 2, 5];

/** Fill `row` from a DeviceKind number. The list is a constant, so this runs once. */
export function setDeviceRow(row, kind) {
  const name = DEVICE_KINDS[kind] || ('kind ' + kind);
  row.kind = KIND_DEVICE;
  row.badge = BADGE_DEVICE;
  row.name = name;
  row.meta = DEVICE_META[kind] || '';
  row.mark = MARK_NONE;
  row.ok = true;
  row.recent = false;
  row.instrument = false;
  row.plugin = null;
  row.pluginIndex = -1;
  row.deviceKind = kind;
  // Searchable by what it is as well as what it is called: typing "sampler", "patcher"
  // or "chop" should all narrow to something useful.
  row.lower = (name + ' ' + row.meta).toLowerCase();
  return row;
}

/**
 * Fill `row` from one project name. `at` is its index in the sidecar's list,
 * which is ordered newest first — the only other thing this side knows about a
 * project.
 *
 * The meta line is NOT built here: it depends on which project is loaded, which
 * changes without the list changing, so render() picks it from the interned
 * constants above. Building it here would need a rebuild on every load.
 */
export function setProjectRow(row, name, at) {
  row.kind = KIND_PROJECT;
  row.badge = BADGE_PROJECT;
  row.name = name;
  row.lower = name.toLowerCase();
  row.meta = '';
  row.mark = MARK_NONE;
  row.ok = true;
  row.recent = at === 0;
  row.instrument = false;
  row.plugin = null;
  return row;
}

/**
 * Fill `row` from one plugin catalogue entry.
 *
 * Every string the row will ever show is built here, once, when the catalogue
 * arrives. `plugin` keeps the entry itself, because the durable identity a host
 * will eventually need — vendor, name, path, uid16 — is on it and nowhere else.
 *
 * A failed scan keeps its row and wears the scanner's error as its meta line.
 * Filtering it out would answer "why is Zebra not in the list" with silence.
 */
export function setPluginRow(row, p, at) {
  const e = p || {};
  const name = e.name || e.path || '(unnamed)';
  const vendor = e.vendor || '';
  const format = e.format || '';
  const ok = !!e.ok;
  row.kind = KIND_PLUGIN;
  row.badge = BADGE_PLUGIN;
  row.name = name;
  row.ok = ok;
  row.instrument = !!e.is_instrument;
  row.recent = false;
  row.plugin = p || null;
  // fillRows hands us the index into the catalogue it was given, which is the
  // same array the engine scanned into pluginCache.entries — so this is the
  // hostSlotIndex an AddDevice needs.
  row.pluginIndex = at === undefined ? -1 : at;
  // Vendor and format when it worked; the reason when it did not. A row that
  // showed "u-he · VST3" for something that failed to load would be describing
  // a plugin this machine cannot actually offer.
  row.meta = ok ? (vendor ? vendor + SEP + format : format)
                : (e.error || META_UNSCANNED);
  // The trailing slot carries the fact you scan a list for. It survives the
  // meta line's truncation, which in a 266px rail is the point: a long vendor
  // must not be able to push "instrument" off the end of the row.
  row.mark = ok ? (row.instrument ? MARK_INSTRUMENT : MARK_EFFECT) : MARK_FAILED;
  // Everything a person might type to find it: "u-he", "vst3" and "instrument"
  // all narrow the list, and none of them costs anything at filter time.
  row.lower = (name + ' ' + vendor + ' ' + format + ' '
               + (row.instrument ? 'instrument' : 'effect')).toLowerCase();
  return row;
}

/**
 * Fill a pool from a feed, growing it and returning how many are live.
 *
 * The pool never shrinks and its records are never replaced — the whole point
 * of holding two pools (one per feed) rather than one is that a project re-list,
 * which happens whenever the rail opens, must not rebuild 52 plugin strings.
 */
export function fillRows(pool, items, fill) {
  const n = items ? items.length : 0;
  while (pool.length < n) pool.push(makeRow());
  for (let i = 0; i < n; i++) fill(pool[i], items[i], i);
  return n;
}

export class Browser {
  /**
   * @param {{onOpen?:function, onSave?:function, onRescan?:function,
   *          onClose?:function}} opts
   *
   * `onOpen` is handed the ROW, not a name: there are two kinds of row now and
   * they are opened by different means — one loads a project, the other cannot
   * do anything yet and has to say so. The record is pooled and mutated in
   * place, so a handler reads what it needs during the call and keeps no
   * reference to it.
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
    // Not the design's "search everything…". It names what is published — which
    // is now two feeds rather than one — and promising everything is the promise
    // this rail is built to avoid making. render() keeps it in step.
    this.input.placeholder = PH_PROJECTS;
    this._ph = PH_PROJECTS;
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
        el.title = k.feed === 'plug' ? TITLE_PLUG_PENDING
                                     : k.label + ' are not published by the engine yet';
      }
      this.chipsEl.appendChild(el);
      this.chips.push(el);
    }
    this._cat = null;
    this._plugChip = null;

    this.listEl = div('br-list', host);
    this.emptyEl = div('br-empty', host);
    this.emptyText = text(this.emptyEl);
    this._empty = null;
    this._emptyOn = null;

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
    // A Text node rather than a fixed string: the caveat has to stop naming
    // plugins the moment plugins arrive, and carry the catalogue's own reason
    // when there is one. See _noteText().
    this.noteText = text(div('br-note', foot));
    this._note = null;
    this.hint = div('br-hint', foot);
    this.hintText = text(this.hint);

    this.pool = [];
    /** Pooled records, one per project, in the sidecar's newest-first order. */
    this.projRows = [];
    this.projCount = 0;
    /** Pooled records, one per catalogue entry, in the scanner's order. */
    this.plugRows = [];
    this.plugCount = 0;
    /*
     * The built-in devices. Built ONCE, here, because unlike the other two this is not
     * a feed — it is the engine's enum, known at load, and it never changes.
     */
    this.devRows = ADDABLE_DEVICE_KINDS.map((k) => setDeviceRow(makeRow(), k));
    this.devCount = this.devRows.length;
    /** All feeds in display order: projects, plugins, devices. References only. */
    this.rows = [];
    /** The project names as last adopted, so a rename is caught by content. */
    this.projects = [];
    /** The catalogue array as last adopted; identity is the key. See setPlugins. */
    this.plugSrc = null;
    this.plugState = PLUG_PENDING;
    this.plugError = '';
    this.plugNote = '';
    this.view = [];           // indices into rows, after category and search
    this.selected = 0;        // indexes view, not rows
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
      this._open();
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
   * accepted `plug` before a catalogue arrived and then showed an empty list
   * would be claiming there are no plugins, which is a different and false
   * statement.
   */
  setCategory(key) {
    if (!this.available(key)) return false;
    if (this.category === key) return true;
    this.category = key;
    this.selected = 0;
    this._refilter();
    return true;
  }

  /**
   * Whether a chip has a feed behind it. `plug` answers for the catalogue's
   * arrival rather than for the protocol's existence: the wire can carry
   * plugins and this machine may still have none to show.
   */
  available(key) {
    for (let i = 0; i < KINDS.length; i++) {
      const k = KINDS[i];
      if (k.key !== key) continue;
      if (k.feed === 'plug') return this.plugState === PLUG_OK;
      return k.live;
    }
    return false;
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
    const moved = !this._sameProjects(names);
    if (moved) this._adoptProjects(names);
    if (this.current !== cur) this.current = cur;
    if (moved) this._rebuild();
    if (this.selected >= this.view.length) this.selected = Math.max(0, this.view.length - 1);
  }

  /**
   * The plugin catalogue, or the reason there is none.
   *
   * Also called once per draw, and the key is the ARRAY'S IDENTITY. That is
   * sound here and it is worth saying why, because keying on something that
   * does not move is the one bug this project keeps having: the list arrives
   * whole from a JSON.parse, so every reply mints a new array and no content
   * change can ever reuse the previous one. It is the safe direction of the
   * error — a reply identical to the last costs one rebuild, and a reply that
   * differs can never be missed. Contrast the project list, which the host
   * rebuilds in place, and which therefore has to be compared by content.
   */
  setPlugins(list, error) {
    const err = error || '';
    const src = list || null;
    if (src === this.plugSrc && err === this.plugError) return;
    this.plugSrc = src;
    this.plugError = err;
    this.plugState = err ? PLUG_ERROR : (src ? PLUG_OK : PLUG_PENDING);
    // Built once, here: the footer shows it every frame and must not build it.
    this.plugNote = err ? 'plugins: ' + err : '';
    this.plugCount = fillRows(this.plugRows, src, setPluginRow);
    // A category cannot outlive its feed. Losing the catalogue while PLUGINS is
    // selected would leave a disabled chip lit over an empty list.
    if (!this.available(this.category)) { this.category = 'all'; this.selected = 0; }
    this._rebuild();
  }

  _sameProjects(names) {
    const n = names ? names.length : 0;
    if (n !== this.projects.length) return false;
    for (let i = 0; i < n; i++) if (this.projects[i] !== names[i]) return false;
    return true;
  }

  _adoptProjects(names) {
    const n = names ? names.length : 0;
    this.projects.length = n;
    for (let i = 0; i < n; i++) this.projects[i] = names[i];
    this.projCount = fillRows(this.projRows, this.projects, setProjectRow);
  }

  /**
   * The two feeds in one display order. References into the two pools, so this
   * costs a pass over an array rather than 52 strings, and it runs when a feed
   * moves rather than per frame.
   */
  _rebuild() {
    const rows = this.rows;
    rows.length = 0;
    for (let i = 0; i < this.projCount; i++) rows.push(this.projRows[i]);
    for (let i = 0; i < this.plugCount; i++) rows.push(this.plugRows[i]);
    for (let i = 0; i < this.devCount; i++) rows.push(this.devRows[i]);
    this._refilter();
  }

  /**
   * Rebuild the visible set. Holds indices into `rows`, not names: a row's meta
   * line depends on where it sits in its feed's order, and a filtered position
   * cannot answer that.
   */
  _refilter() {
    const v = this.view;
    const q = this.queryLower;
    const cat = this.category;
    v.length = 0;
    for (let i = 0; i < this.rows.length; i++) {
      const r = this.rows[i];
      if (cat === 'proj' && r.kind !== KIND_PROJECT) continue;
      if (cat === 'plug' && r.kind !== KIND_PLUGIN) continue;
      if (cat === 'devs' && r.kind !== KIND_DEVICE) continue;
      if (q && r.lower.indexOf(q) < 0) continue;
      v.push(i);
    }
    if (this.selected >= v.length) this.selected = Math.max(0, v.length - 1);
  }

  move(delta) {
    if (!this.view.length) return;
    this.selected = Math.max(0, Math.min(this.view.length - 1, this.selected + delta));
  }

  /** The record a visible row holds, or null. Row indices are view indices. */
  rowAt(at) {
    return at >= 0 && at < this.view.length ? this.rows[this.view[at]] : null;
  }

  /** The name a visible row holds, or null. */
  nameAt(at) {
    const r = this.rowAt(at);
    return r ? r.name : null;
  }

  /** What a visible row IS. Two kinds open differently; the host decides how. */
  kindAt(at) {
    const r = this.rowAt(at);
    return r ? r.kind : null;
  }

  _open() {
    const row = this.rowAt(this.selected);
    if (!row) return null;
    this.onOpen && this.onOpen(row);
    return row;
  }

  /**
   * Open a row BY NAME, through the same `_open` Enter uses.
   *
   * For tests, and for the reason the probe that calls it explains: the rail's own
   * open path is the one that closes the rail, and a test that loads a project any
   * other way cannot see what that leaves behind.
   */
  openNamed(name) {
    for (let i = 0; i < this.view.length; i++) {
      const r = this.rowAt(i);
      if (r && r.name === name) { this.selected = i; return this._open(); }
    }
    return null;
  }

  openSelected() {
    const row = this._open();
    return row ? row.name : null;
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
      el._plug = null; el._inst = null; el._bad = null; el._ti = '';
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
      const row = this.rows[this.view[i]];
      const project = row.kind === KIND_PROJECT;
      const name = row.name;
      if (el._n !== name) {
        el._n = name;
        el._name.nodeValue = name;
        // For the agent loop: assert on the thing a row holds, not its slot.
        el.dataset.name = name;
      }
      if (el._k !== row.badge) { el._k = row.badge; el._badge.nodeValue = row.badge; }
      // `cur` is the LOADED project, which is not the highlighted one and is not
      // a thing a plugin can be.
      const cur = project && name === this.current;
      // Newest first is the sidecar's ordering, so the first project is the last
      // one written. A plugin's line was built when the catalogue arrived.
      const meta = project ? (cur ? META_LOADED : (row.recent ? META_RECENT : META_PLAIN))
                           : row.meta;
      if (el._m !== meta) { el._m = meta; el._meta.nodeValue = meta; }
      const mark = project ? (cur ? MARK_LOADED : MARK_NONE) : row.mark;
      if (el._mk !== mark) { el._mk = mark; el._mark.nodeValue = mark; }
      const sel = i === this.selected;
      if (el._sel !== sel) { el._sel = sel; el.classList.toggle('sel', sel); }
      if (el._cur !== cur) { el._cur = cur; el.classList.toggle('cur', cur); }
      const plug = !project;
      if (el._plug !== plug) { el._plug = plug; el.classList.toggle('plug', plug); }
      const inst = plug && row.instrument && row.ok;
      if (el._inst !== inst) { el._inst = inst; el.classList.toggle('inst', inst); }
      const bad = !row.ok;
      if (el._bad !== bad) { el._bad = bad; el.classList.toggle('bad', bad); }
      // A scanner's reason is longer than 266px and the meta line ellipsises it.
      // The title is where the whole of it can be read without clicking; the
      // click puts it on the reject line too, in full, for anyone who does.
      const tip = bad ? row.meta : '';
      if (el._ti !== tip) { el._ti = tip; el.title = tip; }
    }

    if (this._cat !== this.category) {
      this._cat = this.category;
      for (let i = 0; i < this.chips.length; i++) {
        this.chips[i].classList.toggle('on', KINDS[i].key === this.category);
      }
    }
    // The one chip whose availability is not a constant. Guarded on the state
    // rather than on the boolean, so the title follows the REASON: "not read
    // yet" and "the engine could not read it" are different sentences and the
    // chip is where a pointer asks the question.
    if (this._plugChip !== this.plugState && PLUG_CHIP >= 0) {
      this._plugChip = this.plugState;
      const live = this.plugState === PLUG_OK;
      const chip = this.chips[PLUG_CHIP];
      chip.disabled = !live;
      chip.classList.toggle('off', !live);
      chip.title = live ? TITLE_PLUG_LIVE
                        : (this.plugState === PLUG_ERROR ? this.plugError : TITLE_PLUG_PENDING);
    }

    const total = this._total();
    const noun = this._noun();
    if (this._n !== total || this._shown !== n || this._nounWas !== noun) {
      this._n = total; this._shown = n; this._nounWas = noun;
      // Two shapes, because a count that ignores the filter is a count you have
      // to reconcile against the rows yourself.
      this.statText.nodeValue = n === total
        ? total + (total === 1 ? noun.one : noun.many)
        : n + ' of ' + total + noun.many;
    }
    // Its own guard, and its own inputs: what an empty list MEANS depends on the
    // category and on where the catalogue stands, neither of which moves the
    // counts. Keyed on the count alone it went stale silently — GUIDELINES 2.1.
    const empty = this._emptyText();
    if (this._empty !== empty) { this._empty = empty; this.emptyText.nodeValue = empty; }
    const showEmpty = n === 0;
    if (this._emptyOn !== showEmpty) {
      this._emptyOn = showEmpty;
      this.emptyEl.style.display = showEmpty ? '' : 'none';
    }
    const note = this._noteText();
    if (this._note !== note) { this._note = note; this.noteText.nodeValue = note; }
    const ph = this._placeholder();
    if (this._ph !== ph) { this._ph = ph; this.input.placeholder = ph; }

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

  /** How many rows the current category has, filter aside. */
  _total() {
    if (this.category === 'proj') return this.projCount;
    if (this.category === 'plug') return this.plugCount;
    return this.rows.length;
  }

  /** What to call them. A mixed list is items; a list of one kind says so. */
  _noun() {
    if (this.category === 'proj') return NOUN_PROJECT;
    if (this.category === 'plug') return NOUN_PLUGIN;
    if (this.plugCount === 0) return NOUN_PROJECT;
    if (this.projCount === 0) return NOUN_PLUGIN;
    return NOUN_ITEM;
  }

  /**
   * What an empty list means. Every branch returns an interned constant or a
   * string built when the catalogue arrived, so this is safe to call per frame.
   */
  _emptyText() {
    if (this.category === 'plug') {
      if (this.plugState === PLUG_ERROR) return this.plugError;
      if (this.plugState === PLUG_PENDING) return EMPTY_PLUG_PENDING;
      if (this.plugCount === 0) return EMPTY_PLUG_NONE;
      return EMPTY_PLUG_FILTERED;
    }
    if (this.category === 'proj') return this.projCount === 0 ? EMPTY_NONE : EMPTY_FILTERED;
    if (this.rows.length === 0) {
      return this.plugState === PLUG_ERROR ? this.plugError : EMPTY_NONE;
    }
    return this.projCount === 0 || this.plugCount === 0 ? EMPTY_FILTERED : EMPTY_ANY_FILTERED;
  }

  /**
   * The standing caveat. It names the categories that are still unavailable, so
   * it has to stop naming plugins the moment a catalogue lands — and when the
   * sidecar refused, it carries the refusal, which is how the reason reaches
   * the screen without anyone having to hover a disabled chip.
   */
  _noteText() {
    if (this.plugState === PLUG_ERROR) return this.plugNote;
    return this.plugState === PLUG_OK ? FOOT_NOTE_PLUGINS : FOOT_NOTE;
  }

  /** The field promises exactly what it searches. */
  _placeholder() {
    if (this.category === 'proj') return PH_PROJECTS;
    if (this.category === 'plug') return PH_PLUGINS;
    return this.plugCount ? PH_BOTH : PH_PROJECTS;
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
                   available: this.available(KINDS[i].key),
                   selected: KINDS[i].key === this.category });
    }
    const items = [];
    for (let i = 0; i < this.rows.length; i++) items.push(this.rows[i].name);
    const shown = [];
    const rows = [];
    for (let i = 0; i < this.view.length; i++) {
      const r = this.rows[this.view[i]];
      shown.push(r.name);
      rows.push({ kind: r.kind, name: r.name, meta: r.meta, badge: r.badge,
                  mark: r.mark, ok: r.ok, instrument: r.instrument });
    }
    const sel = this.rowAt(this.selected);
    return { items, selected: this.selected,
             current: this.current, saving: this.field.active, saveText: this.field.text,
             query: this.query, searching: this.searching,
             category: this.category, shown, rows,
             selectedName: sel ? sel.name : null,
             selectedKind: sel ? sel.kind : null, chips,
             projects: this.projCount,
             plugins: { state: this.plugState, count: this.plugCount, error: this.plugError },
             unavailable: chips.filter((c) => !c.available).map((c) => c.key),
             note: this._noteText(),
             domNodes: this.pool.length };
  }
}
