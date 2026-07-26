// Browser rail: projects on disk, and the scale in force.
//
// A rail rather than a view — it sits over whichever surface is showing, because
// opening a project is something you do FROM somewhere, not instead of it.
//
// The project list does not come from the engine. The engine publishes no index
// and a browser cannot read a filesystem, so the sidecar answers `{"type":"list"}`
// with names only. Names, not paths: handing the client a path invites it to send
// one back, and the engine resolves names against its own project directory.

import { createField, begin as fieldBegin, cancel as fieldCancel,
         feed as fieldFeed, display } from './textfield.js';

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

    // Save-as. An inline field rather than a dialog: the rail already owns the
    // keyboard while it is open, so a second focus owner would be one too many.
    this.saveRow = div('br-save', host);
    this.savePrompt = div('br-saveprompt', this.saveRow);
    this.savePrompt.appendChild(document.createTextNode('save as'));
    this.saveName = div('br-savename', this.saveRow);
    this.saveName.appendChild(document.createTextNode(''));
    // Names go to the filesystem, so the charset matches what the sidecar
    // accepts — a name cannot be typed here and refused two hops away.
    this.field = createField({ charset: /[A-Za-z0-9._-]/, max: 28 });

    const foot = div('br-foot', host);
    this.hint = div('br-hint', foot);
    this.hint.appendChild(document.createTextNode(''));

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

  /** Begin save-as, seeded with the loaded project's name. */
  beginSave(seed) { fieldBegin(this.field, seed || ''); }
  cancelSave() { fieldCancel(this.field); }

  /**
   * Feed a keystroke to the save field. One shared implementation with the
   * cell buffer and the rename field — see src/textfield.js for why.
   */
  feedSave(key) {
    const act = fieldFeed(this.field, key);
    if (act === 'commit') {
      const name = this.field.text.trim();
      fieldCancel(this.field);
      if (name) this.onSave && this.onSave(name);
    }
    return act;
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
    if (this._saving !== this.field.active) {
      this._saving = this.field.active;
      this.saveRow.classList.toggle('on', this.field.active);
    }
    const shown = display(this.field);
    if (this._saveText !== shown) { this._saveText = shown; this.saveName.firstChild.nodeValue = shown; }
    const hint = this.field.active ? 'Enter saves · Esc cancels' : 'Enter opens · S saves as · B closes';
    if (this._hint !== hint) { this._hint = hint; this.hint.firstChild.nodeValue = hint; }
  }

  probe() {
    return { items: this.items.slice(), selected: this.selected,
             current: this.current, saving: this.field.active, saveText: this.field.text,
             domNodes: this.pool.length };
  }
}
