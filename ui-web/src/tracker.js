// Tracker renderer: DOM, driven by a view-model.
//
// Three rules carry the performance, all three measured on this machine at the
// redesign's real density (see ui-web/README.md for the table):
//
//   1. Scroll by transforming the band, never by rebinding visible cells.
//      Rebinding cost 11.66 ms/frame at 64x16 against 1.54 ms for a transform.
//      Only rows crossing the band edge are rebound.
//   2. `contain: strict` on the grid, rows absolutely positioned. This is why
//      PrePaint stays flat (0.047 -> 0.082 ms from 3,087 to 11,145 nodes) and
//      why Layout reads 0.000 during a scroll.
//   3. Everything visual comes from tokens.css. No hex, no font, no px literal
//      that design/tokens.json already carries.
//
// The row pool is fixed: OVERSCAN rows above and below the viewport, recycled.
// The timeline is unbounded, so there is no path here that materialises it.

const OVERSCAN = 8;

export class Tracker {
  /**
   * @param {HTMLElement} host
   * @param {{rowHeight:number, gutterWidth:number, laneWidth:number, cellWidth:number}} metrics
   */
  constructor(host, metrics) {
    this.host = host;
    this.m = metrics;
    this.vm = null;
    this.poolSize = 0;
    /** @type {HTMLElement[]} */
    this.pool = [];
    /** @type {Map<number, HTMLElement>} */
    this.byRow = new Map();
    this.bandTop = 0;      // absolute row index the band's first element holds
    this.scrollRow = 0;    // fractional top row of the viewport

    host.className = 'tk-host';
    this.rails = el('div', 'tk-rails');   // clip rails: OUTSIDE the recycled band
    this.band = el('div', 'tk-band');     // the recycled row pool
    host.append(this.rails, this.band);
  }

  /** Size the pool to the viewport. Called on mount and resize only. */
  resize(viewportHeight, rowCount, columns) {
    const need = Math.ceil(viewportHeight / this.m.rowHeight) + OVERSCAN * 2;
    if (need === this.poolSize && this.cols === columns && this.tracks === rowCount) return;
    this.poolSize = need;
    this.tracks = rowCount;
    this.cols = columns;
    this.band.textContent = '';
    this.pool = Array.from({ length: need }, () => this.makeRow(rowCount, columns));
    this.band.append(...this.pool);
  }

  makeRow(trackCount, columns) {
    const row = el('div', 'tk-row');
    row.append(el('div', 'tk-gutter'));
    for (let t = 0; t < trackCount; t++) {
      const tr = el('div', 'tk-track');
      tr.style.setProperty('--tint', `var(--uni-track-tint-${t % 8})`);
      for (let c = 0; c < columns; c++) {
        const cell = el('div', 'tk-cell');
        cell.dataset.track = String(t);
        cell.dataset.col = String(c);
        tr.append(cell);
      }
      row.append(tr);
    }
    return row;
  }

  /** Full bind of one pooled element to an absolute row. Only on band-edge crossing. */
  bindRow(elm, row) {
    elm.dataset.row = String(row.index);
    elm.style.top = `${row.index * this.m.rowHeight}px`;
    elm.classList.toggle('bar', row.bar);
    elm.classList.toggle('beat', row.beat && !row.bar);
    elm.firstChild.textContent = row.label;
    let i = 0;
    for (const tr of elm.children) {
      if (!tr.classList.contains('tk-track')) continue;
      for (const cell of tr.children) {
        const c = row.cells[i++];
        cell.textContent = c.text;
        cell.dataset.kind = c.kind;
      }
    }
  }

  /** @param {ReturnType<import('./viewmodel.js').buildViewModel>} vm */
  render(vm) {
    const prev = this.vm;
    this.vm = vm;
    const first = vm.window.startRow;

    // Rebind only what actually changed identity.
    const needFull = !prev || prev.zoom.index !== vm.zoom.index || prev.tracks.length !== vm.tracks.length;
    this.byRow.clear();
    for (let i = 0; i < this.pool.length; i++) {
      const rowIdx = first + i;
      const row = vm.rows[i];
      const elm = this.pool[i];
      if (!row) { elm.style.display = 'none'; continue; }
      elm.style.display = '';
      if (needFull || elm.dataset.row !== String(rowIdx)) this.bindRow(elm, row);
      this.byRow.set(rowIdx, elm);
    }

    this.band.style.transform = `translateY(${-first * this.m.rowHeight}px)`;
    this.rails.style.transform = this.band.style.transform;
    this.paintClips(vm);
    this.paintState(vm, prev);
  }

  /** Rails span rows, so they live outside the band and are keyed by clip id. */
  paintClips(vm) {
    const seen = new Set();
    for (const clip of vm.clips) {
      seen.add(clip.id);
      let r = this.rails.querySelector(`[data-clip="${clip.id}"]`);
      if (!r) {
        r = el('div', 'tk-rail');
        r.dataset.clip = String(clip.id);
        this.rails.append(r);
      }
      r.classList.toggle('active', clip.active);
      r.style.top = `${clip.startRow * this.m.rowHeight}px`;
      r.style.height = `${(clip.endRow - clip.startRow + 1) * this.m.rowHeight}px`;
      r.style.left = `${this.m.gutterWidth + this.m.laneWidth + clip.track * this.m.cellWidth * this.cols}px`;
      r.style.width = `${this.m.cellWidth * this.cols}px`;
    }
    for (const r of [...this.rails.children]) {
      if (!seen.has(+r.dataset.clip)) r.remove();
    }
  }

  /** Cursor / playhead / selection: a handful of class toggles, never a rebind. */
  paintState(vm, prev) {
    if (prev) {
      this.byRow.get(prev.playhead.row)?.classList.remove('playhead');
      const pc = this.cellEl(prev.cursor);
      pc?.classList.remove('cursor');
    }
    this.byRow.get(vm.playhead.row)?.classList.add('playhead');
    this.cellEl(vm.cursor)?.classList.add('cursor');
  }

  cellEl(cur) {
    const row = this.byRow.get(cur.row);
    return row?.querySelector(`[data-track="${cur.track}"][data-col="${cur.col}"]`) ?? null;
  }

  /** What an agent (or a test) asks. Structure, not pixels. */
  probe() {
    return {
      startRow: this.vm.window.startRow,
      rowCount: this.vm.rows.length,
      tracks: this.vm.tracks.length,
      columns: this.cols,
      zoom: this.vm.zoom.label,
      poolSize: this.poolSize,
      domNodes: this.host.querySelectorAll('*').length,
      cursor: this.vm.cursor,
      playhead: this.vm.playhead.row,
      clips: this.vm.clips.length,
      cellText: (row, track, col) =>
        this.host.querySelector(`[data-row="${row}"] [data-track="${track}"][data-col="${col}"]`)?.textContent ?? null,
      cellRect: (row, track, col) => {
        const e = this.host.querySelector(`[data-row="${row}"] [data-track="${track}"][data-col="${col}"]`);
        if (!e) return null;
        const b = e.getBoundingClientRect();
        return { x: b.x, y: b.y, w: b.width, h: b.height };
      },
    };
  }
}

function el(tag, cls) {
  const e = document.createElement(tag);
  e.className = cls;
  return e;
}
