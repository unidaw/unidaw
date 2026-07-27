// The harmony card: what key the playhead is in, and what the engine does not
// say about tuning.
//
// Same rules as every other renderer here (GUIDELINES 3): pooled rows that are
// hidden rather than removed, every write guarded by a cached value, `.nodeValue`
// rather than `.textContent`, and nothing allocated when nothing changed. It
// redraws with the rest of the app, so the fact that it is one small card is not
// a reason to skip the discipline — it is how the discipline stops being true
// everywhere.

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

/** Fixed labels, written once at construction and never touched again. */
function fixed(cls, parent, value) {
  text(div(cls, parent)).nodeValue = value;
}

const CARET_OPEN = 'ph ph-caret-down';
const CARET_SHUT = 'ph ph-caret-right';
const ARROW = '→';

export class Harmony {
  constructor(host, { onToggle, onSelect } = {}) {
    this.host = host;
    this.host.className = 'hm';
    this.onToggle = onToggle;
    this.onSelect = onSelect;

    const head = div('hm-head', host);
    const title = div('hm-title', head);
    this.caret = document.createElement('i');
    this.caret.className = CARET_OPEN;
    title.appendChild(this.caret);
    // A caret that collapses nothing is a control that silently ignores you
    // (GUIDELINES 4.5), so it exists only when the host gave the click somewhere
    // to go. Set once: this is not a per-draw decision. The chip is outside the
    // click target, because a readout that toggles the card is a surprise.
    if (!onToggle) this.caret.style.display = 'none';
    else { title.classList.add('act'); title.addEventListener('pointerdown', () => this.onToggle()); }
    fixed('hm-label', title, 'HARMONY · TUNING');
    // A readout, not a selector. The engine publishes no TET to choose, so this
    // is deliberately styled as the design's INACTIVE chip: it says what is true
    // of the engine's scales, and the tuning line below says why it is the only
    // one. Its title carries that line for anyone hovering it.
    const chips = div('hm-chips', head);
    this.tet = text(div('hm-chip', chips));
    this.chipEl = chips.firstChild;

    const body = div('hm-body', host);
    this.body = body;

    const key = div('hm-key', body);
    this.name = text(div('hm-name', key));
    this.since = text(div('hm-since', key));
    this.version = text(div('hm-version', key));

    const map = div('hm-map', body);
    this.at = text(div('hm-at', map));
    fixed('hm-arrow', map, ARROW);
    this.pos = text(div('hm-pos', map));
    // The second arrow belongs to the raw fields, so it goes when they do — an
    // arrow pointing at nothing reads as a value that failed to render.
    this.arrowEl = div('hm-arrow', map);
    text(this.arrowEl).nodeValue = ARROW;
    this.fields = text(div('hm-fields', map));

    this.rowsEl = div('hm-rows', body);
    if (onSelect) this.rowsEl.classList.add('act');
    this.moreEl = div('hm-more', body);
    this.more = text(this.moreEl);
    this.noteEl = div('hm-note', body);
    this.note = text(this.noteEl);

    const tune = div('hm-tune', body);
    fixed('hm-tune-label', tune, 'TUNING');
    this.tuneText = text(div('hm-tune-text', tune));

    this.pool = [];
    this.vm = null;
    this._key = null; this._since = null; this._ver = null; this._at = null;
    this._pos = null; this._fields = null; this._more = null; this._note = null;
    this._tune = null; this._tet = null; this._open = null;

    // One listener on the container rather than one per row: rows come and go
    // with the pool, and a listener per pooled element is a listener rebound
    // every time the pool grows.
    if (onSelect) {
      this.rowsEl.addEventListener('pointerdown', (e) => {
        const row = e.target.closest('.hm-row');
        if (row) this.onSelect(row._i, row._tick);
      });
    }
  }

  _row(n) {
    while (this.pool.length < n) {
      const el = div('hm-row', this.rowsEl);
      el._num = text(div('hm-num', el));
      el._name = text(div('hm-rname', el));
      el._at = text(div('hm-rat', el));
      el._n = null; el._t = null; el._a = null; el._cur = null;
      el._i = -1; el._tick = 0;
      this.pool.push(el);
    }
    return this.pool;
  }

  render(vm) {
    this.vm = vm;
    if (this._open !== vm.open) {
      this._open = vm.open;
      this.body.style.display = vm.open ? '' : 'none';
      this.caret.className = vm.open ? CARET_OPEN : CARET_SHUT;
    }
    if (this._tet !== vm.tuning) {
      this._tet = vm.tuning;
      this.tet.nodeValue = vm.tuning;
      this.chipEl.title = vm.tuningNotice;
    }
    if (this._tune !== vm.tuningNotice) { this._tune = vm.tuningNotice; this.tuneText.nodeValue = vm.tuningNotice; }
    if (this._key !== vm.key) { this._key = vm.key; this.name.nodeValue = vm.key; }
    if (this._since !== vm.since) { this._since = vm.since; this.since.nodeValue = vm.since; }
    if (this._ver !== vm.versionText) { this._ver = vm.versionText; this.version.nodeValue = vm.versionText; }
    if (this._at !== vm.at) { this._at = vm.at; this.at.nodeValue = vm.at; }
    if (this._pos !== vm.pos) { this._pos = vm.pos; this.pos.nodeValue = vm.pos; }
    if (this._fields !== vm.fields) {
      this._fields = vm.fields;
      this.fields.nodeValue = vm.fields;
      this.arrowEl.style.display = vm.fields ? '' : 'none';
    }
    if (this._more !== vm.more) {
      this._more = vm.more;
      this.more.nodeValue = vm.more;
      this.moreEl.style.display = vm.more ? '' : 'none';
    }
    if (this._note !== vm.notice) {
      this._note = vm.notice;
      this.note.nodeValue = vm.notice;
      this.noteEl.style.display = vm.notice ? '' : 'none';
    }

    const pool = this._row(vm.rowCount);
    for (let i = 0; i < pool.length; i++) {
      const el = pool[i];
      const on = i < vm.rowCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const r = vm.rows[i];
      if (el._n !== r.num) { el._n = r.num; el._num.nodeValue = r.num; }
      if (el._t !== r.name) { el._t = r.name; el._name.nodeValue = r.name; }
      if (el._a !== r.at) { el._a = r.at; el._at.nodeValue = r.at; }
      if (el._cur !== r.current) { el._cur = r.current; el.classList.toggle('cur', r.current); }
      // The event index is the row's identity, so it is both the numeric cache
      // the click handler reads and the attribute an agent asserts on.
      if (el._i !== r.index) { el._i = r.index; el.dataset.event = String(r.index); }
      el._tick = r.tick;
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      key: vm.key,
      root: vm.root,
      scaleId: vm.scaleId,
      index: vm.index,
      count: vm.count,
      known: vm.known,
      version: vm.version,
      open: vm.open,
      at: vm.at,
      pos: vm.pos,
      fields: vm.fields,
      since: vm.since,
      notice: vm.notice,
      more: vm.more,
      // The whole point of the card. `tuningKnown: false` is what a test asserts
      // on so the honesty cannot be quietly dropped, exactly as the mixer's
      // `authoritative: false` was until the read-back landed.
      tuning: vm.tuning,
      tuningKnown: vm.tuningKnown,
      tuningNotice: vm.tuningNotice,
      rowFirst: vm.rowFirst,
      rows: vm.rows.slice(0, vm.rowCount).map((r) => ({
        index: r.index, name: r.name, at: r.at, current: r.current,
      })),
      domNodes: this.host.querySelectorAll('*').length,
    };
  }
}
