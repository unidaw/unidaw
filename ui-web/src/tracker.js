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

const OVERSCAN = 4;

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
    /** Fixed pool of rail elements. Clips scroll in and out constantly, and
     *  creating/removing a DOM node per clip per frame churned 219 nodes over a
     *  28-second scroll — allocation and layout in the hot path. Rails are now
     *  reused and hidden, never destroyed, so a scroll mutates no DOM structure. */
    this.railPool = [];
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
    this.measure();
  }

  /**
   * Ask CSS for the geometry instead of recomputing it. Deriving the content
   * width arithmetically meant duplicating the box model, and it silently
   * omitted the per-track 2px border — 32px across 16 tracks, which made the
   * last 30px of the strip permanently unreachable by horizontal scrolling.
   *
   * Forces layout, so it runs on shape change only, never per frame.
   */
  measure() {
    const row = this.pool[0];
    if (!row) return;
    this.contentWidth = row.scrollWidth;
    const tracks = row.querySelectorAll('.tk-track');
    this.stripLeft = tracks[0] ? tracks[0].offsetLeft : this.m.gutterWidth;
    this.trackStride = tracks.length > 1 ? tracks[1].offsetLeft - tracks[0].offsetLeft
                                         : this.m.cellWidth * this.cols;
  }

  /** Left edge of a cell, in band coordinates. Uses measured stride, so it
   *  stays correct whatever the CSS borders do. */
  cellLeft(track, col) {
    return this.stripLeft + track * this.trackStride + col * this.m.cellWidth;
  }

  /** How far right the band can travel before the strip's end meets the edge. */
  maxScrollX(viewportWidth) {
    return Math.max(0, (this.contentWidth || 0) - viewportWidth);
  }

  makeRow(trackCount, columns) {
    const row = el('div', 'tk-row');
    const g = el('div', 'tk-gutter');
    g.appendChild(document.createTextNode(''));
    row.append(g);
    // The harmony column, between time and tracks. Two text nodes owned up
    // front, like every other cell here: the key and the tuning line.
    const h = el('div', 'tk-harm');
    const hl = el('span', 'tk-harm-key');
    hl.appendChild(document.createTextNode(''));
    const hs = el('span', 'tk-harm-sub');
    hs.appendChild(document.createTextNode(''));
    h.append(hl, hs);
    row.append(h);
    for (let t = 0; t < trackCount; t++) {
      const tr = el('div', 'tk-track');
      tr.style.setProperty('--tint', `var(--uni-track-tint-${t % 8})`);
      for (let c = 0; c < columns; c++) {
        const cell = el('div', 'tk-cell');
        cell.dataset.track = String(t);
        cell.dataset.col = String(c);
        // Own the Text node up front. Assigning .textContent destroys and
        // recreates one every write — 621,239 node mutations over a 5-minute
        // soak, which is the heap sawtooth. Writing .nodeValue mutates in place
        // and allocates nothing.
        cell.appendChild(document.createTextNode(''));
        // The contour mark, created once and hidden. A pitch-range bar says more
        // at coarse zoom than a count does; see viewmodel.js.
        const bar = el('i', 'tk-bar');
        cell.appendChild(bar);
        tr.append(cell);
      }
      row.append(tr);
    }
    return row;
  }

  /**
   * Full bind of one pooled element to an absolute row. Only on band-edge
   * crossing or a zoom change.
   *
   * Every write is guarded by a comparison. That looks redundant but it is the
   * difference between a scroll and a no-op: assigning textContent invalidates
   * style and paint for that node even when the string is identical, so an
   * unguarded rebind makes a clamped zoom or a cursor move cost as much as a
   * real scroll. Reading first is far cheaper than dirtying the node.
   */
  bindRow(elm, row) {
    // Numeric cache, not a string compare. String(row.index) allocated once per
    // pool row per draw — 66 strings a frame for a value that rarely changes.
    if (elm._row !== row.index) {
      elm._row = row.index;
      elm.dataset.row = row.index;            // assigning a number coerces once
      elm.style.top = (row.index * this.m.rowHeight) + 'px';
    }
    elm.classList.toggle('bar', row.bar);
    elm.classList.toggle('beat', row.beat && !row.bar);
    const gt = elm.firstChild.firstChild;
    if (gt.nodeValue !== row.label) gt.nodeValue = row.label;
    // The harmony cell. `starts` drives a class rather than the text alone: a
    // change is a rule on the page, and the row it lands on needs the emphasis
    // whether or not the label happens to be wide.
    const hcell = elm.children[1];
    const h = row.harmony;
    const hk = hcell.firstChild.firstChild;
    const hsv = hcell.lastChild.firstChild;
    if (hk.nodeValue !== h.label) hk.nodeValue = h.label;
    if (hsv.nodeValue !== h.sub) hsv.nodeValue = h.sub;
    if (hcell._st !== h.starts) { hcell._st = h.starts; hcell.classList.toggle('starts', h.starts); }
    if (hcell._ac !== h.active) { hcell._ac = h.active; hcell.classList.toggle('active', h.active); }
    let i = 0;
    for (const tr of elm.children) {
      if (!tr.classList.contains('tk-track')) continue;
      for (const cell of tr.children) {
        const c = row.cells[i++];
        const bar = cell.lastChild;
        if (c.aggCount) {
          // MIDI 24..96 covers the useful range; clamp rather than scale to the
          // window, so a mark means the same pitch height on every row.
          const lo = Math.max(0, Math.min(1, (c.aggLo - 24) / 72));
          const hi = Math.max(0, Math.min(1, (c.aggHi - 24) / 72));
          const bottom = (lo * 100) | 0;
          const height = Math.max(8, ((hi - lo) * 100) | 0);
          const op = Math.min(1, 0.25 + c.aggCount / 24);
          if (bar._b !== bottom) { bar._b = bottom; bar.style.bottom = bottom + '%'; }
          if (bar._h !== height) { bar._h = height; bar.style.height = height + '%'; }
          if (bar._o !== op) { bar._o = op; bar.style.opacity = op; }
          if (bar._on !== 1) { bar._on = 1; bar.style.display = 'block'; }
        } else if (bar._on !== 0) { bar._on = 0; bar.style.display = 'none'; }
        const tn = cell.firstChild;
        if (tn.nodeValue !== c.text) tn.nodeValue = c.text;
        if (cell.dataset.kind !== c.kind) cell.dataset.kind = c.kind;
      }
    }
  }

  /** @param {ReturnType<import('./viewmodel.js').buildViewModel>} vm */
  render(vm) {
    const prev = this.vm;
    this.vm = vm;
    const first = vm.window.startRow;

    // A ring, not a list. Pool slot is `row mod poolSize`, so a row keeps the
    // same element until it leaves the window entirely — scrolling one row
    // rebinds exactly one element instead of all of them.
    //
    // Indexing the pool by position instead looks equivalent and is not: every
    // element's row identity shifts by one on every scroll, so a one-row move
    // costs a full rebind of the window. Measured, that mistake made a 1-row
    // and a 32-row scroll cost the same 7.8 ms.
    //
    // Absolute `top` is what makes this work — an element is positioned by the
    // row it holds, so which slot it occupies does not matter.
    // Content can change without any row changing identity — engine notes
    // arriving is exactly that case, and it is invisible to an identity check.
    // The renderer bound cells only on rebind, so live data never reached the
    // DOM: the grid kept showing whatever the first draw put there. A content
    // revision makes "the same rows now say something different" expressible.
    const needFull = !prev
      || prev.zoom.index !== vm.zoom.index
      || prev.tracks.length !== vm.tracks.length
      || (vm.contentRevision || 0) !== (prev.contentRevision || 0);
    const n = this.pool.length;

    // A zoom re-contents every row, and doing the whole pool in the input frame
    // costs 14.4 ms of a 16.6 ms budget — 86% utilisation, so any jitter misses
    // the deadline. Only the rows a user can actually see have to be right this
    // frame; the overscan exists for scroll headroom and can land in the next
    // one. Deferring it trades one frame of headroom for a zoom step that fits.
    const visible = Math.min(vm.rows.length, Math.ceil(this.host.clientHeight / this.m.rowHeight) + 1);
    const limit = needFull ? visible : vm.rows.length;

    this.byRow.clear();
    for (let i = 0; i < vm.rows.length && i < n; i++) {
      const rowIdx = first + i;
      const row = vm.rows[i];
      const elm = this.pool[((rowIdx % n) + n) % n];
      if (elm.style.display === 'none') elm.style.display = '';
      if (i < limit && (needFull || elm._row !== rowIdx)) this.bindRow(elm, row);
      this.byRow.set(rowIdx, elm);
    }

    if (needFull && limit < vm.rows.length) {
      cancelAnimationFrame(this._tail);
      this._tail = requestAnimationFrame(() => {
        if (this.vm !== vm) return;            // superseded by a newer frame
        for (let i = limit; i < vm.rows.length && i < n; i++) {
          const rowIdx = first + i;
          this.bindRow(this.pool[((rowIdx % n) + n) % n], vm.rows[i]);
        }
      });
    }
    // Slots not claimed this frame (a short window at the end of the timeline).
    for (let i = vm.rows.length; i < n; i++) {
      const elm = this.pool[(((first + i) % n) + n) % n];
      if (elm.style.display !== 'none') elm.style.display = 'none';
    }

    const sx = -(vm.scrollX || 0), sy = -first * this.m.rowHeight;
    if (this._sx !== sx || this._sy !== sy) {
      this._sx = sx; this._sy = sy;
      const xf = 'translate(' + sx + 'px, ' + sy + 'px)';   // only on movement
      this.band.style.transform = xf;
      this.rails.style.transform = xf;
    }
    this.paintClips(vm);
    this.paintState(vm, prev);
  }

  /**
   * Rails span rows, so they live outside the recycled band and are keyed by
   * clip id. Clips arrive in ticks; projecting to pixels is the renderer's job,
   * which is what keeps a clip from moving when the zoom changes.
   */
  paintClips(vm) {
    const perRow = vm.zoom.rowNanoticks;
    for (let i = 0; i < vm.clips.length; i++) {
      const clip = vm.clips[i];
      let r = this.railPool[i];
      if (!r) {
        r = el('div', 'tk-rail');
        this.rails.append(r);
        this.railPool[i] = r;
      }
      if (r._id !== clip.id) { r._id = clip.id; r.dataset.clip = clip.id; }
      if (r.style.display === 'none') r.style.display = '';
      if (r._active !== clip.active) { r._active = clip.active; r.classList.toggle('active', clip.active); }
      const top = (clip.startTick / perRow) * this.m.rowHeight;
      const height = Math.max(1, (clip.endTick - clip.startTick) / perRow) * this.m.rowHeight;
      const left = this.stripLeft + clip.track * this.trackStride;
      const width = this.trackStride;
      if (r._top !== top) { r._top = top; r.style.top = top + 'px'; }
      if (r._h !== height) { r._h = height; r.style.height = height + 'px'; }
      if (r._l !== left) { r._l = left; r.style.left = left + 'px'; }
      if (r._w !== width) { r._w = width; r.style.width = width + 'px'; }
    }
    // Surplus rails are hidden, not removed — the pool high-water-marks and
    // then stops mutating the DOM entirely.
    for (let i = vm.clips.length; i < this.railPool.length; i++) {
      const r = this.railPool[i];
      if (r.style.display !== 'none') r.style.display = 'none';
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
    this.paintSelection(vm);
  }

  /**
   * Selection is a rectangle in (row, field) space, where field is the flattened
   * track*columns+col — so a drag across a track boundary selects the fields
   * between, which is what a tracker user means by it.
   *
   * Painted by toggling a class on the cells inside, and only on the cells whose
   * membership actually changed, so dragging costs a handful of class writes
   * rather than a repaint of the band.
   */
  paintSelection(vm) {
    const s = vm.selection;
    for (const [rowIdx, elm] of this.byRow) {
      const inRows = s && rowIdx >= s.r0 && rowIdx <= s.r1;
      let f = 0;
      for (const tr of elm.children) {
        if (!tr.classList.contains('tk-track')) continue;
        for (const cell of tr.children) {
          const on = inRows && f >= s.f0 && f <= s.f1;
          if (cell._sel !== on) { cell._sel = on; cell.classList.toggle('sel', on); }
          f++;
        }
      }
    }
  }

  cellEl(cur) {
    const row = this.byRow.get(cur.row);
    return row?.querySelector(`[data-track="${cur.track}"][data-col="${cur.col}"]`) ?? null;
  }

  /**
   * Pixel -> cell. Uses the measured stride, so it stays correct whatever the
   * CSS borders do — the same reason cellLeft() measures rather than computes.
   * Returns null outside the grid or over the gutter.
   */
  hitTest(clientX, clientY, scrollX) {
    const b = this.host.getBoundingClientRect();
    const x = clientX - b.left + scrollX;
    const y = clientY - b.top;
    if (x < this.stripLeft || y < 0) return null;
    const row = this.vm.window.startRow + Math.floor(y / this.m.rowHeight);
    const rel = x - this.stripLeft;
    const track = Math.floor(rel / this.trackStride);
    if (track < 0 || track >= this.tracks) return null;
    const col = Math.min(this.cols - 1, Math.floor((rel - track * this.trackStride) / this.m.cellWidth));
    return { row, track, col };
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
      domNodes: this.host.querySelectorAll('*').length, // O(tree) — diagnostics only, never per frame
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
