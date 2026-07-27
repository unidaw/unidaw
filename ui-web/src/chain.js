// The device chain strip: what is on the cursor's track, in order.
//
// Same rules as every other renderer here (GUIDELINES 3): pooled elements that
// are hidden rather than removed, every style write guarded by a cached value,
// `.nodeValue` rather than `.textContent`, and nothing allocated when nothing
// changed. This surface redraws with the rest of the app, so "it is only a few
// cards" is not a reason to skip the discipline — it is how the discipline
// stops being true everywhere.

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

export class Chain {
  /**
   * @param {HTMLElement} host
   * @param {{onSelect?: (pos:number) => void, onAdd?: (track:number) => void}} opts
   *
   * `onAdd` is called with the track the strip is CURRENTLY showing, not with
   * whatever the caller thinks the cursor is on — the two differ for a frame
   * whenever the cursor moves, and a device added to the previous track is the
   * kind of wrong that looks like nothing happened.
   */
  constructor(host, { onSelect, onAdd } = {}) {
    this.host = host;
    this.host.className = 'dv';
    this.onSelect = onSelect;
    this.onAdd = onAdd;

    const head = div('dv-head', host);
    this.label = text(div('dv-label', head));
    this.track = text(div('dv-track', head));
    this.version = text(div('dv-version', head));
    this.notice = text(div('dv-notice', head));

    this.cardsEl = div('dv-cards', host);
    this.pool = [];
    this.vm = null;
    this._track = null; this._version = null; this._notice = null;

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
    this.cardsEl.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.dv-add')) {
        if (this.onAdd) this.onAdd(this.vm ? this.vm.track : -1);
        return;
      }
      const card = e.target.closest('.dv-card');
      if (card && this.onSelect) this.onSelect(Number(card.dataset.pos));
    });
  }

  _card(n) {
    while (this.pool.length < n) {
      const el = div('dv-card', this.cardsEl);
      const top = div('dv-card-top', el);
      const badge = div('dv-badge', top);
      const title = div('dv-title', top);
      const body = div('dv-body', el);
      el._rows = [];
      const foot = div('dv-foot', el);
      el._footEl = foot;
      el._badge = text(badge);
      el._title = text(title);
      el._body = text(body);
      el._foot = text(foot);
      el._b = null; el._t = null; el._y = null; el._f = null;
      el._sel = null; el._byp = null;
      this.pool.push(el);
    }
    return this.pool;
  }

  render(vm) {
    this.vm = vm;
    if (this._notice !== vm.notice) { this._notice = vm.notice; this.notice.nodeValue = vm.notice; }
    if (this._track !== vm.trackName) {
      this._track = vm.trackName;
      this.track.nodeValue = vm.trackName;
    }
    // "chainVersion 87" when there is one, and nothing at all when the engine
    // has not published a chain — a version of -1 on screen is worse than no
    // version, because it looks like a number somebody could act on.
    const ver = vm.known ? 'chainVersion ' + vm.version : '';
    if (this._version !== ver) { this._version = ver; this.version.nodeValue = ver; }
    if (this.label.nodeValue !== 'DEVICE CHAIN') this.label.nodeValue = 'DEVICE CHAIN';

    const pool = this._card(vm.cardCount);
    for (let i = 0; i < pool.length; i++) {
      const el = pool[i];
      const on = i < vm.cardCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const c = vm.cards[i];
      if (el._b !== c.badge) { el._b = c.badge; el._badge.nodeValue = c.badge; }
      if (el._t !== c.title) { el._t = c.title; el._title.nodeValue = c.title; }
      const body = c.caps || 'no declared capabilities';
      if (el._y !== body) { el._y = body; el._body.nodeValue = body; }
      this._params(el, c);
      if (el._f !== c.sub) { el._f = c.sub; el._foot.nodeValue = c.sub; }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      if (el._byp !== c.bypass) { el._byp = c.bypass; el.classList.toggle('byp', c.bypass); }
      if (el.dataset.pos !== String(c.pos)) el.dataset.pos = String(c.pos);
    }
  }

  /**
   * A card's parameter rows: name, a bar, the host's own display string.
   *
   * Pooled per card and hidden rather than removed, like everything else here —
   * a card's parameter count changes when its host answers, and rebuilding the
   * rows on that transition is a DOM churn on every rack you look at.
   */
  _params(el, c) {
    const rows = el._rows;
    while (rows.length < c.paramCount) {
      // NOT div('dv-p', el): that appends first, so the insertBefore below became
      // insertBefore(r, r) — a no-op that left every parameter row underneath the
      // footer. Build it detached and place it once.
      const r = div('dv-p');
      const n = div('dv-p-n', r); n.appendChild(document.createTextNode(''));
      const bar = div('dv-p-bar', r);
      const fill = div('dv-p-fill', bar);
      const v = div('dv-p-v', r); v.appendChild(document.createTextNode(''));
      r._n = n.firstChild; r._f = fill; r._v = v.firstChild;
      r._nv = null; r._fv = null; r._vv = null;
      rows.push(r);
      // Above the footer, which was created before any of these existed.
      el.insertBefore(r, el._footEl);
    }
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i];
      const on = i < c.paramCount;
      const disp = on ? '' : 'none';
      if (r.style.display !== disp) r.style.display = disp;
      if (!on) continue;
      const q = c.params[i];
      if (r._nv !== q.name) { r._nv = q.name; r._n.nodeValue = q.name; }
      if (r._vv !== q.display) { r._vv = q.display; r._v.nodeValue = q.display; }
      const w = Math.round(q.frac * 100);
      if (r._fv !== w) { r._fv = w; r._f.style.width = w + '%'; }
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      track: vm.track,
      known: vm.known,
      version: vm.version,
      cards: vm.cardCount,
      notice: vm.notice,
      // The affordance is unconditional: a chain the engine has not published is
      // still a chain a device can be appended to, and hiding the control would
      // make "we have not read it yet" look like "you may not add one".
      canAdd: !!this.addEl,
      titles: vm.cards.slice(0, vm.cardCount).map((c) => c.title),
      named: vm.cards.slice(0, vm.cardCount).filter((c) => c.named).length,
      params: vm.cards.slice(0, vm.cardCount).map((c) => c.paramCount),
      domNodes: this.host.querySelectorAll('*').length,
    };
  }
}
