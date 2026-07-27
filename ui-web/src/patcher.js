// Patcher renderer: nodes as boxes, edges as SVG curves.
//
// SVG for the edges because they are curves, and one <svg> holding all of them
// rather than one per edge — a path element is cheap, an svg element is not.
// Nodes stay as divs so they can carry text without SVG's layout rules.

const SVG_NS = 'http://www.w3.org/2000/svg';

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

export class Patcher {
  constructor(host, { onSelect } = {}) {
    this.host = host;
    this.host.className = 'pt';
    this.onSelect = onSelect;

    this.svg = document.createElementNS(SVG_NS, 'svg');
    this.svg.setAttribute('class', 'pt-edges');
    this.host.appendChild(this.svg);
    this.nodesEl = div('pt-nodes', host);
    this.notice = div('pt-notice', host);
    this.notice.appendChild(document.createTextNode(''));

    this.nodePool = [];
    this.edgePool = [];
    this.vm = null;
    this._notice = null;

    this.nodesEl.addEventListener('pointerdown', (e) => {
      const el = e.target.closest('.pt-node');
      if (el && this.onSelect) this.onSelect(Number(el.dataset.id));
    });
  }

  _node(i) {
    while (this.nodePool.length <= i) {
      const el = div('pt-node', this.nodesEl);
      const type = div('pt-type', el);
      type.appendChild(document.createTextNode(''));
      const id = div('pt-id', el);
      id.appendChild(document.createTextNode(''));
      const cfg = div('pt-cfg', el);
      cfg.appendChild(document.createTextNode(''));
      const field = div('pt-field', el);
      field.appendChild(document.createTextNode(''));
      el._type = type.firstChild; el._id = id.firstChild; el._cfg = cfg.firstChild;
      el._field = field.firstChild; el._fieldV = null;
      el._typeV = null; el._idV = null; el._cfgV = null; el._x = -1; el._y = -1; el._sel = null;
      this.nodePool.push(el);
    }
    return this.nodePool[i];
  }

  _edge(i) {
    while (this.edgePool.length <= i) {
      const p = document.createElementNS(SVG_NS, 'path');
      p.setAttribute('class', 'pt-edge');
      p._d = null; p._kind = null;
      this.svg.appendChild(p);
      this.edgePool.push(p);
    }
    return this.edgePool[i];
  }

  render(vm) {
    this.vm = vm;
    for (let i = 0; i < vm.nodeCount; i++) {
      const n = vm.nodes[i];
      const el = this._node(i);
      if (el.style.display === 'none') el.style.display = '';
      if (el._x !== n.x || el._y !== n.y) {
        el._x = n.x; el._y = n.y;
        el.style.transform = `translate(${n.x}px, ${n.y}px)`;
      }
      if (el._typeV !== n.typeName) { el._typeV = n.typeName; el._type.nodeValue = n.typeName; }
      if (el._idV !== n.id) { el._idV = n.id; el._id.nodeValue = '#' + n.id; el.dataset.id = String(n.id); }
      if (el._cfgV !== n.config) { el._cfgV = n.config; el._cfg.nodeValue = n.config; }
      if (el._sel !== n.selected) { el._sel = n.selected; el.classList.toggle('sel', n.selected); }
      if (el._fieldV !== n.fieldName) {
        el._fieldV = n.fieldName;
        el._field.nodeValue = n.fieldName ? '\u25B8 ' + n.fieldName : '';
      }
    }
    for (let i = vm.nodeCount; i < this.nodePool.length; i++) {
      const el = this.nodePool[i];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    for (let i = 0; i < vm.edgeCount; i++) {
      const e = vm.edges[i];
      const p = this._edge(i);
      if (p.getAttribute('display') === 'none') p.removeAttribute('display');
      if (p._d !== e.path) { p._d = e.path; p.setAttribute('d', e.path); }
      if (p._kind !== e.kindName) { p._kind = e.kindName; p.setAttribute('class', 'pt-edge ' + e.kindName); }
    }
    for (let i = vm.edgeCount; i < this.edgePool.length; i++) {
      const p = this.edgePool[i];
      if (p.getAttribute('display') !== 'none') p.setAttribute('display', 'none');
    }

    // The graph is real but singular. Saying so is the difference between a
    // surface that is honest about its scope and one that quietly implies a
    // per-device view it does not have.
    // What the next keystroke will do comes first, because it changes; the
    // standing caveat about one global graph comes after it.
    const scope = vm.empty
      ? 'no patcher graph published'
      : `one global graph, on device ${vm.device} — per-device execution is not in the engine yet`;
    const note = vm.linkFrom >= 0
      ? `connecting from #${vm.linkFrom} — press c on the destination · ${scope}`
      : (vm.addType ? `a adds ${vm.addType} (t cycles) · ${scope}` : scope);
    if (this._notice !== note) {
      this._notice = note;
      this.notice.firstChild.nodeValue = note;
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      nodes: vm.nodeCount, edges: vm.edgeCount, version: vm.version, device: vm.device,
      types: vm.nodes.slice(0, vm.nodeCount).map((n) => n.typeName),
      field: (vm.nodes.slice(0, vm.nodeCount).find((n) => n.selected) || {}).fieldName || '',
      addType: vm.addType,
      linkFrom: vm.linkFrom,
      notice: this._notice,
      configs: vm.nodes.slice(0, vm.nodeCount).map((n) => n.config).filter(Boolean),
      domNodes: this.nodePool.length + this.edgePool.length,
    };
  }
}
