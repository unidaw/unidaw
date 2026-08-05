// The cell inspector: a panel that says what is under the cursor, or under the pointer.
//
// Pure view. Every string it draws was decided by inspectmodel.js; this file owns nodes and
// nothing else, which is why the interesting behaviour is testable without a browser.
//
// RENDERING DISCIPLINE, the same as chain.js and browser.js (GUIDELINES 3): rows are pooled
// and hidden rather than removed, every write is guarded by a cached value, and text goes in
// through `.nodeValue` rather than `.textContent` so no node is replaced. The model rebuilds
// its strings only when the cell changes, so a still cursor costs one string comparison a
// frame and nothing else.

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

export class Inspect {
  constructor(host) {
    this.host = host;
    this.host.className = 'insp';

    const head = div('insp-head', host);
    this.labelText = text(div('insp-label', head));
    this.labelText.nodeValue = 'CELL';
    this.titleText = text(div('insp-title', head));
    this.subText = text(div('insp-sub', host));

    this.listEl = div('insp-list', host);
    this.pool = [];

    // Cached, so a frame where nothing moved writes nothing at all.
    this._title = null;
    this._sub = null;
    this._n = -1;
    this._empty = null;
  }

  /** Grow the pool to `n` rows. Never shrinks; extra rows are hidden. */
  _rows(n) {
    while (this.pool.length < n) {
      const el = div('insp-row', this.listEl);
      el._label = text(div('insp-k', el));
      el._value = text(div('insp-v', el));
      el._l = null; el._v = null; el._kind = null;
      this.pool.push(el);
    }
    return this.pool;
  }

  render(vm) {
    if (!vm) return;
    if (this._title !== vm.title) { this._title = vm.title; this.titleText.nodeValue = vm.title; }
    if (this._sub !== vm.subtitle) { this._sub = vm.subtitle; this.subText.nodeValue = vm.subtitle; }
    if (this._empty !== vm.empty) {
      this._empty = vm.empty;
      this.host.classList.toggle('empty', !!vm.empty);
    }

    const pool = this._rows(vm.count);
    for (let i = 0; i < pool.length; i++) {
      const el = pool[i];
      const on = i < vm.count;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const r = vm.rows[i];
      if (el._l !== r.label) { el._l = r.label; el._label.nodeValue = r.label; }
      if (el._v !== r.value) { el._v = r.value; el._value.nodeValue = r.value; }
      if (el._kind !== r.kind) {
        el._kind = r.kind;
        el.classList.toggle('muted', r.kind === 'muted');
        el.classList.toggle('strong', r.kind === 'strong');
      }
    }
    this._n = vm.count;
  }

  /** What is on screen, for a test that should not read the DOM's shape. */
  probe() {
    const rows = [];
    for (let i = 0; i < this._n; i++) {
      const el = this.pool[i];
      rows.push({ label: el._l, value: el._v, kind: el._kind });
    }
    return { title: this._title, subtitle: this._sub, rows };
  }
}
