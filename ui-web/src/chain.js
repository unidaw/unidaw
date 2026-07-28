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
         reapParamEdits } from './chainmodel.js';

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
  constructor(host, { onSelect, onAdd, onParam, onOpenEditor } = {}) {
    this.host = host;
    this.host.className = 'dv';
    this.onSelect = onSelect;
    this.onOpenEditor = onOpenEditor;
    this.onAdd = onAdd;
    this.onParam = onParam;

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
    // The card box is measured, so a resize invalidates it. Cheap: it only
    // clears a flag, and the next draw takes the two reads.
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
      const list = div('dv-plist', el);
      const space = div('dv-pspace', list);
      list._card = el;
      el._list = list; el._space = space;
      el._rows = [];
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
      open.appendChild(document.createTextNode('open'));
      el.append(open);
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
    const card = e.target.closest('.dv-card');
    if (card && this.onSelect) this.onSelect(Number(card.dataset.pos));
    const bar = e.target.closest('.dv-p-bar');
    if (!bar || !card) return;
    const row = bar.parentNode;
    // A row that has never been bound has no parameter behind it. Refusing here
    // is cheaper than sending index -1 and reading the sidecar's answer to a
    // question nobody meant to ask.
    if (!row || !(row._pi >= 0)) return;
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
        if (el._busEl.style.display !== bd) el._busEl.style.display = bd;
        // Incomplete and truncated are different states and read differently: one
        // resolves in a frame, the other never will.
        el._busEl.classList.toggle('partial', c.busPartial);
        el._busEl.classList.toggle('trunc', c.busTruncated);
      }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      if (el._byp !== c.bypass) { el._byp = c.bypass; el.classList.toggle('byp', c.bypass); }
      // Keyed on the number rather than on the string it renders to. The old
      // `dataset.pos !== String(c.pos)` built one string per card per frame —
      // and then a second one to write it — only ever to conclude that the
      // position had not changed. The attribute itself still has to be there:
      // `_down` reads it to turn a click into a chain position.
      if (el._pos !== c.pos) { el._pos = c.pos; el.dataset.pos = c.pos; }
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
      r._n = nm.firstChild; r._f = fill; r._v = v.firstChild;
      r._nv = null; r._fv = -1; r._vv = null; r._top = -1; r._pi = -1;
      r._uid = ''; r._pend = null; r._pendMilli = -1; r._pendText = '';
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

  _bind(r, c, idx, device) {
    const q = c.params[idx];
    if (r.style.display !== '') r.style.display = '';
    if (r._top !== idx) { r._top = idx; r.style.top = (idx * this._rowH) + 'px'; }
    r._pi = q.index;
    r._uid = q.uid;
    if (r._nv !== q.name) { r._nv = q.name; r._n.nodeValue = q.name; }

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
    const pend = !!ed;
    if (r._pend !== pend) { r._pend = pend; r.classList.toggle('pend', pend); }
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
