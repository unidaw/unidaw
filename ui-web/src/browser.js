// Browser rail: projects on disk, and the scale in force.
//
// A rail rather than a view — it sits over whichever surface is showing, because
// opening a project is something you do FROM somewhere, not instead of it.
//
// The project list does not come from the engine. The engine publishes no index
// and a browser cannot read a filesystem, so the sidecar answers `{"type":"list"}`
// with names only. Names, not paths: handing the client a path invites it to send
// one back, and the engine resolves names against its own project directory.

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

export class Browser {
  constructor(host, { onOpen, onSave } = {}) {
    this.host = host;
    this.host.className = 'br';
    this.onOpen = onOpen; this.onSave = onSave;

    const head = div('br-head', host);
    const title = div('br-title', head);
    title.appendChild(document.createTextNode('PROJECTS'));
    this.count = div('br-count', head);
    this.count.appendChild(document.createTextNode(''));

    this.listEl = div('br-list', host);
    this.emptyEl = div('br-empty', host);
    this.emptyEl.appendChild(document.createTextNode('no projects found'));

    const foot = div('br-foot', host);
    this.hint = div('br-hint', foot);
    this.hint.appendChild(document.createTextNode('B closes · Enter opens'));

    this.pool = [];
    this.items = [];
    this.selected = 0;
    this.current = '';

    this.listEl.addEventListener('pointerdown', (e) => {
      const row = e.target.closest('.br-item');
      if (!row) return;
      this.selected = Number(row.dataset.index);
      this.onOpen && this.onOpen(this.items[this.selected]);
    });
  }

  setItems(names, current) {
    this.items = names || [];
    this.current = current || '';
    if (this.selected >= this.items.length) this.selected = Math.max(0, this.items.length - 1);
  }

  move(delta) {
    if (!this.items.length) return;
    this.selected = Math.max(0, Math.min(this.items.length - 1, this.selected + delta));
  }

  openSelected() {
    if (!this.items.length) return null;
    const name = this.items[this.selected];
    this.onOpen && this.onOpen(name);
    return name;
  }

  render() {
    const n = this.items.length;
    while (this.pool.length < n) {
      const el = div('br-item', this.listEl);
      el.appendChild(document.createTextNode(''));
      el.dataset.index = String(this.pool.length);
      el._name = null; el._sel = null; el._cur = null;
      this.pool.push(el);
    }
    for (let i = 0; i < this.pool.length; i++) {
      const el = this.pool[i];
      const on = i < n;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const name = this.items[i];
      if (el._name !== name) { el._name = name; el.firstChild.nodeValue = name; }
      const sel = i === this.selected;
      if (el._sel !== sel) { el._sel = sel; el.classList.toggle('sel', sel); }
      const cur = name === this.current;
      if (el._cur !== cur) { el._cur = cur; el.classList.toggle('cur', cur); }
    }
    if (this._n !== n) {
      this._n = n;
      this.count.firstChild.nodeValue = String(n);
      this.emptyEl.style.display = n ? 'none' : '';
    }
  }

  probe() {
    return { items: this.items.slice(), selected: this.selected,
             current: this.current, domNodes: this.pool.length };
  }
}
