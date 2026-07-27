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
  constructor(host, { onSelect } = {}) {
    this.host = host;
    this.host.className = 'dv';
    this.onSelect = onSelect;

    const head = div('dv-head', host);
    this.label = text(div('dv-label', head));
    this.track = text(div('dv-track', head));
    this.version = text(div('dv-version', head));
    this.notice = text(div('dv-notice', head));

    this.cardsEl = div('dv-cards', host);
    this.pool = [];
    this.vm = null;
    this._track = null; this._version = null; this._notice = null;

    // One listener on the container rather than one per card: cards come and go
    // with the pool, and a listener per pooled element is a listener rebound
    // every time the pool grows.
    this.cardsEl.addEventListener('pointerdown', (e) => {
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
      const foot = div('dv-foot', el);
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
      if (el._f !== c.sub) { el._f = c.sub; el._foot.nodeValue = c.sub; }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      if (el._byp !== c.bypass) { el._byp = c.bypass; el.classList.toggle('byp', c.bypass); }
      if (el.dataset.pos !== String(c.pos)) el.dataset.pos = String(c.pos);
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
      titles: vm.cards.slice(0, vm.cardCount).map((c) => c.title),
      domNodes: this.host.querySelectorAll('*').length,
    };
  }
}
