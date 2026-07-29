// Patcher renderer: nodes as boxes with ports on their edges, cables as SVG
// curves running port to port.
//
// SVG for the cables because they are curves, and one <svg> holding all of them
// rather than one per edge — a path element is cheap, an svg element is not.
// Nodes stay as divs so they can carry text without SVG's layout rules.
//
// Same rules as every other renderer here (GUIDELINES 3): pooled elements that
// are hidden rather than removed, every style write guarded by a cached value,
// `.nodeValue` rather than `.textContent`, and nothing allocated when nothing
// changed. A node's ports and its box size are rebound only when its TYPE
// changes, because that is the only thing they are computed from — a pooled box
// that starts holding a different node is the one case where they can move.
//
// The box size is written from the view-model rather than set in patcher.css.
// It used to be in both, and they disagreed by 10x22px: cables were aimed at a
// box bigger than the one on screen, so every one of them began past the node's
// right edge and ended in mid-air. One owner for one number.

import { PORT_SIZE, dragSteps } from './patchermodel.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

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

export class Patcher {
  constructor(host, { onSelect, onFieldPick, onFieldDrag } = {}) {
    this.host = host;
    this.host.className = 'pt';
    this.onSelect = onSelect;
    // Picking a field and changing it are separate callbacks because they are
    // separate acts: clicking a row must select it whether or not you then drag,
    // and the keyboard operates on whatever the last click selected.
    this.onFieldPick = onFieldPick;
    this.onFieldDrag = onFieldDrag;

    // Three layers, and the split matters: `.pt-scroll` is the viewport and
    // `.pt-canvas` is the graph's own extent, so the notice below can stay put
    // while the graph scrolls under it.
    this.scroll = div('pt-scroll', host);
    this.canvas = div('pt-canvas', this.scroll);
    this.svg = document.createElementNS(SVG_NS, 'svg');
    this.svg.setAttribute('class', 'pt-edges');
    this.canvas.appendChild(this.svg);
    this.nodesEl = div('pt-nodes', this.canvas);
    this.notice = div('pt-notice', host);

    // The legend is built once and never touched by a draw. Three kinds of cable
    // that differ only in colour and dash are three kinds nobody can name; the
    // design puts this key on the canvas for the same reason.
    const key = div('pt-key', this.notice);
    for (const kind of ['event', 'audio', 'control']) {
      const item = div('pt-key-item', key);
      div('pt-key-swatch ' + kind, item);
      text(div('pt-key-label', item)).nodeValue = kind;
    }
    this.noticeText = text(div('pt-notice-text', this.notice));

    this.nodePool = [];
    this.edgePool = [];
    this.vm = null;
    this._notice = null;
    // Everything the notice sentence is made of, so it is composed only when one
    // of them moves. See the comment at the bottom of render() for why these
    // five are the whole of it.
    this._nEmpty = null; this._nDevice = -1; this._nLink = -2;
    this._nAdd = null; this._nUnres = -1;
    this._w = -1; this._h = -1;

    /*
     * A NODE'S PARAMETERS, BY POINTER.
     *
     * They were keyboard-only: select a node, arrow left/right to a field, arrow
     * up/down to change it. Every value on screen looked like a control and none
     * of them answered a click, which is the failure GUIDELINES calls out — a
     * thing that looks operable and is not costs more than one that looks inert.
     *
     * Press picks the row. Drag changes it, vertically, accumulating pixels (see
     * `dragSteps`) so a slow drag is not rounded away. Shift is finer. The
     * keyboard path is untouched and shares the same selection, so the two
     * agree about what "the selected field" means.
     */
    this.nodesEl.addEventListener('pointerdown', (e) => {
      const el = e.target.closest('.pt-node');
      if (!el) return;
      const id = Number(el.dataset.id);
      if (this.onSelect) this.onSelect(id);
      const row = e.target.closest('.pt-row');
      if (!row) return;
      const at = el._rows.indexOf(row);
      if (at < 0) return;
      if (this.onFieldPick) this.onFieldPick(id, at);
      if (!this.onFieldDrag) return;
      this._fieldDrag = { id, at, y0: e.clientY, applied: 0, pointerId: e.pointerId };
      this.nodesEl.setPointerCapture(e.pointerId);
      // The row keeps the cursor for the whole gesture, so a drag that wanders
      // off the row still reads as adjusting the thing you grabbed.
      row.classList.add('dragging');
      this._fieldRow = row;
    });
    this.nodesEl.addEventListener('pointermove', (e) => {
      const d = this._fieldDrag;
      if (!d) return;
      const { delta, total } = dragSteps(e.clientY - d.y0, d.applied, e.shiftKey);
      if (!delta) return;
      d.applied = total;
      this.onFieldDrag(d.id, d.at, delta);
    });
    const end = (e) => {
      const d = this._fieldDrag;
      if (!d) return;
      this._fieldDrag = null;
      try { this.nodesEl.releasePointerCapture(d.pointerId); } catch (err) { /* gone */ }
      if (this._fieldRow) { this._fieldRow.classList.remove('dragging'); this._fieldRow = null; }
    };
    this.nodesEl.addEventListener('pointerup', end);
    this.nodesEl.addEventListener('pointercancel', end);
    this._fieldDrag = null;
    this._fieldRow = null;
  }

  _node(i) {
    while (this.nodePool.length <= i) {
      const el = div('pt-node', this.nodesEl);
      const head = div('pt-head', el);
      el._type = text(div('pt-type', head));
      el._id = text(div('pt-id', head));
      el._body = div('pt-body', el);
      el._empty = div('pt-empty', el._body);
      text(el._empty).nodeValue = 'no config published';
      el._emptyV = null;
      el._rows = [];
      el._ports = [];
      el._typeV = null; el._idV = null; el._x = -1; el._y = -1;
      el._w = -1; el._hV = -1; el._sel = null; el._portsFor = -1;
      this.nodePool.push(el);
    }
    return this.nodePool[i];
  }

  /**
   * A node's ports. Bound on a TYPE change and never again: which ports a node
   * has, where they sit and what colour they are all come from its type, so a
   * per-frame rebind would be pure churn on a surface that mostly sits still.
   */
  _ports(el, n) {
    if (el._portsFor === n.type) return;
    el._portsFor = n.type;
    const list = n.ports;
    while (el._ports.length < list.length) {
      const p = div('pt-port', el);
      p._label = text(div('pt-port-lbl', p));
      p._cls = null; p._top = -1; p._lbl = null;
      el._ports.push(p);
    }
    for (let i = 0; i < el._ports.length; i++) {
      const p = el._ports[i];
      const on = i < list.length;
      const disp = on ? '' : 'none';
      if (p.style.display !== disp) p.style.display = disp;
      if (!on) continue;
      const port = list[i];
      const cls = 'pt-port ' + port.kindName + (port.out ? ' out' : ' in');
      if (p._cls !== cls) { p._cls = cls; p.className = cls; }
      // The dot is centred on the node's edge, so its top is the port's centre
      // less half its diameter — the same -5px the design uses on a 10px dot.
      const top = port.dy - PORT_SIZE / 2;
      if (p._top !== top) { p._top = top; p.style.top = top + 'px'; }
      if (p._lbl !== port.label) { p._lbl = port.label; p._label.nodeValue = port.label; }
      if (p.dataset.port !== '' + port.id) p.dataset.port = '' + port.id;
    }
  }

  /** A node's config, one row per named field: name, a bar, the value. */
  _rows(el, n) {
    if (el._emptyV !== n.unpublished) {
      el._emptyV = n.unpublished;
      el._empty.style.display = n.unpublished ? '' : 'none';
    }
    const rows = el._rows;
    while (rows.length < n.rowCount) {
      const r = div('pt-row', el._body);
      r._name = text(div('pt-row-n', r));
      const bar = div('pt-row-bar', r);
      r._fill = div('pt-row-fill', bar);
      r._bar = bar;
      r._value = text(div('pt-row-v', r));
      r._nV = null; r._vV = null; r._fV = -2; r._sel = null;
      rows.push(r);
    }
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i];
      const on = i < n.rowCount;
      const disp = on ? '' : 'none';
      if (r.style.display !== disp) r.style.display = disp;
      if (!on) continue;
      const q = n.rows[i];
      if (r._nV !== q.name) { r._nV = q.name; r._name.nodeValue = q.name; }
      if (r._vV !== q.value) { r._vV = q.value; r._value.nodeValue = q.value; }
      // -1 is "this type declares no range", which is not the same as an empty
      // bar. An always-empty bar beside a real number reads as "off".
      const pct = q.frac < 0 ? -1 : Math.round(q.frac * 100);
      if (r._fV !== pct) {
        r._fV = pct;
        if (pct < 0) { if (r._bar.style.display !== 'none') r._bar.style.display = 'none'; }
        else {
          if (r._bar.style.display !== '') r._bar.style.display = '';
          r._fill.style.width = pct + '%';
        }
      }
      if (r._sel !== q.selected) { r._sel = q.selected; r.classList.toggle('sel', q.selected); }
    }
  }

  _edge(i) {
    while (this.edgePool.length <= i) {
      const p = document.createElementNS(SVG_NS, 'path');
      p.setAttribute('class', 'pt-edge');
      // A fresh path carries no `display`, so it starts shown. Tracking that as
      // a flag is what lets the draw skip getAttribute() per cable per frame;
      // nothing but the two writes below ever touches the attribute.
      p._d = null; p._kind = null; p._on = true;
      this.svg.appendChild(p);
      this.edgePool.push(p);
    }
    return this.edgePool[i];
  }

  render(vm) {
    this.vm = vm;
    // The canvas is the graph's own extent, not the viewport's: an <svg> at
    // 100% clips every cable that runs past the visible box, and a nodes layer
    // with no size gives the host nothing to scroll.
    if (this._w !== vm.width) {
      this._w = vm.width;
      this.svg.setAttribute('width', vm.width);
      this.canvas.style.width = vm.width + 'px';
    }
    if (this._h !== vm.height) {
      this._h = vm.height;
      this.svg.setAttribute('height', vm.height);
      this.canvas.style.height = vm.height + 'px';
    }

    for (let i = 0; i < vm.nodeCount; i++) {
      const n = vm.nodes[i];
      const el = this._node(i);
      if (el.style.display === 'none') el.style.display = '';
      if (el._x !== n.x || el._y !== n.y) {
        el._x = n.x; el._y = n.y;
        el.style.transform = `translate(${n.x}px, ${n.y}px)`;
      }
      if (el._w !== n.w) { el._w = n.w; el.style.width = n.w + 'px'; }
      if (el._hV !== n.h) { el._hV = n.h; el.style.height = n.h + 'px'; }
      if (el._typeV !== n.typeName) { el._typeV = n.typeName; el._type.nodeValue = n.typeName; }
      if (el._idV !== n.id) { el._idV = n.id; el._id.nodeValue = '#' + n.id; el.dataset.id = String(n.id); }
      if (el._sel !== n.selected) { el._sel = n.selected; el.classList.toggle('sel', n.selected); }
      this._ports(el, n);
      this._rows(el, n);
    }
    for (let i = vm.nodeCount; i < this.nodePool.length; i++) {
      const el = this.nodePool[i];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    for (let i = 0; i < vm.edgeCount; i++) {
      const e = vm.edges[i];
      const p = this._edge(i);
      // An edge with no path is one whose endpoints this side could not place.
      // Hidden rather than drawn as an empty `d`, which some engines render as
      // a dot at the origin.
      const on = !!e.path;
      if (on) {
        if (!p._on) { p._on = true; p.removeAttribute('display'); }
        if (p._d !== e.path) { p._d = e.path; p.setAttribute('d', e.path); }
        if (p._kind !== e.kindName) { p._kind = e.kindName; p.setAttribute('class', 'pt-edge ' + e.kindName); }
      } else if (p._on) {
        p._on = false;
        p.setAttribute('display', 'none');
      }
    }
    for (let i = vm.edgeCount; i < this.edgePool.length; i++) {
      const p = this.edgePool[i];
      if (p._on) { p._on = false; p.setAttribute('display', 'none'); }
    }

    // The graph is real but singular. Saying so is the difference between a
    // surface that is honest about its scope and one that quietly implies a
    // per-device view it does not have.
    // What the next keystroke will do comes first, because it changes; the
    // standing caveat about one global graph comes after it.
    //
    // Three template literals for a sentence that is the same sentence on the
    // overwhelming majority of frames, so they are guarded on everything they
    // read: `empty` and `device` are the whole of the scope clause, `linkFrom`
    // and `addType` the whole of the keystroke clause, and `unresolved` the
    // whole of the tail. Nothing else in the view-model appears in the text, so
    // these five name it completely — a sixth term would only cost work.
    if (this._nEmpty !== vm.empty || this._nDevice !== vm.device
        || this._nLink !== vm.linkFrom || this._nAdd !== vm.addType
        || this._nUnres !== vm.unresolved) {
      this._nEmpty = vm.empty; this._nDevice = vm.device; this._nLink = vm.linkFrom;
      this._nAdd = vm.addType; this._nUnres = vm.unresolved;
      const scope = vm.empty
        ? 'no patcher graph published'
        : `one global graph, on device ${vm.device} — per-device execution is not in the engine yet`;
      const base = vm.linkFrom >= 0
        ? `connecting from #${vm.linkFrom} — press c on the destination · ${scope}`
        : (vm.addType ? `a adds ${vm.addType} (t cycles) · ${scope}` : scope);
      // A cable that could not find the port it names is drawn against the
      // nearest one of its kind. That is a guess, so it is counted out loud
      // rather than left looking like an exact read.
      const note = vm.unresolved
        ? `${base} · ${vm.unresolved} edge${vm.unresolved > 1 ? 's' : ''} name a port `
          + 'their node type does not have — drawn against the nearest of the same kind'
        : base;
      if (this._notice !== note) {
        this._notice = note;
        this.noticeText.nodeValue = note;
      }
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    const nodes = vm.nodes.slice(0, vm.nodeCount);
    const edges = vm.edges.slice(0, vm.edgeCount);
    return {
      nodes: vm.nodeCount, edges: vm.edgeCount, version: vm.version, device: vm.device,
      types: nodes.map((n) => n.typeName),
      field: (nodes.find((n) => n.selected) || {}).fieldName || '',
      addType: vm.addType,
      linkFrom: vm.linkFrom,
      notice: this._notice,
      configs: nodes.map((n) => n.config).filter(Boolean),
      // The three numbers the "no ports, cables wrong" verdict was about.
      ports: nodes.reduce((a, n) => a + n.ports.length, 0),
      portsPerNode: nodes.map((n) => n.ports.length),
      anchored: edges.filter((e) => e.anchored).length,
      exact: edges.filter((e) => e.exact).length,
      unresolved: vm.unresolved,
      extent: [vm.width, vm.height],
      domNodes: this.nodePool.length + this.edgePool.length,
    };
  }
}
