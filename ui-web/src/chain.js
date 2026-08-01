// The device chain strip: what is on the cursor's track, in order.
//
// Same rules as every other renderer here (GUIDELINES 3): pooled elements that
// are hidden rather than removed, every style write guarded by a cached value,
// `.nodeValue` rather than `.textContent`, and nothing allocated when nothing
// changed. This surface redraws with the rest of the app, so "it is only a few
// cards" is not a reason to skip the discipline — it is how the discipline
// stops being true everywhere.
//
// TWO THINGS DECIDE THIS FILE'S SHAPE, and both come from one number: Zebra2
// publishes 256 parameters.
//
//  - The card cannot list them. The strip is 196px: 26 for its head, 16 of
//    padding, which leaves 154 for a card, and a card spends that on a title
//    line, a capability line and a footer before it gets to parameters. So the
//    PARAMETER LIST is the one part of a card whose length is unbounded, and it
//    is therefore the part that takes the leftover space and scrolls — about six
//    rows visible, 256 reachable. Everything else on a card is a fixed number of
//    lines and stays put. Cutting the list off at five and counting the rest was
//    the previous answer, and the count was never even drawn.
//  - 256 rows must not be 256 elements. The list is virtualised exactly as the
//    tracker is (GUIDELINES 3.4): a spacer holds the full scroll extent, the
//    rows are absolutely positioned, and the pool is a RING indexed by
//    `row mod poolSize` so that scrolling one row rebinds one row rather than
//    shifting every element's identity. Eight cards of Zebra2 are ~60 row
//    elements in total, created once.

import { createParamEdits, findParamEdit, setParamEdit, dropParamEdit,
         reapParamEdits, FILTER_TYPES } from './chainmodel.js';

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

/** What a refused edit says, once its hold has run out. */
const SNAPPED = 'the engine did not take that parameter change — bar reset';
/** What a drag says when nothing is wired to carry it. */
const NO_ROUTE = 'nothing is listening for parameter edits';

/*
 * THE WAVEFORM CONTRACT, copied from arrange.js because both sides of it must agree.
 *
 * Windows are keyed on (source, decimation, firstFrame) with the decimation a POWER OF TWO and
 * the first frame anchored to a multiple of `dec * WAVE_TILE_COLS`. Asking for anything else
 * produces a key nothing ever lands under — a request that succeeds and an answer that never
 * arrives, which is indistinguishable from a source that failed to decode.
 */
const WAVE_TILE_COLS = 4096;
const WAVE_MASK_BOTH = 3;

export class Chain {
  /**
   * @param {HTMLElement} host
   * @param {{onSelect?: (pos:number) => void, onAdd?: (track:number) => void,
   *          onParam?: (edit:{track:number, device:number, index:number,
   *                           uid:string, valueMilli:number}) => boolean}} opts
   *
   * `onAdd` is called with the track the strip is CURRENTLY showing, not with
   * whatever the caller thinks the cursor is on — the two differ for a frame
   * whenever the cursor moves, and a device added to the previous track is the
   * kind of wrong that looks like nothing happened.
   *
   * `onParam` MUST RETURN whether the edit actually went out. It is the only way
   * this side can tell "sent, now waiting" from "there is no socket": both look
   * identical from here, and only one of them justifies moving the bar. A
   * falsey return leaves the bar where the plugin says it is and puts the reason
   * on the strip, rather than showing a value nobody has accepted.
   */
  constructor(host, { onSelect, onAdd, onParam, onOpenEditor, onDelete,
                      onOpenPatcher, onBypass, onFilter, onDefaultGate, onMap } = {}) {
    this.host = host;
    this.host.className = 'dv';
    this.onSelect = onSelect;
    this.onOpenEditor = onOpenEditor;
    this.onAdd = onAdd;
    this.onParam = onParam;
    // Destructured here or it does not exist. index.html passed `onDelete`, the
    // button rendered, the hit test found it, `_down` matched `.dv-del` — and
    // `this.onDelete` was undefined, so the branch fell straight through and the
    // click did nothing at all. No error, nothing on screen: a control that is
    // present, reachable and inert. Only pressing it finds this.
    this.onDelete = onDelete;
    // Double-clicking a PATCHER device opens its graph. Its own callback rather
    // than a flag on `onOpenEditor`, because the two go to different places: an
    // editor is the plugin's own window, a graph is a surface in this app.
    this.onOpenPatcher = onOpenPatcher;
    // Destructured, for the reason onDelete's comment gives — that failure looks
    // exactly like a working button and is only found by pressing it.
    this.onBypass = onBypass;
    // The sampler's filter, cycled from the card. Same shape as bypass: the handler names the
    // state it wants rather than asking for the next one.
    this.onFilter = onFilter;
    // The bank's note-off default. Same shape as onFilter: names the state it wants.
    this.onDefaultGate = onDefaultGate;
    /*
     * MAP. Destructured and assigned here for the same reason `onBypass` is, one line up:
     * a callback the constructor accepts and never stores gives a badge that looks live,
     * responds to a hover, and does nothing — and nothing throws, because a listener
     * nobody reaches is not a fault (GUIDELINES 2.15).
     */
    this.onMap = onMap;

    const head = div('dv-head', host);
    this.label = text(div('dv-label', head));
    this.track = text(div('dv-track', head));
    this.version = text(div('dv-version', head));
    this.notice = text(div('dv-notice', head));
    // What the RACK has to say, as opposed to what the model says about the
    // chain: a refusal it made itself, or one it watched the engine make by
    // never answering. Separate element because the two have different authors
    // and the model overwrites its own line on every draw.
    this.saidEl = div('dv-said', head);
    this.said = text(this.saidEl);
    this._said = '';

    this.cardsEl = div('dv-cards', host);
    this.pool = [];
    this.vm = null;
    // The version line is memoised on the NUMBERS it is built from rather than
    // on the string it produces — see `render`. `null` is a value neither of
    // them can take, so the first draw always writes.
    this._track = null; this._known = null; this._verNum = null; this._notice = null;

    /** Edits sent and not yet confirmed, and the one the pointer is holding. */
    this.edits = createParamEdits();
    this._drag = null;

    // Row and list geometry, measured from the real elements rather than
    // restated here (GUIDELINES 3.11): a hand-copied 13px stops being true the
    // first time the font or the card padding moves, and the scroll extent
    // computed from it would leave the last rows unreachable — which is the
    // exact bug that hid 30px of the mixer.
    this._rowH = 0;
    this._listH = 0;
    this._geom = false;

    // One coalesced frame of our own (GUIDELINES 3.8). The app's scheduler draws
    // when the app's state changes; a list scrolling inside a card and an edit
    // ageing out are changes the app does not know about.
    this._queued = false;
    this._onFrame = () => {
      this._queued = false;
      if (this.vm && !this.host.hidden) this.render(this.vm);
    };
    // Pre-bound, so the per-frame reap below allocates no closure.
    this._engineMilli = (device, index) => this._published(device, index);

    // Built once, here, and never touched again by a draw.
    //
    // It is the FIRST child rather than the last because the pool appends: a
    // card created on the fifth device would land after anything the constructor
    // put at the end, so keeping it last in the DOM would mean moving a node
    // every time the pool grows. CSS `order` puts it last on screen instead,
    // which costs nothing and never mutates the tree. It is deliberately not a
    // `.dv-card` — the delegated handler below reads `dataset.pos` off those,
    // and a card with no position would select NaN.
    this.addEl = div('dv-add', this.cardsEl);
    this.addEl.setAttribute('role', 'button');
    this.addEl.setAttribute('aria-label', 'add a device to this chain');
    this.addEl.title = 'add a device';
    text(this.addEl).nodeValue = '+';

    // One listener on the container rather than one per card: cards come and go
    // with the pool, and a listener per pooled element is a listener rebound
    // every time the pool grows.
    this.cardsEl.addEventListener('pointerdown', (e) => this._down(e));
    this.cardsEl.addEventListener('pointermove', (e) => this._move(e));
    this.cardsEl.addEventListener('pointerup', () => this._up());
    this.cardsEl.addEventListener('pointercancel', () => this._up());
    /*
     * DOUBLE-CLICK OPENS THE DEVICE.
     *
     * `dblclick`, not a hand-rolled pair of clicks: the browser already knows
     * the platform's interval and its slop radius, and both differ per OS.
     *
     * Which "open" depends on the KIND, and the kinds are the engine's own — a
     * patcher device opens its graph, a plugin opens its editor window. Sending
     * a patcher to the editor path is not merely wrong, it is SILENT: the engine
     * skips OpenPluginEditor for non-VST kinds and logs to its own stderr, so
     * the card looked inert for the same reason `onDelete` did.
     *
     * The controls are excluded first. A double-click that lands on the delete
     * button is two deletes, not a delete and an open.
     */
    this.cardsEl.addEventListener('dblclick', (e) => {
      if (e.target.closest('.dv-add, .dv-open, .dv-del, .dv-byp, .dv-flt, .dv-gat, .dv-p-bar')) return;
      const card = e.target.closest('.dv-card');
      if (!card || !this.vm) return;
      const pos = Number(card.dataset.pos);
      const c = this.vm.cards[pos];
      if (!c) return;
      // Resolved by DEVICE ID, not by the position the first click saw. The two
      // clicks are a third of a second apart and the pool rebinds on any chain
      // change, so a device added or removed in between makes `pos` mean
      // something else entirely.
      if (c.kind <= 2 && c.patcherNode >= 0) {
        if (this.onOpenPatcher) this.onOpenPatcher(this.vm.track, c.id, pos);
      } else if (this.onOpenEditor) {
        /*
         * AN OBJECT, which is what the handler destructures.
         *
         * This passed two positional arguments while `onOpenEditor` takes
         * `({ track, device })`, so destructuring a NUMBER gave undefined for both
         * and double-clicking a VST card opened nothing at all. Silent in every
         * direction: no throw, no message, and the other call site (the open button)
         * passes the object correctly, so the feature demonstrably worked.
         *
         * `onOpenPatcher` above really is positional — the two callbacks have
         * different shapes, which is how this happened and why they now look
         * different on the page.
         */
        this.onOpenEditor({ track: this.vm.track, device: c.id });
      }
    });
    // Scroll does not bubble, but it does fire on the way DOWN — so one capturing
    // listener on the container sees every card's list. Per-card listeners would
    // have to be rebound every time the pool grows, which is the same reason
    // every other handler here is delegated.
    this.cardsEl.addEventListener('scroll', (e) => {
      const l = e.target;
      if (!l || !l._card) return;
      l._card._scrollTop = l.scrollTop;
      this._frame();
    }, true);
    /*
     * THE CARD BOX IS MEASURED, so anything that moves it invalidates the
     * measurement — not only a window resize.
     *
     * That is what this was, and it was wrong: dragging the chain strip TALLER
     * changes the card's height without a window resize, so `_listH` stayed at
     * whatever it was when the strip was short and the parameter list kept
     * showing the same handful of rows in a box with room for thirty. Reported
     * as "the scrollable VSTi parameter section stays the same size".
     *
     * A ResizeObserver watches the box itself, which is the thing the
     * measurement is OF. It clears the flag and asks for a frame — the class's
     * own throttled redraw, not the page's, because the page has no reason to
     * know the rack re-measures.
     *
     * No feedback loop: the callback writes no geometry, and `_grow` appends
     * into `.dv-pspace`, which is inside a `contain: strict` scroller and cannot
     * change `.dv-cards`.
     */
    if (typeof ResizeObserver === 'function') {
      this._ro = new ResizeObserver(() => { this._geom = false; this._frame(); });
      this._ro.observe(this.cardsEl);
    }
    // Kept as well. An observer fires on the element's own box; a window resize
    // that leaves the box alone can still change the row height through a font
    // or a metric, and that is what `_rowH` is.
    addEventListener('resize', () => { this._geom = false; });
  }

  /**
   * Replace the in-flight edit store.
   *
   * The strip makes its own, so the page has to wire nothing but `onParam`. This
   * exists for the tests, which drive settle / expire / answered-with-a-
   * different-value directly — the interesting cases all live in the
   * reconciliation rather than in the drawing.
   */
  attachEdits(edits) { this.edits = edits; return this; }

  /** Ask for one frame of our own, coalescing whatever else asked this frame. */
  _frame() {
    if (this._queued) return;
    this._queued = true;
    requestAnimationFrame(this._onFrame);
  }

  /** What the ENGINE last published for a parameter, in milli-units, or -1. */
  _published(device, index) {
    const vm = this.vm;
    if (!vm) return -1;
    for (let i = 0; i < vm.cardCount; i++) {
      const c = vm.cards[i];
      if (c.id !== device) continue;
      // The engine publishes them in index order, so the common case is a hit
      // rather than a scan of 256. Bounded by paramCount, not by the pool's
      // length: the slots past the count still hold the LAST device's values.
      const at = index < c.paramCount ? c.params[index] : null;
      if (at && at.index === index) return at.milli;
      for (let k = 0; k < c.paramCount; k++) {
        if (c.params[k].index === index) return c.params[k].milli;
      }
      return -1;
    }
    return -1;
  }

  _card(n) {
    while (this.pool.length < n) {
      const el = div('dv-card', this.cardsEl);
      const top = div('dv-card-top', el);
      const badge = div('dv-badge', top);
      const title = div('dv-title', top);
      const body = div('dv-body', el);
      // The scroller and, inside it, a spacer holding the full extent. The rows
      // live in the spacer and are placed absolutely, so scrolling moves nothing
      // but the scroll offset.
      /*
       * THE METERS (v24). Two rows at most — in above out — each an RMS fill with
       * a PEAK tick riding on top of it.
       *
       * Both, because they disagree in the way that matters: a limiter's job is
       * to hold the peak down while the RMS climbs, and either number alone hides
       * exactly the thing you put the limiter there to see. The tick is a
       * separate element rather than a second background, so moving it is one
       * transform and never a repaint of the bar under it.
       *
       * ABOVE the parameter list, under the facts about what the device IS. The
       * list SCROLLS, and a live readout pinned to the bottom of a scrolling
       * region moves when you scroll something unrelated to it.
       */
      const meter = div('dv-meter', el);
      const rowIn = div('dv-m-row in', meter);
      const inFill = div('dv-m-fill', rowIn);
      const inTick = div('dv-m-tick', rowIn);
      const rowOut = div('dv-m-row', meter);
      const outFill = div('dv-m-fill', rowOut);
      const outTick = div('dv-m-tick', rowOut);
      const outDb = text(div('dv-m-db', meter));
      el._meter = meter; el._rowIn = rowIn;
      el._inFill = inFill; el._inTick = inTick;
      el._outFill = outFill; el._outTick = outTick; el._outDb = outDb;
      el._mi = -1; el._mip = -1; el._mo = -1; el._mop = -1;
      el._mOn = null; el._mIn = null; el._mDb = null;
      const list = div('dv-plist', el);
      const space = div('dv-pspace', list);
      list._card = el;
      el._list = list; el._space = space;
      /*
       * THE SAMPLE VIEW — one sampler's audio, with its slice boundaries drawn on it.
       *
       * A CANVAS, and a sibling of the list rather than a row in it: the list is a pooled,
       * absolutely-positioned row machine keyed by parameter index, and a waveform is one
       * picture. It sits hidden until `defaultView` says otherwise, so an ordinary rack pays a
       * single detached element per card and nothing else.
       *
       * The kit list is what a sampler shows by default and it stays the default: this answers
       * "where in the file is slice 3", which is a different question from "what is on pad 3"
       * and the reason the extents were published at all.
       */
      const wave = document.createElement('canvas');
      wave.className = 'dv-wave';
      el.insertBefore(wave, list);
      el._wave = wave; el._waveCtx = null; el._waveKey = ''; el._waveOn = null;
      el._rows = [];
      el._fIn = null; el._fOut = null;
      el._scrollTop = 0; el._spaceH = -1;
      const foot = div('dv-foot', el);
      // The device's audio buses, on its own line: "8 out · 1 in", or "buses 3/8"
      // while they are still arriving. Its own element rather than folded into the
      // footer because the two answer different questions — the footer is where the
      // device sits in the chain, this is what it can route — and because it is the
      // one line that changes when a plugin renegotiates its layout.
      /**
       * Open the plugin's own window.
       *
       * The engine has accepted OpenPluginEditor since before this UI existed and
       * nothing ever sent it — "how do I open the plugin UI" had the answer "you
       * can't", for a capability that was already there. A command reached it
       * first; a command you have to know the name and the device id of is only
       * half-reachable, so it gets the button it should always have had.
       *
       * Only meaningful for a device with an editor, and the rack does not know
       * which those are — the engine publishes no has_editor flag. Shown for all
       * of them rather than guessed at: a button that does nothing on a device
       * with no window is a smaller lie than a missing button on one that has.
       */
      const open = document.createElement('button');
      open.className = 'dv-open';
      open.title = 'Open the plugin\'s own window';
      // An icon, not the word: the card is 232px wide and every pixel of it is
      // spoken for by the device's name and its parameters. `arrow-square-out` is
      // the same glyph every application uses for "this opens somewhere else",
      // which is exactly what it does.
      const openIcon = document.createElement('i');
      openIcon.className = 'ph ph-arrow-square-out';
      open.appendChild(openIcon);
      /**
       * Remove the device.
       *
       * The rack could add a device and could not remove one. `delDevice` existed
       * as a __uni hook the whole time, with no button, no key and no dock
       * command behind it — reachable from a test and from nowhere a person can
       * point at. Jaakko asked "how do I delete a device from the device chain"
       * and the honest answer was "you cannot".
       *
       * The op-registry test knew: it had `deldevice` recorded as a gap. Recording
       * a hole is not the same as closing one, and a ratchet that permits an entry
       * forever is a list of things nobody is going to do.
       */
      const del = document.createElement('button');
      del.className = 'dv-del';
      del.title = 'Remove this device';
      const delIcon = document.createElement('i');
      delIcon.className = 'ph ph-trash';
      del.appendChild(delIcon);
      /**
       * Bypass the device.
       *
       * The chain snapshot has carried every device's bypass state since v20 and
       * this card has dimmed itself for it the whole time — with nothing able to
       * SET it. A state you can see and cannot change reads as a control that has
       * stopped working, and the only way in was a project file.
       *
       * It sits beside remove because they are the two things you do to a device
       * you are unsure about, and one of them is reversible. `power` rather than a
       * crossed-out circle: this is the switch, not a warning.
       */
      const byp = document.createElement('button');
      byp.className = 'dv-byp';
      byp.title = 'Bypass this device';
      const bypIcon = document.createElement('i');
      bypIcon.className = 'ph ph-power';
      byp.appendChild(bypIcon);
      /*
       * THE SAMPLER'S FILTER, as a cycling button.
       *
       * Text rather than an icon because there is no icon for "LP24" that anyone reads faster
       * than the letters, and the states are a short closed list. Built for every card and shown
       * only on samplers — the pool is reused as the rack changes shape, so creating it
       * conditionally would mean a card that becomes a sampler has no button until it is
       * rebuilt.
       *
       * It exists because opcode 86 gave the filter a write path at last: until this morning no
       * command in the product could turn it on, so every cutoff modulator was inert and the
       * kit's `!` badge was the only state reachable. A console verb alone would leave the
       * pointer unable to do something the keyboard can, which is the half-built shape this
       * project keeps refusing.
       */
      const flt = document.createElement('button');
      flt.className = 'dv-flt';
      flt.appendChild(document.createTextNode(''));
      el._fltEl = flt;
      el._flt = -2;                      // not -1: -1 is "no filter here", a value it can hold
      /*
       * THE BANK'S NOTE-OFF DEFAULT, as a two-state button beside the filter.
       *
       * Jaakko asked for this as "a setting per bank: ignore note-offs", and it is the setting
       * rather than the mechanism — it SEEDS every slot the kit mints from then on and leaves
       * the ones already there alone. So the label says what NEW pads will do, which is why it
       * reads "1shot"/"gate" rather than describing the kit as a whole.
       */
      const gat = document.createElement('button');
      gat.className = 'dv-gat';
      gat.appendChild(document.createTextNode(''));
      el._gatEl = gat;
      el._gat = -2;                      // not -1: -1 is "no sampler here", a value it can hold
      el.append(open, gat, flt, byp, del);
      const bus = div('dv-bus', el);
      el._busEl = bus;
      el._badge = text(badge);
      el._title = text(title);
      el._body = text(body);
      el._foot = text(foot);
      el._bus = text(bus);
      el._b = null; el._t = null; el._y = null; el._f = null; el._bt = null;
      el._sel = null; el._byp = null;
      // `null` rather than -1: a chain position of -1 is not a thing the engine
      // publishes, but a cache that starts at a real value is one write away
      // from being wrong on the first frame.
      el._pos = null;
      el._devId = -1;
      this.pool.push(el);
    }
    return this.pool;
  }

  _down(e) {
    /*
     * MAP, before the parameter bar. The badge sits inside the row, so a click on it is
     * also a click on the row — and the row's bar is a DRAG that writes a value, so
     * without this order pressing MAP would also move the parameter it was mapping.
     */
    const mapper = e.target.closest('.dv-p-map');
    if (mapper) {
      const card = mapper.closest('.dv-card');
      const row = mapper.closest('.dv-p');
      if (card && row && this.onMap) {
        const link = Number(mapper.dataset.link || 0);
        this.onMap({
          track: this.vm ? this.vm.track : -1,
          device: card._devId,
          param: row._pi,
          uid: row._uid,
          // The link that is already there, or 0. Which direction this is: an id means
          // unmap THAT link, and 0 means make one.
          link,
        });
      }
      // Consumed, so the bar drag below never starts. Otherwise the parameter jumps to
      // wherever in its range the badge happens to sit.
      e.preventDefault();
      return;
    }
    if (e.target.closest('.dv-add')) {
      if (this.onAdd) this.onAdd(this.vm ? this.vm.track : -1);
      return;
    }
    // Before the card's own select: the button is inside the card, so a click on
    // it is also a click on the card, and selecting underneath would be a second
    // thing happening that nobody asked for.
    const opener = e.target.closest('.dv-open');
    if (opener) {
      const c = opener.closest('.dv-card');
      if (c && this.onOpenEditor) {
        this.onOpenEditor({ track: this.vm ? this.vm.track : -1, device: c._devId });
      }
      return;
    }
    const gate = e.target.closest('.dv-gat');
    if (gate) {
      const c = gate.closest('.dv-card');
      if (c && this.onDefaultGate) {
        const pos = Number(c.dataset.pos);
        const card = this.vm && this.vm.cards && this.vm.cards[pos];
        if (card && card.defaultGate >= 0) {
          if (this.onSelect) this.onSelect(pos);
          // The state we WANT, from the one being drawn — never "toggle". Two presses racing on
          // the ring both meaning "the other one" cancel out; two that name a state settle.
          this.onDefaultGate({ track: this.vm ? this.vm.track : -1, device: c._devId,
                               on: card.defaultGate === 0 });
        }
      }
      return;
    }
    const filt = e.target.closest('.dv-flt');
    if (filt) {
      const c = filt.closest('.dv-card');
      if (c && this.onFilter) {
        const pos = Number(c.dataset.pos);
        const card = this.vm && this.vm.cards && this.vm.cards[pos];
        if (card && card.filterType >= 0) {
          if (this.onSelect) this.onSelect(pos);
          // The state we WANT, computed from the one being drawn — never "next from whatever it
          // is now". Two presses racing on the command ring both mean "the one after that", and
          // the pair lands somewhere neither press asked for; two presses that NAME a state
          // settle on the second. Same reasoning as bypass, one line above.
          const next = (card.filterType + 1) % FILTER_TYPES.length;
          this.onFilter({ track: this.vm ? this.vm.track : -1, device: c._devId, type: next });
        }
      }
      return;
    }
    const bypasser = e.target.closest('.dv-byp');
    if (bypasser) {
      const c = bypasser.closest('.dv-card');
      if (c && this.onBypass) {
        const pos = Number(c.dataset.pos);
        const card = this.vm && this.vm.cards && this.vm.cards[pos];
        /*
         * THIS one selects the card, where open and delete do not.
         *
         * Their reason for not selecting is that the card is about to stop being
         * the thing you are looking at — a plugin window takes over, or the card
         * goes away. Bypass is the opposite: the card stays, and you are almost
         * certainly about to press it again, because comparing is the entire
         * point of the control. Selecting hands the rack the keyboard, so the
         * second and third comparisons are the `b` key rather than the mouse.
         */
        if (this.onSelect) this.onSelect(pos);
        // The state we WANT, computed from the one we are drawing — not "toggle".
        // Two presses racing on the command ring both mean "the other one", and
        // the pair cancels out; two presses that name a state settle on the second.
        this.onBypass({ track: this.vm ? this.vm.track : -1, device: c._devId,
                        on: !(card && card.bypass) });
      }
      return;
    }
    const remover = e.target.closest('.dv-del');
    if (remover) {
      const c = remover.closest('.dv-card');
      if (c && this.onDelete) {
        const pos = Number(c.dataset.pos);
        // The card's OWN title, not the raw device. `kind` is a number, so
        // `dev.name || dev.kind` asked "Remove 3?" — the vm already resolves a
        // plugin name, or a readable kind, and that is what the card is showing.
        const card = this.vm && this.vm.cards && this.vm.cards[pos];
        this.onDelete({ track: this.vm ? this.vm.track : -1, device: c._devId,
                        pos, title: (card && card.title) || 'this device' });
      }
      return;
    }
    const card = e.target.closest('.dv-card');
    if (card && this.onSelect) this.onSelect(Number(card.dataset.pos));
    const bar = e.target.closest('.dv-p-bar');
    if (!bar || !card) return;
    const row = bar.parentNode;
    // A row that has never been bound has no parameter behind it. Refusing here
    // is cheaper than sending index -1 and reading the sidecar's answer to a
    // question nobody meant to ask.
    if (!row || !(row._pi >= 0)) return;
    /*
     * A SLOT ROW IS NOT A FADER.
     *
     * `_pi` is the parameter index for a plugin and the SLOT ID for a sampler pad — both
     * non-negative, so the guard above passed and dragging a pad's bar sent SetParam for
     * parameter 1 of a sampler. It moved nothing (the sampler has no such parameter) and it
     * reported nothing, which is why it survived: the bar was empty, so there was no reason to
     * drag it. Now that the bar draws the slice extent there is every reason.
     */
    if (row.classList.contains('slot')) return;
    // The rect is taken once and reused for the whole gesture, exactly as the
    // mixer's fader does: the fill inside the bar changes width as you drag, and
    // re-measuring would move the reference frame under the pointer.
    const r = bar.getBoundingClientRect();
    this._drag = { el: card, rect: r, index: row._pi, uid: row._uid, slot: null, last: -1 };
    // The gesture belongs to the strip until the button comes up, even if the
    // pointer leaves the 3px bar — which it will, immediately, on a 13px row.
    this.cardsEl.setPointerCapture(e.pointerId);
    // A drag inside a scroller is a drag, not a scroll.
    e.preventDefault();
    this._apply(e.clientX);
  }

  _move(e) {
    if (!this._drag) return;
    this._apply(e.clientX);
  }

  _up() {
    if (!this._drag) return;
    this._drag = null;
    this._frame();
  }

  /**
   * Send one value, and hold it locally until the engine agrees.
   *
   * Milli-units on the wire, not a 0..1 float. The engine's own region carries
   * `valueMilli` and the sidecar's command parser reads integers — a float there
   * would parse as its integer part, so 0.62 would arrive as 0 and every drag
   * would read as "set it to minimum". A unit that cannot be silently truncated
   * is worth more than a prettier number.
   */
  _apply(clientX) {
    const d = this._drag;
    const el = d.el;
    const r = d.rect;
    if (!r.width) return;
    let f = (clientX - r.left) / r.width;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    const milli = Math.round(f * 1000);
    // One send per value, not one per pointermove: a move event that lands on
    // the same milli-unit is not an edit, and a socket message per mouse sample
    // is a query storm the engine did not ask for.
    if (milli === d.last) return;
    d.last = milli;
    const sent = this.onParam
      ? !!this.onParam({ track: this.vm ? this.vm.track : -1, device: el._devId,
                         index: d.index, uid: d.uid, valueMilli: milli })
      : false;
    if (!sent) {
      // Nothing went out, so there is nothing to be optimistic about. Say so
      // rather than moving a bar that no longer describes the plugin.
      if (d.slot) { dropParamEdit(this.edits, d.slot); d.slot = null; }
      this._say(NO_ROUTE);
      this._frame();
      return;
    }
    this._say('');
    if (this.edits) {
      d.slot = setParamEdit(this.edits, el._devId, d.index, milli, performance.now());
    }
    this._frame();
  }

  _say(s) {
    if (this._said === s) return;
    this._said = s;
    this.said.nodeValue = s;
    this.saidEl.classList.toggle('on', !!s);
  }

  render(vm) {
    this.vm = vm;
    // Settle or expire first, so the rows below draw one consistent answer.
    if (this.edits && this.edits.count) {
      const held = this._drag ? this._drag.slot : null;
      const gone = reapParamEdits(this.edits, this._engineMilli, performance.now(), held);
      if (gone) this._say(SNAPPED);
    }
    if (this._notice !== vm.notice) { this._notice = vm.notice; this.notice.nodeValue = vm.notice; }
    if (this._track !== vm.trackName) {
      this._track = vm.trackName;
      this.track.nodeValue = vm.trackName;
    }
    // "chainVersion 87" when there is one, and nothing at all when the engine
    // has not published a chain — a version of -1 on screen is worse than no
    // version, because it looks like a number somebody could act on.
    //
    // Keyed on (known, version), which is everything the line is made of.
    // Building the string first and comparing it afterwards still concatenated
    // once per frame — sixteen bytes, 60 times a second, to discover that the
    // chain version had not moved.
    if (this._known !== vm.known || this._verNum !== vm.version) {
      this._known = vm.known; this._verNum = vm.version;
      this.version.nodeValue = vm.known ? 'chainVersion ' + vm.version : '';
    }
    if (this.label.nodeValue !== 'DEVICE CHAIN') this.label.nodeValue = 'DEVICE CHAIN';

    const pool = this._card(vm.cardCount);
    for (let i = 0; i < pool.length; i++) {
      const el = pool[i];
      const on = i < vm.cardCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const c = vm.cards[i];
      el._devId = c.id;
      if (el._b !== c.badge) { el._b = c.badge; el._badge.nodeValue = c.badge; }
      /*
       * The filter button, guarded on the VALUE and not merely on being a sampler: this runs per
       * card per frame, and an unguarded nodeValue write is a string assignment sixty times a
       * second for every card in the rack.
       */
      if (el._gat !== c.defaultGate) {
        el._gat = c.defaultGate;
        const off = c.defaultGate < 0;
        el._gatEl.style.display = off ? 'none' : '';
        if (!off) {
          const on = c.defaultGate !== 0;
          el._gatEl.firstChild.nodeValue = on ? 'gate' : '1shot';
          el._gatEl.title = on
            ? 'New pads respect note-off — click for one-shot (plays the whole sample)'
            : 'New pads are one-shot and ignore note-off — click to gate them';
          // Marked when it is the accidental default rather than a choice: a slot minted at
          // one-shot plays its whole sample however short the note is.
          el._gatEl.classList.toggle('off', !on);
        }
      }
      if (el._flt !== c.filterType) {
        el._flt = c.filterType;
        const off = c.filterType < 0;
        el._fltEl.style.display = off ? 'none' : '';
        if (!off) {
          el._fltEl.firstChild.nodeValue = FILTER_TYPES[c.filterType] || '?';
          // The label says the state; the title says what pressing it does, because a button
          // showing "lp24" reads equally as "it is lp24" and "press for lp24".
          el._fltEl.title = `Filter: ${FILTER_TYPES[c.filterType]} — click for `
                          + `${FILTER_TYPES[(c.filterType + 1) % FILTER_TYPES.length]}`;
          // OFF is the state a cutoff modulator is inert in, so it is marked rather than left
          // looking like any other setting.
          el._fltEl.classList.toggle('off', c.filterType === 0);
        }
      }
      if (el._t !== c.title) { el._t = c.title; el._title.nodeValue = c.title; }
      const body = c.caps || 'no declared capabilities';
      if (el._y !== body) { el._y = body; el._body.nodeValue = body; }
      this._params(el, c);
      if (el._f !== c.sub) { el._f = c.sub; el._foot.nodeValue = c.sub; }
      if (el._bt !== c.busText) {
        el._bt = c.busText;
        el._bus.nodeValue = c.busText;
        // Hidden rather than blank when a device publishes no buses: an empty line
        // still takes its height, and every non-plugin device in the rack would
        // grow a gap where a fact about plugins used to be.
        const bd = c.busText ? '' : 'none';
        if (el._busEl.style.display !== bd) {
          el._busEl.style.display = bd;
          // Showing or hiding this line changes how much height is left INSIDE
          // one card, which the observer on `.dv-cards` cannot see — that box
          // did not move. `_listH` is one scalar shared by every card, so a card
          // that gained a bus line would keep drawing rows for the height it had
          // without one.
          this._geom = false;
        }
        // Incomplete and truncated are different states and read differently: one
        // resolves in a frame, the other never will.
        el._busEl.classList.toggle('partial', c.busPartial);
        el._busEl.classList.toggle('trunc', c.busTruncated);
      }
      /*
       * METERS. Every write guarded on a cached number (GUIDELINES 3.2), because
       * this is the one part of a card that genuinely changes every frame — so it
       * is also the one place where an unguarded style write costs 120 layouts a
       * second per card rather than none.
       *
       * Quantised to whole percent before comparing. The engine publishes a new
       * number every block and the bar is ~64px wide, so the untruncated value
       * differs every single frame while the PIXEL does not: rounding first is
       * what makes the guard a guard instead of a formality.
       */
      if (el._mOn !== c.hasMeter) {
        el._mOn = c.hasMeter;
        el._meter.style.display = c.hasMeter ? '' : 'none';
        /*
         * EMPTY IT ON THE WAY OUT. The pool rebinds cards to different devices,
         * so a hidden meter keeps the last width it was given — and a card that
         * held a loud insert leaves a 100% bar sitting in a hidden element. It
         * cannot be seen there, but the moment that card is rebound to a device
         * that DOES meter, the first frame draws the previous device's level:
         * a pegged bar, for one frame, on the wrong instrument. Cheaper to clear
         * than to reason about which frame wins.
         */
        if (!c.hasMeter) {
          el._mi = -1; el._mip = -1; el._mo = -1; el._mop = -1; el._mDb = null;
          el._inFill.style.width = '0%'; el._inTick.style.left = '0%';
          el._outFill.style.width = '0%'; el._outTick.style.left = '0%';
          el._outDb.nodeValue = '';
        }
        // Showing or hiding a row changes the height left for the parameter list
        // inside this card, which the observer on `.dv-cards` cannot see.
        this._geom = false;
      }
      if (c.hasMeter) {
        if (el._mIn !== c.hasIn) {
          el._mIn = c.hasIn;
          el._rowIn.style.display = c.hasIn ? '' : 'none';
          this._geom = false;
        }
        const pc = (v) => Math.round(v * 100);
        if (c.hasIn) {
          const i = pc(c.inRms), ip = pc(c.inPeak);
          if (el._mi !== i) { el._mi = i; el._inFill.style.width = i + '%'; }
          if (el._mip !== ip) { el._mip = ip; el._inTick.style.left = ip + '%'; }
        }
        const o = pc(c.outRms), op = pc(c.outPeak);
        if (el._mo !== o) { el._mo = o; el._outFill.style.width = o + '%'; }
        if (el._mop !== op) { el._mop = op; el._outTick.style.left = op + '%'; }
        if (el._mDb !== c.meterText) { el._mDb = c.meterText; el._outDb.nodeValue = c.meterText; }
      }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      if (el._byp !== c.bypass) { el._byp = c.bypass; el.classList.toggle('byp', c.bypass); }
      // What reaches this card, and — on the last one — what leaves it. Written
      // as dataset rather than four toggled classes: one string write when it
      // changes, and the stylesheet selects on the attribute.
      if (el._fIn !== c.flowIn) { el._fIn = c.flowIn; el.dataset.flowIn = c.flowIn; }
      if (el._fOut !== c.flowOut) { el._fOut = c.flowOut; el.dataset.flowOut = c.flowOut; }
      // Keyed on the number rather than on the string it renders to. The old
      // `dataset.pos !== String(c.pos)` built one string per card per frame —
      // and then a second one to write it — only ever to conclude that the
      // position had not changed. The attribute itself still has to be there:
      // `_down` reads it to turn a click into a chain position.
      if (el._pos !== c.pos) { el._pos = c.pos; el.dataset.pos = c.pos; }
      /*
       * ...and its DEVICE ID, as an attribute.
       *
       * `_devId` is already on the element for the handlers, but a selector cannot read a
       * property. Every command names a device by ID and positions shift under a reorder,
       * so a test — or anything else selecting a card — must be able to ask for the card
       * that is device 3 rather than for the third card.
       */
      if (el.dataset.dev !== String(c.id)) el.dataset.dev = String(c.id);
    }
    // An edit in flight ages, and nothing else on this page is going to redraw
    // for that. Keep our own frame coming until the last one has settled.
    if (this.edits && this.edits.count) this._frame();
  }

  /**
   * A card's parameter rows: name, a bar, the host's own display string.
   *
   * Virtualised — see the header. The pool holds only what fits plus one, and is
   * indexed as a ring so a one-row scroll rebinds one row. Rows are hidden and
   * never removed, like everything else here.
   */
  _params(el, c) {
    /*
     * THE SAMPLE VIEW REPLACES THE LIST, and only when it has something to draw.
     *
     * `_paintWave` returns false while the window is still in flight, and the list stays up
     * until it lands — a blank canvas where a waveform should be is indistinguishable from a
     * file that decoded to silence, and the pad list is a better thing to be looking at while
     * you wait than an empty box.
     *
     * The rows are hidden rather than skipped: `_params` below still owns them, and leaving
     * them visible under a canvas would put a second thing in the same space.
     */
    const wantWave = c.defaultView === 1 && !!c.sample;
    const painted = wantWave ? this._paintWave(el, c) : false;
    if (el._waveOn !== painted) {
      el._waveOn = painted;
      el._wave.style.display = painted ? '' : 'none';
      el._list.style.display = painted ? 'none' : '';
    }
    if (painted) return;

    const rows = el._rows;
    const n = c.paramCount;
    if (!n) {
      for (let i = 0; i < rows.length; i++) {
        if (rows[i].style.display !== 'none') rows[i].style.display = 'none';
      }
      if (el._spaceH !== 0) { el._spaceH = 0; el._space.style.height = '0px'; }
      return;
    }
    // One row has to exist before anything can be measured from one.
    if (!rows.length) this._grow(el, 1);
    if (!this._geom) this._measure(el);
    // Nothing measurable — a zero-height card, which means the strip is not on
    // screen. Draw no rows rather than divide by it.
    if (!(this._rowH > 0)) return;
    const per = this._geom
      ? Math.ceil(this._listH / this._rowH) + 1
      : rows.length;
    this._grow(el, Math.min(per, n));

    const H = n * this._rowH;
    if (el._spaceH !== H) { el._spaceH = H; el._space.style.height = H + 'px'; }

    const size = Math.min(rows.length, n);
    // Clamped so the last window is full rather than half empty, and so a
    // scrollTop left over from a longer list cannot point past the end.
    let first = Math.floor(el._scrollTop / this._rowH);
    if (first > n - size) first = n - size;
    if (first < 0) first = 0;

    for (let i = 0; i < size; i++) {
      const idx = first + i;
      // Ring, not list: the slot is decided by the row it holds, so scrolling
      // one row changes one slot's contents instead of every slot's identity.
      this._bind(rows[idx % size], c, idx, el._devId);
    }
    for (let s = size; s < rows.length; s++) {
      if (rows[s].style.display !== 'none') rows[s].style.display = 'none';
    }
  }

  _grow(el, want) {
    const rows = el._rows;
    while (rows.length < want) {
      const r = div('dv-p', el._space);
      const nm = div('dv-p-n', r); nm.appendChild(document.createTextNode(''));
      const bar = div('dv-p-bar', r);
      const fill = div('dv-p-fill', bar);
      const v = div('dv-p-v', r); v.appendChild(document.createTextNode(''));
      /*
       * MAP — the modulation badge, and the only control on a parameter row.
       *
       * Lit when something moves this parameter, dim when nothing does, and clickable
       * either way: dim maps it to the track's macro, lit unmaps it. One target for both
       * directions, because "map this" and "stop mapping this" are the same question
       * asked of the same thing, and a separate unmap affordance would appear and
       * disappear under the pointer as links come and go.
       *
       * `data-map` so the delegated handler can find it, and the LINK ID on the element
       * so unmapping does not have to search the model for which link this row meant.
       */
      const map = div('dv-p-map', r);
      map.appendChild(document.createTextNode('MAP'));
      map.dataset.map = '1';
      r._n = nm.firstChild; r._f = fill; r._v = v.firstChild; r._map = map;
      r._nv = null; r._fv = -1; r._flv = -1; r._vv = null; r._top = -1; r._pi = -1;
      r._uid = ''; r._pend = null; r._pendMilli = -1; r._pendText = '';
      r._mod = -1; r._modInert = null; r._meta = null; r._stepped = null;
      r._mappable = null;
      rows.push(r);
    }
  }

  /**
   * Two layout reads, on a shape change only.
   *
   * The row height and the list height both come from CSS — the card's padding,
   * the head, the font — and a copy of either one here would be a second box
   * model that goes stale silently.
   */
  _measure(el) {
    const r = el._rows[0];
    const rh = r ? r.offsetHeight : 0;
    const lh = el._list.clientHeight;
    if (rh > 0) this._rowH = rh;
    if (lh > 0) this._listH = lh;
    this._geom = this._rowH > 0 && this._listH > 0;
  }

  /**
   * Hand the rack the waveform cache and the request function, after both exist.
   *
   * The same two handles `arrange.bindAudio` takes, wired the same way and for the same reason:
   * they are declared below the `new Chain(...)` that wants them and a `const` does not
   * initialise when it hoists, so passing them to the constructor reads them in the temporal
   * dead zone. That is the failure mode index.html's comment calls the only kind that fails
   * silently.
   */
  bindAudio(cache, request) { this.waveCache = cache; this.requestWave = request; }

  /**
   * PAINT ONE SAMPLER'S AUDIO, with its slices marked.
   *
   * min/max pairs, exactly as the arrangement reads them — one decimated bucket per column, two
   * int16s each — so this and the arrangement cannot come to disagree about what a window means.
   * The whole file is asked for at once at a decimation that fits the card, because a card is
   * 232px and the question is "where in the file is slice 3", which needs the WHOLE file on
   * screen. Scrolling into it is the arrangement's job, not this one's.
   *
   * Returns false when there is nothing yet, so the caller can leave the kit list up rather
   * than swapping to an empty box — a blank canvas where a waveform should be is
   * indistinguishable from a file that decoded to silence.
   */
  _paintWave(el, c) {
    const cv = el._wave;
    const s = c.sample;
    if (!cv) { el._waveWhy = 'no canvas'; return false; }
    if (!s) { el._waveWhy = 'no sample'; return false; }
    if (!s.frames) { el._waveWhy = 'source has no frames'; return false; }
    if (!this.waveCache) { el._waveWhy = 'no wave cache bound'; return false; }
    /*
     * MEASURED FROM WHICHEVER OF THE TWO IS LAID OUT.
     *
     * The canvas starts `display: none`, so its own box is 0x0 — and the first version read that
     * and returned false, which kept it hidden, which kept the box at zero. A deadlock that
     * looked exactly like a waveform window that never arrived: the model was right, the source
     * was resolved, nine slices were ready, and nothing was ever drawn.
     *
     * The list occupies the same space and is up until the swap happens, so it is the honest
     * measure while the canvas is hidden — and the canvas is the honest one afterwards, when the
     * list is the one with no box.
     */
    const own = cv.getBoundingClientRect();
    const alt = el._list ? el._list.getBoundingClientRect() : null;
    const w = Math.round(own.width > 8 ? own.width : (alt ? alt.width : 0));
    const h = Math.round(own.height > 8 ? own.height : (alt ? alt.height : 0));
    if (w < 8 || h < 8) { el._waveWhy = `no box (${w}x${h})`; return false; }
    const dpr = window.devicePixelRatio || 1;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
      cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
      el._waveCtx = null;
    }
    if (!el._waveCtx) {
      el._waveCtx = cv.getContext('2d');
      el._waveCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }
    /*
     * THE DECIMATION IS A POWER OF TWO AND THE REQUEST IS TILED, because that is the contract —
     * the engine builds a PYRAMID, and windows are keyed on (source, decimation, firstFrame)
     * anchored to a multiple of `dec * WAVE_TILE_COLS`.
     *
     * My first version asked for `ceil(frames / width)` columns starting at 0, which is neither:
     * a decimation of 1680 is not a level the pyramid has and 0 is only accidentally a tile
     * boundary. Nothing ever landed under the key it then looked for, so the view stayed empty
     * with the model perfectly correct — the failure of an invented contract, which looks
     * exactly like the failure of a broken one.
     */
    const framesPerPx = s.frames / w;
    let dec = 1;
    while (dec * 2 <= framesPerPx) dec *= 2;
    const tileFrames = dec * WAVE_TILE_COLS;

    const ctx = el._waveCtx;
    ctx.clearRect(0, 0, w, h);
    const style = getComputedStyle(cv);
    const waveFill = style.getPropertyValue('--wave') || '#7aa2f7';
    let drewAny = false;
    let missing = false;
    for (let tileFirst = 0; tileFirst < s.frames; tileFirst += tileFrames) {
      const key = `${s.source}:${dec}:${tileFirst}`;
      const win = this.waveCache.get(key);
      if (!win) {
        missing = true;
        if (this.requestWave) {
          this.requestWave(s.source, dec, tileFirst, WAVE_TILE_COLS, WAVE_MASK_BOTH,
                           { track: s.track, device: s.device });
        }
        continue;
      }
      const ch = win.channels > 0 ? win.channels : 1;
      const cols = win.columns;
      const bandH = h / ch;
      // The device pixels this tile covers, and no others: walking the whole card per tile would
      // be O(width x tiles) to draw the same picture.
      const x0 = Math.max(0, Math.floor((tileFirst / s.frames) * w));
      const x1 = Math.min(w, Math.ceil(((tileFirst + cols * dec) / s.frames) * w));
      ctx.fillStyle = waveFill;
      for (let cIdx = 0; cIdx < ch; cIdx++) {
        const base = cIdx * cols * 2;
        const mid = cIdx * bandH + bandH / 2;
        const half = bandH / 2;
        for (let x = x0; x < x1; x++) {
          const f0 = (x / w) * s.frames;
          let i0 = Math.floor((f0 - tileFirst) / dec);
          let i1 = Math.ceil((f0 + framesPerPx - tileFirst) / dec);
          if (i0 < 0) i0 = 0;
          if (i1 > cols) i1 = cols;
          if (i1 <= i0) continue;
          let lo = 32767, hi = -32768;
          for (let i = i0; i < i1; i++) {
            const mn = win.pairs[base + i * 2], mx = win.pairs[base + i * 2 + 1];
            if (mn < lo) lo = mn;
            if (mx > hi) hi = mx;
          }
          const yt = mid - (hi / 32768) * half;
          const yb = mid - (lo / 32768) * half;
          // A MINIMUM OF ONE PIXEL, the same rule the arrangement states: min === max wherever
          // the material is constant, and a zero-height rectangle draws nothing, so silence and
          // DC would leave the band empty rather than flat.
          ctx.fillRect(x, Math.round(yt), 1, Math.max(1, Math.round(yb) - Math.round(yt)));
          drewAny = true;
        }
      }
    }
    // NOTHING YET IS NOT A PICTURE. A partly-arrived file is worth showing — it fills in — but a
    // canvas with no audio on it at all is what the caller must not swap the pad list for.
    if (!drewAny) { el._waveWhy = missing ? 'windows not arrived' : 'window empty'; return false; }
    el._waveWhy = '';
    /*
     * THE SLICE BOUNDARIES, which are the reason this view exists.
     *
     * A line at every slice's START, and the LAST slice's end, so N slices draw N+1 boundaries
     * and the picture reads as regions rather than as marks. Drawn after the waveform so a
     * boundary is never hidden by a loud transient sitting on it.
     */
    if (s.cuts.length) {
      ctx.fillStyle = style.getPropertyValue('--cut') || '#e0af68';
      for (const [b0] of s.cuts) {
        const x = Math.round((b0 / s.frames) * w);
        ctx.fillRect(Math.min(w - 1, x), 0, 1, h);
      }
      const last = s.cuts[s.cuts.length - 1][1];
      ctx.fillRect(Math.min(w - 1, Math.round((last / s.frames) * w)), 0, 1, h);
    }
    return true;
  }

  _bind(r, c, idx, device) {
    const q = c.params[idx];
    if (r.style.display !== '') r.style.display = '';
    if (r._top !== idx) { r._top = idx; r.style.top = (idx * this._rowH) + 'px'; }
    r._pi = q.index;
    r._uid = q.uid;
    if (r._nv !== q.name) { r._nv = q.name; r._n.nodeValue = q.name; }
    /*
     * WHAT THIS PARAMETER IS, in the row's title — the range as the plugin renders it, the unit,
     * whether it is a switch, and whether automation would be ignored.
     *
     * In the TITLE rather than on the row because there is no room for it at 17px and it is a
     * thing you ask about one parameter at a time. Before this the rack drew every parameter as
     * an anonymous 0..1 bar, so setting a value in real units meant dragging and reading the
     * display string until it said the right thing — a binary search rather than an interface.
     */
    const meta = q.range || q.steps || (!q.automatable && !q.isSlot)
      ? [q.range, q.steps ? `${q.steps} positions` : '',
         // A SLOT is not an un-automatable parameter, it is not a parameter. Saying "not
         // automatable" about one reads as a limitation of this slot rather than a category
         // error — see `isSlot` in chainmodel.js, which exists because one flag had two
         // consumers wanting different things.
         (q.automatable || q.isSlot) ? '' : 'not automatable'].filter(Boolean).join(' · ')
      : '';
    if (r._meta !== meta) { r._meta = meta; r.title = meta; }
    /*
     * A SWITCH IS DRAWN AS A SWITCH. `steps` non-zero means the value can only take that many
     * positions, and a continuous bar over it says the values in between are reachable — which
     * they are not. The class puts notches on the fill; the bar still drags, because a switch you
     * can only set from the console is a switch nobody sets.
     */
    // Marks the row as a pad rather than a parameter — read by the drag guard, and by the
    // stylesheet so a slice span can be drawn differently from a value.
    if (r._isSlot !== !!q.isSlot) { r._isSlot = !!q.isSlot; r.classList.toggle('slot', !!q.isSlot); }
    const stepped = q.steps > 0 && q.steps <= 16;
    if (r._stepped !== stepped) {
      r._stepped = stepped;
      r.classList.toggle('stepped', stepped);
      // The positions as a CSS variable, so the notches are drawn by the stylesheet rather than
      // by this file inventing geometry the theme cannot change.
      if (stepped) r.style.setProperty('--steps', String(q.steps));
      else r.style.removeProperty('--steps');
    }

    // An edit in flight wins over the published value, and says that it is one.
    // The plugin's display string is NOT reused while it does: "620 Hz" beside a
    // bar that has moved somewhere else is the most confident kind of wrong.
    const ed = this.edits ? findParamEdit(this.edits, device, q.index) : null;
    const milli = ed ? ed.milli : q.milli;
    let shown;
    if (ed) {
      if (r._pendMilli !== milli) {
        r._pendMilli = milli;
        r._pendText = (milli / 1000).toFixed(3);
      }
      shown = r._pendText;
    } else {
      shown = q.display;
    }
    if (r._vv !== shown) { r._vv = shown; r._v.nodeValue = shown; }
    const w = Math.round(milli / 10);
    if (r._fv !== w) { r._fv = w; r._f.style.width = w + '%'; }
    /*
     * WHERE THE FILL STARTS. Zero for a parameter, the slice's offset for a slot.
     *
     * `margin-left` rather than absolute positioning: both percentages then resolve against the
     * same box, so the pair is a span in one coordinate system and the stylesheet keeps owning
     * the bar's geometry. Guarded like everything else here — this runs per row per draw.
     */
    const left = Math.round((q.spanLeft || 0) / 10);
    if (r._flv !== left) { r._flv = left; r._f.style.marginLeft = left + '%'; }
    const pend = !!ed;
    if (r._pend !== pend) { r._pend = pend; r.classList.toggle('pend', pend); }

    /*
     * The MAP badge. Guarded on the LINK ID, not on a boolean, so re-mapping a row to a
     * different link still repaints — and so the id on the element is always the id the
     * badge is showing, which is what the click handler reads.
     */
    /*
     * NO MAP ON A PARAMETER THE PLUGIN WILL IGNORE.
     *
     * `automatable: false` (v30) means the plugin does not accept a value from a host for this —
     * so a link pointed here is accepted by the engine, published, drawable, and moves nothing.
     * That is the same class of lie the badge's third state exists to expose, and the cheapest
     * fix is not to offer it: the badge is hidden rather than shown-and-refusing, because a
     * control that says no is still a control somebody presses twice.
     *
     * The TITLE says why, on the row, so the absence is explained rather than merely present.
     */
    const mappable = q.automatable !== false;
    if (r._mappable !== mappable) {
      r._mappable = mappable;
      r._map.style.display = mappable ? '' : 'none';
    }
    if (r._mod !== q.mod || r._modInert !== q.modInert) {
      r._mod = q.mod; r._modInert = q.modInert;
      r._map.classList.toggle('on', q.mod !== 0 && !q.modInert);
      // A THIRD state, because there are three: nothing here, something that works, and
      // something that exists and cannot work. Folding the last into either of the others
      // is the lie — into `on` it claims a modulation that moves nothing, into `off` it
      // hides a link you cannot remove because you cannot see it.
      r._map.classList.toggle('inert', q.mod !== 0 && q.modInert);
      if (q.mod) r._map.dataset.link = String(q.mod);
      else delete r._map.dataset.link;
      // The depth, in the title rather than on the row: at this size there is no room for
      // a second number, and "how much" is a thing you ask about one parameter at a time.
      r._map.title = !q.mod
        ? 'click to modulate this from the macro'
        : q.modInert
          ? 'linked but NOT WORKING — the link names no parameter, so it moves nothing. '
            + 'Click to remove it.'
          : `modulated · depth ${q.modDepth.toFixed(2)} — click to unmap`;
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    const pending = [];
    if (this.edits) {
      for (let i = 0; i < this.edits.count; i++) {
        const s = this.edits.slots[i];
        pending.push({ device: s.device, index: s.index, valueMilli: s.milli });
      }
    }
    return {
      track: vm.track,
      known: vm.known,
      version: vm.version,
      cards: vm.cardCount,
      notice: vm.notice,
      // What the strip itself last said, as opposed to what the model says about
      // the chain. A test asserting on a refusal has to be able to see it.
      said: this._said,
      // The affordance is unconditional: a chain the engine has not published is
      // still a chain a device can be appended to, and hiding the control would
      // make "we have not read it yet" look like "you may not add one".
      canAdd: !!this.addEl,
      titles: vm.cards.slice(0, vm.cardCount).map((c) => c.title),
      named: vm.cards.slice(0, vm.cardCount).filter((c) => c.named).length,
      params: vm.cards.slice(0, vm.cardCount).map((c) => c.paramCount),
      // How many of those are ON SCREEN, and where each card's list is scrolled
      // to. `params` counts what the device HAS; these two are what a person can
      // currently see and how far they can get to the rest.
      rows: this.pool.slice(0, vm.cardCount).map((el) => el._rows.length),
      scroll: this.pool.slice(0, vm.cardCount).map((el) => el._scrollTop),
      extent: this.pool.slice(0, vm.cardCount).map((el) => el._spaceH),
      rowHeight: this._rowH,
      /*
       * THE SAMPLE VIEW, per card: which view the device remembers, whether this renderer
       * actually PAINTED one, and what it was asked to draw.
       *
       * Three separate facts because they fail separately — the setting can be 1 while the
       * source has not resolved, and the source can be there while the waveform window is still
       * in flight — and a probe that reported one number would send a reader to the wrong layer.
       * That is exactly what happened writing the check for this: `samplerKitCached(t, 0)` came
       * back null because the card keys the kit on the device's REAL id and 0 is only a wildcard
       * for COMMANDS, so the state looked absent when it was merely somewhere else.
       */
      views: vm.cards.slice(0, vm.cardCount).map((c) => c.defaultView),
      // WHY a card is not painting, in the terms that tell the causes apart: no cache bound, no
      // box to draw in, or a window that has not arrived. One boolean would send a reader to the
      // wrong layer, which is what the first three attempts at this feature each did.
      waveWhy: this.pool.slice(0, vm.cardCount).map((el) => el._waveWhy || ''),
      painted: this.pool.slice(0, vm.cardCount).map((el) => !!el._waveOn),
      samples: vm.cards.slice(0, vm.cardCount).map(
        (c) => (c.sample ? { source: c.sample.source, frames: c.sample.frames,
                             cuts: c.sample.cuts.length } : null)),
      /*
       * WHAT THE METERS WERE DRAWN FROM, per card, as the fractions the renderer
       * actually wrote — not as the engine's current numbers.
       *
       * The distinction is the whole reason this is here. A test comparing the DOM
       * against `deviceMeters()` is comparing two different instants: the DOM was
       * written on the last draw and the store has moved on since, by up to a frame
       * — and a percussive signal swings a long way in a frame. That check failed
       * at 28 percentage points of disagreement with no bug present, which is what
       * a badly-specified comparison looks like.
       *
       * These are the model's own values, so a test can assert model -> DOM
       * exactly, and assert engine -> model separately against the published
       * region. Two exact checks beat one with a tolerance wide enough to hide the
       * absence of a comparison.
       */
      meters: vm.cards.slice(0, vm.cardCount).map((c) => (c.hasMeter ? {
        device: c.id,
        outRms: Math.round(c.outRms * 100),
        outPeak: Math.round(c.outPeak * 100),
        inRms: Math.round(c.inRms * 100),
        hasIn: c.hasIn,
        text: c.meterText,
      } : null)),
      // The measured height of the parameter list. Reported because it is the
      // value that went stale when the strip was resized, and a stale scalar is
      // invisible to every other assertion.
      listHeight: this._listH,
      // The first and last parameter each card currently has a row for, by the
      // engine's own index — the honest answer to "can I reach number 255".
      shown: this.pool.slice(0, vm.cardCount).map((el) => {
        let lo = Infinity, hi = -1;
        for (const r of el._rows) {
          if (r.style.display === 'none') continue;
          if (r._pi < lo) lo = r._pi;
          if (r._pi > hi) hi = r._pi;
        }
        return hi < 0 ? null : [lo, hi];
      }),
      pending,
      dragging: !!this._drag,
      domNodes: this.host.querySelectorAll('*').length,
    };
  }
}
