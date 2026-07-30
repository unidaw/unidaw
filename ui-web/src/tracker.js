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

/** The eight per-track hues, as the custom-property values a rail is set to.
 *  Fixed at load; see paintClips for why they are a table. */
const TRACK_HUES = [];
for (let i = 0; i < 8; i++) TRACK_HUES.push('var(--uni-track-hue-' + i + ')');

/**
 * '0%'..'99%', built once.
 *
 * A hundred possible values and one of them written per marked cell per frame:
 * exactly the domain that is small, closed and dense enough to precompute, which
 * is the same argument the velocity and effect columns already make.
 */
const DEV_PCT = new Array(100);
for (let i = 0; i < 100; i++) DEV_PCT[i] = i + '%';

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
    /** Per-track geometry, filled by measure(). Tracks are not a uniform width:
     *  a lane carries its bar readout only when its bars are not the song's. */
    this.trackLeft = null; this.trackWidth = null; this.laneBarW = null;
    this.trackCountMeasured = 0;
    this.widestTrack = 0;
    /** The lane-visibility bitmask currently applied to the pool. */
    this._laneSig = -1;
    /** ...and which lanes are folded away under a collapsed parent. */
    this._hiddenSig = -1;
    /** @type {HTMLElement[]} */
    this.pool = [];
    /** How many pool slots the current frame's window claims — see rowEl(). */
    this._live = 0;
    /*
     * The elements currently WEARING the cursor and playhead classes.
     *
     * Held rather than recomputed, because the recomputation cannot find them once
     * the window has scrolled past their rows — see paintState. Declared here so the
     * shape is fixed from construction.
     */
    this._curEl = null;
    this._phEl = null;
    /** Whether last frame had a selection; see paintSelection(). */
    this._hadSel = false;
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
    // The harmony lane. Outside the band because a field SPANS rows, and outside
    // the horizontal scroll because the design keeps it fixed beside the time
    // gutter — it answers "where am I in the music", which does not scroll away
    // when you look at track 12.
    this.harmLane = el('div', 'tk-harm-lane');
    this.harmPool = [];
    /*
     * THE RULER: one exemplar of each track WIDTH CLASS, measured instead of every track.
     *
     * `measure()` used to read a box per track — O(tracks) layout reads on every shape change —
     * and, more importantly, it could only learn the width of a track that was RENDERED. That
     * is the thing standing between this file and virtualizing the track axis: you cannot ask
     * the browser how wide a track is if you have not drawn it.
     *
     * There are only ever three widths: folded (0), plain, and plain-plus-lane-bar. So measure
     * one of each and SUM IN JS. That keeps the box model in the stylesheet — the rule
     * GUIDELINES 3.11 actually states, which is about two authorities disagreeing, not about
     * arithmetic — while making the prefix sum something this file owns rather than something
     * it reads back out of flex layout.
     *
     * It is worth being exact about the rule, because this looks like breaking it. The repo
     * ALREADY computes track geometry as a cold-start fallback in index.html, and that
     * computation is already wrong: `cellWidth * columns + laneBarWidth` is 268 against a real
     * 270, missing the 2px border — the very mistake 3.11 is named after, with a comment above
     * it saying so. Measuring two real boxes and adding is strictly better than that, and
     * strictly better than reading N boxes, because CSS stays the only authority on a width.
     *
     * `visibility: hidden` rather than `display: none`: a display-none box measures 0, which is
     * how this file once recorded a zero-width strip and made all 612 cells unclickable.
     */
    this.ruler = null;
    this._wPlain = 0; this._wBar = 0; this._wLaneBar = 0;
    host.append(this.rails, this.harmLane, this.band);
  }

  /** Size the pool to the viewport. Called on mount and resize only. */
  resize(viewportHeight, rowCount, columns) {
    // Stored before the early return, because render() reads it every frame to
    // decide how many rows are actually VISIBLE (as opposed to overscan). It
    // used to ask `this.host.clientHeight` there, which forces a style and
    // layout flush in the middle of the draw — and the caller has just measured
    // the same box to call this, so the number was already in hand.
    // Compare the INPUTS, then compute. `Math.ceil(h / rowHeight)` is a division
    // that lands on a fraction, so V8 boxes the intermediate — ~33 bytes a frame
    // to work out a pool size that has not changed since the window was last
    // resized. The three inputs decide `need` entirely, so comparing them first
    // is both cheaper and exact.
    if (this._viewH === viewportHeight && this.cols === columns
        && this.tracks === rowCount && this.poolSize > 0) return;
    this._viewH = viewportHeight;
    const need = Math.ceil(viewportHeight / this.m.rowHeight) + OVERSCAN * 2;
    if (need === this.poolSize && this.cols === columns && this.tracks === rowCount) return;
    this.poolSize = need;
    this.tracks = rowCount;
    this.cols = columns;
    this.band.textContent = '';
    this.pool = Array.from({ length: need }, () => this.makeRow(rowCount, columns));
    // The ruler carries `columns` cells like any track, so it is rebuilt with the pool — a
    // ruler measured at three columns while the strip draws six would put every track after
    // the first in the wrong place, which is the failure this whole model exists to prevent.
    this.buildRuler(columns);
    this.band.append(...this.pool);
    /**
     * The new rows carry NO lane classes, so the cached signatures must stop
     * claiming they do.
     *
     * `applyLaneShow` early-returns when the signature is unchanged — which after
     * a pool rebuild is exactly the wrong answer, because the rows it would have
     * styled no longer exist. Measured: fold a parent, resize the window, and the
     * collapsed children reappear at full width while `folded()` still reports
     * them folded. It does not heal, because nothing else changes those
     * signatures; only toggling the fold again does.
     *
     * Worse when the widest track is unaffected: `trackStride` does not change
     * either, so buildHead's key is unchanged too and the header is never rebuilt
     * — leaving headers 40px left of the lanes they label, permanently.
     */
    this._laneSig = -1;
    this._hiddenSig = -1;
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
  /**
   * Track geometry, measured off the DOM.
   *
   * Per track rather than one stride, because a lane carries its bar readout only
   * when its bars are not the song's — so tracks are no longer all the same width.
   * `trackStride` survives as the widest track, which is what the header's cold
   * start and the scroll clamp want; everything that positions a cell uses the
   * per-track offset.
   *
   * Measured, never derived from the tokens (GUIDELINES 3.11): the box model lives
   * in the stylesheet, and a second copy here is how a hit test and a paint end up
   * disagreeing by a border width.
   */
  measure() {
    /*
     * THE PREFIX SUM IS OURS NOW.
     *
     * This used to read `offsetLeft` and `offsetWidth` from every track and take `scrollWidth`
     * off the row — i.e. it asked flex layout to do the summation and read the answer back. That
     * works only for tracks that are RENDERED, which is exactly the wall between this file and
     * virtualizing the track axis.
     *
     * Now: four box reads for the two width classes (see `measureClasses`), then addition. CSS
     * is still the only authority on how wide a track is; this file is the authority on where
     * they sit, which is a different fact and one it can hold for tracks it has not drawn.
     *
     * A test pins the model against the rendered box for every drawn track, so the two cannot
     * drift — that assertion is the whole reason this is safe, and it is what the old
     * "measured, never derived" comment was really protecting.
     */
    const n = this.tracks | 0;
    if (!n) return;
    this.measureClasses();
    if (!this._wPlain) return;      // nothing measured yet; keep the last real geometry

    /*
     * stripLeft is still MEASURED. It is the gutter plus the harmony spacer — structure this
     * file does not own and has no class for — so summing it would be the second authority the
     * rule warns about. One box read, and only when a row exists to read it from.
     */
    const row = this.pool[0];
    if (row) {
      const first = row.querySelector('.tk-track');
      if (first) {
        const box = this.host.getBoundingClientRect();
        if (box.width >= 1 && box.height >= 1) this.stripLeft = first.offsetLeft;
      }
    }
    if (!this.stripLeft) this.stripLeft = this.m.gutterWidth;

    if (!this.trackLeft || this.trackLeft.length < n + 1) {
      this.trackLeft = new Float64Array((n + 1) * 2);
      this.trackWidth = new Float64Array((n + 1) * 2);
      this.laneBarW = new Float64Array((n + 1) * 2);
    }
    let widest = 0, x = 0;
    for (let t = 0; t < n; t++) {
      const w = this.trackWidthOf(t);
      this.trackLeft[t] = x;
      this.trackWidth[t] = w;
      this.laneBarW[t] = (this._laneHidden && this._laneHidden[t]) ? 0
                       : ((this._laneShow && this._laneShow[t]) ? this._wLaneBar : 0);
      x += w;
      if (w > widest) widest = w;
    }
    // One past the end, so the strip's extent is a subscript rather than a special case.
    this.trackLeft[n] = x;
    this.trackCountMeasured = n;
    this.contentWidth = this.stripLeft + x;
    this.trackStride = widest || this.m.cellWidth * this.cols;
    this.widestTrack = this.trackStride;
  }

  /**
   * Show or hide each lane's readout, and re-measure when the set changes.
   *
   * `display: none` rather than a zero width, so the track's box genuinely shrinks
   * and `measure()` sees it. Guarded on a bitmask: this walks every row and every
   * track, and the answer changes on a project load or a clip edit, never inside a
   * scroll. Re-measuring is not optional — every cell position, the hit test, the
   * header and the scroll clamp are derived from widths that just changed.
   */
  applyLaneShow(laneShow, sig, laneHidden, hiddenSig) {
    if (this._laneSig === sig && this._hiddenSig === hiddenSig) return false;
    this._laneSig = sig;
    this._hiddenSig = hiddenSig;
    // KEPT, not just applied: the width model asks these for every track, including ones with
    // no DOM. Held by reference — the caller rebuilds them per frame into the same arrays.
    this._laneShow = laneShow;
    this._laneHidden = laneHidden;
    for (let i = 0; i < this.pool.length; i++) {
      const lanes = this.pool[i]._lanes;
      if (!lanes) continue;
      for (let t = 0; t < lanes.length; t++) {
        lanes[t].classList.toggle('no-lane', !(laneShow && laneShow[t]));
        // A lane whose parent is collapsed takes no width. NOT removed and not
        // renumbered: the track keeps its published index, so the cursor, the
        // selection's field indices and every track-keyed command are untouched.
        // Collapse is what is drawn, never what exists.
        lanes[t].classList.toggle('folded', !!(laneHidden && laneHidden[t]));
      }
    }
    this.measure();
    return true;
  }

  /** Left edge of a cell, in band coordinates. Uses measured stride, so it
   *  stays correct whatever the CSS borders do. */
  cellLeft(track, col) {
    const left = this.trackLeft && track < this.trackCountMeasured
      ? this.trackLeft[track] : track * this.trackStride;
    const lb = this.laneBarW && track < this.trackCountMeasured ? this.laneBarW[track] : 0;
    return this.stripLeft + left + lb + col * this.m.cellWidth;
  }

  /** Which track a band-relative x falls in, or -1. Linear because tracks are no
   *  longer a uniform stride and sixteen is the cap. */
  trackAt(rel) {
    const n = this.trackCountMeasured || 0;
    for (let t = 0; t < n; t++) {
      if (rel >= this.trackLeft[t] && rel < this.trackLeft[t] + this.trackWidth[t]) return t;
    }
    return -1;
  }

  /** How far right the band can travel before the strip's end meets the edge. */
  maxScrollX(viewportWidth) {
    return Math.max(0, (this.contentWidth || 0) - viewportWidth);
  }

  /**
   * (Re)build the ruler for the current column count. Two tracks: index 0 carries its lane bar,
   * index 1 does not. Their widths are the two non-zero width classes.
   */
  buildRuler(columns) {
    if (this.ruler) this.ruler.remove();
    const r = this.makeRow(2, columns);
    r.classList.add('tk-ruler');
    // The classes that decide the two width classes, applied to the exemplars rather than
    // inferred: `no-lane` is what actually removes the bar, so the exemplar must wear it.
    r._lanes[1].classList.add('no-lane');
    this.ruler = r;
    this._rulerBar = r._lanes[0];
    this._rulerPlain = r._lanes[1];
    this.host.appendChild(r);
  }

  /**
   * The two non-zero width classes, in four box reads, on shape change only.
   *
   * Returns false when nothing moved, so a caller can skip the prefix sum — and false when the
   * surface is hidden, keeping the last real measurement. Stale class widths are survivable in
   * a way stale per-track geometry never was: a class is (a track with a bar, a track without),
   * and neither can change while the surface is not on screen.
   */
  measureClasses() {
    if (!this._rulerPlain) return false;
    const box = this.host.getBoundingClientRect();
    if (box.width < 1 || box.height < 1) return false;
    const plain = this._rulerPlain, bar = this._rulerBar;
    const wp = plain.offsetWidth, wb = bar.offsetWidth;
    const lbEl = bar.firstElementChild;
    const wl = lbEl ? lbEl.offsetWidth : 0;
    if (!wp || !wb) return false;
    if (wp === this._wPlain && wb === this._wBar && wl === this._wLaneBar) return false;
    this._wPlain = wp; this._wBar = wb; this._wLaneBar = wl;
    return true;
  }

  /** The width of track `t`, from its class. Defined for every track, drawn or not. */
  trackWidthOf(t) {
    if (this._laneHidden && this._laneHidden[t]) return 0;
    return (this._laneShow && this._laneShow[t]) ? this._wBar : this._wPlain;
  }

  makeRow(trackCount, columns) {
    const row = el('div', 'tk-row');
    const g = el('div', 'tk-gutter');
    g.appendChild(document.createTextNode(''));
    row.append(g);
    // The harmony column is a LANE, not a cell — see paintHarmony. The row still
    // reserves its width so the tracks line up with the header.
    row.append(el('div', 'tk-harm-spacer'));
    /**
     * The row's cells, flat, in view-model order (track * columns + col).
     *
     * Everything that walks a row used to walk `elm.children`, skip the two
     * non-track children with a `classList.contains('tk-track')` test, then walk
     * `tr.children` — nine HTMLCollection iterators per row, per frame, in
     * bindRow, and the same again in paintSelection. That is ~600 iterator
     * objects a frame to re-derive a list whose contents are fixed the moment
     * the row is built. Building it once here makes both an index loop, and
     * incidentally makes cellEl a subscript instead of a querySelector.
     */
    /*
     * THE TWO SPACERS that will let this row hold only the tracks on screen.
     *
     * The plan this came from wanted `.tk-track` taken out of flex flow and positioned
     * absolutely from the model. That works, and it costs more than it needs to: the row's
     * gutter is `position: sticky`, which wants a flow context, and `scrollWidth` — which the
     * scroll clamp reads — collapses the moment the tracks stop being in flow.
     *
     * A leading spacer sized to `trackLeft[first]` and a trailing one covering the rest keeps
     * ALL of that working with no JS scroll interception at all: flex still lays the row out,
     * the gutter still sticks, and `scrollWidth` still equals the strip's true extent because
     * the spacers are exactly the tracks that are not there.
     *
     * Zero-width today — every track is still rendered — so this commit changes nothing it can
     * be blamed for. That is the point of landing them separately: if the sticky gutter or the
     * scroll extent were going to object, they object now, against a tree where the goldens are
     * byte-identical and the only variable is two empty divs.
     */
    const lead = el('div', 'tk-spacer tk-spacer-lead');
    const tail = el('div', 'tk-spacer tk-spacer-tail');
    row.append(lead);
    row._lead = lead;
    row._tail = tail;
    row._leadW = -1;
    row._tailW = -1;

    const cells = new Array(trackCount * columns);
    let k = 0;
    const laneBars = new Array(trackCount);
    const laneEls = new Array(trackCount);
    for (let t = 0; t < trackCount; t++) {
      const tr = el('div', 'tk-track');
      /*
       * THE TRACK'S IDENTITY, on the element.
       *
       * Everything that wanted a track used to find it by POSITION —
       * `.tk-track:nth-child(n + 3)`, which bakes in "gutter, harm-spacer, then tracks" — and
       * that assumption broke the moment a spacer was added in front. It will break far worse
       * when the row holds only the tracks on screen, where the third child might be track 9.
       *
       * This is the absolute track index and never a render slot. Nothing outside this file may
       * learn a slot: `trackAt` and `hitTest` return track ids, and this attribute is how a
       * test or an agent asks the same question of the DOM.
       */
      tr.dataset.track = String(t);
      tr.style.setProperty('--tint', `var(--uni-track-tint-${t % 8})`);
      /**
       * The lane's clip-local bar:beat readout — the FIRST child of the track,
       * before its cells.
       *
       * Deliberately not a cell and deliberately not in `row._cells`. That array
       * is what paintSelection sweeps by flat field index and what cellEl
       * subscripts as `track * cols + col`; putting a fourth element per track in
       * it would shift every field index in the program. Staying out of it is what
       * keeps `state.columns` at 3, so the cursor, the selection and the clipboard
       * are untouched by this feature.
       *
       * No `data-col`, and not `.tk-cell`: anything that finds a cell by those
       * must not find this. It is a readout — there is nothing to type into it.
       */
      const lb = el('div', 'tk-lane-bar');
      lb.dataset.track = String(t);
      lb.appendChild(document.createTextNode(''));
      lb._text = lb.firstChild;
      lb._accV = -1;
      tr.append(lb);
      laneBars[t] = lb;
      laneEls[t] = tr;
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
        cell._text = cell.firstChild;
        cell._bar = bar;
        cell._kindV = null;
        cell._devV = -1; cell._devOutV = 0;
        tr.append(cell);
        cells[k++] = cell;
      }
      row.append(tr);
    }
    row.append(tail);
    row._cells = cells;
    row._laneBars = laneBars;
    row._lanes = laneEls;
    row._shown = 1;
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

    /**
     * Each lane's own bar:beat, and its own accents.
     *
     * Guarded on both, like every other write here: the readout changes on a
     * handful of rows per scroll, and rebinding a Text node that already says the
     * right thing invalidates style and paint for nothing.
     *
     * The accent goes on the LANE element, not on the row: that is the whole point
     * of it. A row is a bar in the song's meter; a lane is a bar in its clip's, and
     * in a polyrhythm those are different rows. The classes are toggled from a
     * cached number so a lane that did not change costs one integer compare.
     */
    const lanes = elm._laneBars, lel = elm._lanes;
    const ln = Math.min(lanes.length, row.laneBar.length);
    for (let t = 0; t < ln; t++) {
      const lb = lanes[t];
      const txt = row.laneBar[t];
      if (lb._text.nodeValue !== txt) lb._text.nodeValue = txt;
      const acc = row.laneAcc[t];
      if (lb._accV !== acc) {
        lb._accV = acc;
        const e = lel[t];
        e.classList.toggle('lbar', (acc & 1) !== 0);
        e.classList.toggle('lbeat', (acc & 2) !== 0 && (acc & 1) === 0);
      }
    }

    const cells = elm._cells;
    const n = Math.min(cells.length, row.cells.length);
    for (let i = 0; i < n; i++) {
      const cell = cells[i];
      const c = row.cells[i];

      /**
       * Where in the row the note actually sounds.
       *
       * Drawn with a pseudo-element rather than a real one: this would otherwise
       * be 768 more DOM nodes on a 16-track viewport, for a mark that is absent
       * from most cells most of the time. `--dev` is the only thing written, and
       * the percentage strings are interned — a style write per cell per frame to
       * produce one of a hundred possible strings is the shape GUIDELINES 3 exists
       * to catch.
       *
       * Guarded on the integer, so a cell whose note has not moved costs one
       * compare. The class carries whether there is a mark at all, so the common
       * case — a note exactly on its row — writes nothing.
       */
      if (cell._devV !== c.dev) {
        const had = cell._devV >= 0;
        cell._devV = c.dev;
        if (c.dev >= 0) {
          cell.style.setProperty('--dev', DEV_PCT[c.dev]);
          if (!had) cell.classList.add('dev');
        } else if (had) {
          cell.classList.remove('dev');
        }
      }
      /*
       * ...and whether it sounds outside this row at all, which quantize made
       * reachable: a grid coarser than the row can pull a note to a line before
       * its own row. `--dev` pins to the edge there, and this is what stops the
       * pin reading as "on time". Its own cached integer, because it changes far
       * less often than the position does.
       */
      if (cell._devOutV !== c.devOut) {
        cell._devOutV = c.devOut;
        cell.classList.toggle('dev-early', c.devOut < 0);
        cell.classList.toggle('dev-late', c.devOut > 0);
      }

      const bar = cell._bar;
      /**
       * The pitch mark. Two things share one pooled element because they are the
       * same statement at two zooms: "the register here is THIS high".
       *
       * At a coarse zoom a row spans many notes, so the mark is the RANGE the
       * engine aggregated. At a fine zoom a row holds one note, so it is that
       * note — and down a column the marks form the melodic contour, which is
       * what ARCHITECTURE_REVIEW Movement 1 item 14 asks for: shape and register
       * collisions visible without reading a single note name.
       *
       * MIDI 24..96 covers the useful range, clamped rather than scaled to what
       * is on screen — so a mark means the same pitch height on every row of
       * every track, which is the whole basis for comparing two columns by eye.
       * Scaling to the visible window would make the same note sit at different
       * heights depending on what else happened to be nearby.
       */
      let markLo = -1, markHi = -1, markOp = 0;
      /*
       * A COLLIDED CELL IS NOT AN AGGREGATE, and its opacity must not be read as
       * one.
       *
       * For a span summary, opacity encodes DENSITY — a bar holding forty notes is
       * more opaque than one holding two. For a cell holding three notes on one row,
       * the same formula gives 0.37: FAINTER than the single note beside it, which
       * reads exactly backwards. Three notes are more, not less.
       *
       * So a collision draws at full strength, with the spread of the pitches
       * actually in it. Its own branch, before the aggregate one, because the two
       * share `aggCount` and mean different things by it.
       */
      if (c.kind === 'collide') {
        markLo = c.aggLo; markHi = c.aggHi;
        markOp = 1;
      } else if (c.aggCount) {
        markLo = c.aggLo; markHi = c.aggHi;
        markOp = Math.min(1, 0.25 + c.aggCount / 24);
      } else if (c.pitch >= 0) {
        markLo = c.pitch;
        markHi = c._hiPitch === undefined ? c.pitch : c._hiPitch;
        markOp = 1;
      }
      if (markLo >= 0) {
        const lo = Math.max(0, Math.min(1, (markLo - 24) / 72));
        const hi = Math.max(0, Math.min(1, (markHi - 24) / 72));
        const bottom = (lo * 100) | 0;
        // A single note has lo === hi, so without a floor the mark is zero-height
        // and the ribbon is invisible at exactly the zoom you read it at. 8% of a
        // 17px row is about a pixel and a half: a tick, not a bar.
        const height = Math.max(8, ((hi - lo) * 100) | 0);
        if (bar._b !== bottom) { bar._b = bottom; bar.style.bottom = bottom + '%'; }
        if (bar._h !== height) { bar._h = height; bar.style.height = height + '%'; }
        if (bar._o !== markOp) { bar._o = markOp; bar.style.opacity = markOp; }
        if (bar._on !== 1) { bar._on = 1; bar.style.display = 'block'; }
      } else if (bar._on !== 0) { bar._on = 0; bar.style.display = 'none'; }
      const tn = cell._text;
      if (tn.nodeValue !== c.text) tn.nodeValue = c.text;
      // Cached beside the attribute rather than read back off it: `dataset.kind`
      // is a DOMStringMap lookup per cell per rebind, and there are 1,776 cells.
      if (cell._kindV !== c.kind) { cell._kindV = c.kind; cell.dataset.kind = c.kind; }
    }
  }

  /** @param {ReturnType<import('./viewmodel.js').buildViewModel>} vm */
  render(vm) {
    const prev = this.vm;
    this.vm = vm;
    const first = vm.window.startRow;

    // Which lanes carry a readout, BEFORE anything is positioned: it changes track
    // widths, and every cell position, the hit test and the scroll clamp are
    // derived from those. Guarded on a bitmask, so a scroll costs one compare.
    const laneChanged = this.applyLaneShow(vm.laneShow, vm.laneShowSig | 0,
                                           vm.laneHidden, vm.laneHiddenSig | 0);

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
    const visible = Math.min(vm.rows.length,
                             Math.ceil((this._viewH || 0) / this.m.rowHeight) + 1);
    const limit = needFull ? visible : vm.rows.length;

    // How many pool slots this frame's window claims. Kept because rowEl() needs
    // the same bound to answer "is this row on screen", and recomputing it there
    // would be two places to get the edge case wrong.
    this._live = Math.min(vm.rows.length, n);
    for (let i = 0; i < this._live; i++) {
      const rowIdx = first + i;
      const row = vm.rows[i];
      const elm = this.pool[((rowIdx % n) + n) % n];
      if (elm._shown !== 1) { elm._shown = 1; elm.style.display = ''; }
      if (i < limit && (needFull || elm._row !== rowIdx)) this.bindRow(elm, row);
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
      if (elm._shown !== 0) { elm._shown = 0; elm.style.display = 'none'; }
    }

    // Kept POSITIVE and negated inside the string. `-(0)` is `-0`, which is not a
    // small integer, so V8 boxes a heap number for it — on every frame of a view
    // that has not moved, which is most of them. Storing the distance and
    // spelling the direction in the template costs nothing and allocates
    // nothing.
    const sx = vm.scrollX || 0, sy = first * this.m.rowHeight;
    if (this._sx !== sx || this._sy !== sy) {
      this._sx = sx; this._sy = sy;
      const xf = 'translate(-' + sx + 'px, -' + sy + 'px)';   // only on movement
      this.band.style.transform = xf;
      this.rails.style.transform = xf;
    }
    // The lane moves vertically with the rows and never horizontally.
    if (this._hy !== sy) {
      this._hy = sy;
      this.harmLane.style.transform = 'translateY(-' + sy + 'px)';
    }
    this.paintHarmony(vm, first);
    this.paintClips(vm);
    this.paintState(vm, prev);
  }

  /**
   * Harmony fields, as blocks that span their extent.
   *
   * The label is STICKY: a field scrolled partway off the top keeps its name on
   * the first visible row, because the question it answers — what key is this —
   * does not stop mattering once you have scrolled past the change. The footer
   * does the same at the bottom. `first` is the band's top row, which is what
   * the lane's transform is against, so the clamp is in the same space as the
   * geometry rather than in viewport pixels that would need a second reading of
   * the layout.
   */
  paintHarmony(vm, first) {
    const perRow = vm.zoom.rowNanoticks;
    const rh = this.m.rowHeight;
    const viewTop = first * rh;
    const viewBottom = viewTop + this.poolSize * rh;
    for (let i = 0; i < vm.harmony.length; i++) {
      const b = vm.harmony[i];
      let e = this.harmPool[i];
      if (!e) {
        e = el('div', 'tk-hb');
        const lab = el('div', 'tk-hb-label');
        const k = el('span', 'tk-hb-key'); k.appendChild(document.createTextNode(''));
        const s = el('span', 'tk-hb-sub'); s.appendChild(document.createTextNode(''));
        lab.append(k, s);
        const foot = el('div', 'tk-hb-foot'); foot.appendChild(document.createTextNode(''));
        e.append(lab, foot);
        e._k = k.firstChild; e._s = s.firstChild; e._f = foot.firstChild;
        e._lab = lab; e._foot = foot;
        e._shown = 1;
        this.harmLane.append(e);
        this.harmPool[i] = e;
      }
      if (e._shown !== 1) { e._shown = 1; e.style.display = ''; }
      const top = (b.startTick / perRow) * rh;
      const height = Math.max(rh, ((b.endTick - b.startTick) / perRow) * rh);
      if (e._t !== top) { e._t = top; e.style.top = top + 'px'; }
      if (e._h !== height) { e._h = height; e.style.height = height + 'px'; }
      if (e._kv !== b.label) { e._kv = b.label; e._k.nodeValue = b.label; }
      if (e._sv !== b.sub) { e._sv = b.sub; e._s.nodeValue = b.sub; }
      if (e._fv !== b.foot) { e._fv = b.foot; e._f.nodeValue = b.foot; }
      // Offsets that keep both ends of the block inside the viewport.
      const lo = Math.max(0, viewTop - top);
      const fo = Math.max(0, (top + height) - viewBottom);
      if (e._lo !== lo) { e._lo = lo; e._lab.style.transform = 'translateY(' + lo + 'px)'; }
      if (e._fo !== fo) { e._fo = fo; e._foot.style.transform = 'translateY(' + (-fo) + 'px)'; }
    }
    for (let i = vm.harmony.length; i < this.harmPool.length; i++) {
      const e = this.harmPool[i];
      if (e._shown !== 0) { e._shown = 0; e.style.display = 'none'; }
    }
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
        const lab = el('span', 'tk-rail-name');
        lab.appendChild(document.createTextNode(''));
        r.append(lab);
        r._label = lab.firstChild;
        r._name = null; r._hue = -1; r._shown = 1;
        this.rails.append(r);
        this.railPool[i] = r;
      }
      if (r._id !== clip.id) { r._id = clip.id; r.dataset.clip = clip.id; }
      if (r._shown !== 1) { r._shown = 1; r.style.display = ''; }
      if (r._active !== clip.active) { r._active = clip.active; r.classList.toggle('active', clip.active); }
      if (r._name !== clip.name) { r._name = clip.name; r._label.nodeValue = clip.name; }
      // The track's own hue, so a clip is findable across the width of the
      // screen. Eight shades of one accent are not.
      //
      // From a table: this was concatenated per rail per frame and only then
      // compared against the cached copy, which at eight visible clips was the
      // single largest per-frame allocation in the tracker — ~600 bytes a frame
      // to rediscover one of eight strings that were fixed at load.
      const hueIdx = clip.track % 8;
      if (r._hue !== hueIdx) { r._hue = hueIdx; r.style.setProperty('--clip', TRACK_HUES[hueIdx]); }
      // 2px inset at each end, so consecutive clips show a gap rather than one
      // unbroken bar — the boundary between two clips is information.
      const top = (clip.startTick / perRow) * this.m.rowHeight + 2;
      const height = Math.max(10,
        ((clip.endTick - clip.startTick) / perRow) * this.m.rowHeight - 4);
      // Tucked inside the track's LAST column rather than sitting on the border,
      // where it would be indistinguishable from the rule already there.
      /**
       * The rail sits at the track's own RIGHT edge, from its measured box.
       *
       * This was `clip.track * trackStride + trackStride`, which is only the right
       * edge while every track is the same width — and they have not been since a
       * lane started carrying a bar readout only when its bars differ from the
       * song's. Measured on the clip-meters fixture, whose widths are
       * 270/270/230/270/230: rails landed at 766/1036/1306/1576/1846 against real
       * right edges of 773/1043/1273/1543/1773, so track 2's rail was drawn 33px
       * INSIDE track 3's lane and track 4's fell 73px past the end of the strip
       * and was clipped away entirely — that clip simply had no rail.
       *
       * Every other consumer of this geometry moved to the per-track arrays; this
       * one line did not, and no assertion looked at a rail's x.
       */
      const measured = this.trackLeft && clip.track < this.trackCountMeasured;
      const tLeft = measured ? this.trackLeft[clip.track] : clip.track * this.trackStride;
      const tWidth = measured ? this.trackWidth[clip.track] : this.trackStride;
      // A folded lane has no width, so it has nowhere to put a rail. Hidden rather
      // than positioned: at width 0 the expression below lands 7px into the lane
      // to its LEFT, which draws a sliver belonging to a track you cannot see.
      if (measured && tWidth === 0) { if (r._shown !== 0) { r._shown = 0; r.style.display = 'none'; } continue; }
      const left = this.stripLeft + tLeft + tWidth - 7;
      const width = 5;
      if (r._top !== top) { r._top = top; r.style.top = top + 'px'; }
      if (r._h !== height) { r._h = height; r.style.height = height + 'px'; }
      if (r._l !== left) { r._l = left; r.style.left = left + 'px'; }
      if (r._w !== width) { r._w = width; r.style.width = width + 'px'; }
    }
    // Surplus rails are hidden, not removed — the pool high-water-marks and
    // then stops mutating the DOM entirely.
    for (let i = vm.clips.length; i < this.railPool.length; i++) {
      const r = this.railPool[i];
      if (r._shown !== 0) { r._shown = 0; r.style.display = 'none'; }
    }
  }

  /**
   * The pooled element holding an absolute row, or null when that row is outside
   * the window this frame.
   *
   * This replaces a `Map<rowIdx, element>` that was cleared and refilled with
   * ~74 entries on every frame — and, worse, was iterated with
   * `for (const [rowIdx, elm] of map)`, which allocates a two-element array per
   * entry per frame on top of the iterator. The map was a cache for a formula:
   * the pool is a ring indexed by `row mod poolSize`, which is the same
   * arithmetic render() uses to fill it. Recomputing costs a modulo.
   */
  rowEl(rowIdx) {
    const vm = this.vm;
    if (!vm) return null;
    const i = rowIdx - vm.window.startRow;
    if (i < 0 || i >= this._live) return null;
    const n = this.pool.length;
    return n ? this.pool[((rowIdx % n) + n) % n] : null;
  }

  /**
   * Cursor / playhead / selection: a handful of class toggles, never a rebind.
   *
   * REMOVED THROUGH THE ELEMENT IT WAS ADDED TO, not through a recomputed one.
   *
   * `rowEl`/`cellEl` turn an absolute row into a pooled element by `row mod
   * poolSize`, and return null when that row is OUTSIDE the current window. So
   * un-painting the previous cursor by looking its row up again failed the moment
   * the window moved past it: the class was never removed, and because the pool is
   * a ring that element is now showing some other row entirely.
   *
   * Measured: cursor parked on row 5, scroll away, and the highlight is drawn on
   * ROW 435 while the cursor is still at 5. A phantom cursor on an unrelated row is
   * worse than no cursor — the whole job of that highlight is to say where you are,
   * and there is nothing about it that looks wrong.
   *
   * Holding the element costs two references and cannot go stale in a way that
   * matters: if the pool rebinds it to another row, removing the class is exactly
   * what we want; if it is gone, the reference is harmless.
   */
  paintState(vm, prev) {
    if (this._phEl) { this._phEl.classList.remove('playhead'); this._phEl = null; }
    if (this._curEl) { this._curEl.classList.remove('cursor'); this._curEl = null; }
    this._phEl = this.rowEl(vm.playhead.row) || null;
    if (this._phEl) this._phEl.classList.add('playhead');
    this._curEl = this.cellEl(vm.cursor) || null;
    if (this._curEl) this._curEl.classList.add('cursor');
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
    // Nothing selected now and nothing selected last frame: there is no cell
    // whose membership can have changed, so the whole 1,776-cell sweep is
    // skipped. Selection is null for almost the entire life of the program —
    // this loop was running sixty times a second to set `false` on cells that
    // were already false. `_hadSel` rather than a compare against the previous
    // view-model's selection, because a drag that ends replaces the object with
    // null and the frame that does so still has to clear the classes.
    if (!s && !this._hadSel) return;
    this._hadSel = !!s;
    const first = vm.window.startRow;
    const n = this.pool.length;
    for (let i = 0; i < this._live; i++) {
      const rowIdx = first + i;
      const elm = this.pool[((rowIdx % n) + n) % n];
      const inRows = !!s && rowIdx >= s.r0 && rowIdx <= s.r1;
      const cells = elm._cells;
      for (let f = 0; f < cells.length; f++) {
        const cell = cells[f];
        const on = inRows && f >= s.f0 && f <= s.f1;
        if (cell._sel !== on) { cell._sel = on; cell.classList.toggle('sel', on); }
      }
    }
  }

  /** The cell at a (row, track, col) cursor. A subscript into the row's flat
   *  cell list — it was a `querySelector` with a template-literal selector, so
   *  every call built a string and made the engine parse it afresh. */
  cellEl(cur) {
    const row = this.rowEl(cur.row);
    if (!row) return null;
    return row._cells[cur.track * this.cols + cur.col] || null;
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
    const track = this.trackAt(rel);
    if (track < 0 || track >= this.tracks) return null;
    // A click on the bar readout is not a click on a cell. Returning null here is
    // the same answer the gutter already gives — there is nothing to type into a
    // readout, and resolving it to column 0 would put the cursor a column to the
    // left of wherever the user actually pointed.
    const inTrack = rel - this.trackLeft[track];
    if (inTrack < this.laneBarW[track]) return null;
    const col = Math.min(this.cols - 1,
                         Math.floor((inTrack - this.laneBarW[track]) / this.m.cellWidth));
    return { row, track, col };
  }

  /** What an agent (or a test) asks. Structure, not pixels. */
  probe() {
    return {
      /*
       * THE WIDTH MODEL, exposed so a test can pin it against the rendered box.
       *
       * This is the assertion that makes JS-owned geometry safe: the old code measured every
       * track, so paint and hit-test could not disagree by construction. Now they could, and
       * the only thing standing between "could" and "does" is a check that runs on every scene
       * — including the ragged ones, where a per-class model and a single stride give different
       * answers.
       */
      stripLeft: this.stripLeft,
      contentWidth: this.contentWidth,
      widthClasses: { plain: this._wPlain, bar: this._wBar, laneBar: this._wLaneBar },
      trackGeom: (t) => ({
        left: this.trackLeft ? this.trackLeft[t] : 0,
        width: this.trackWidth ? this.trackWidth[t] : 0,
        laneBarW: this.laneBarW ? this.laneBarW[t] : 0,
      }),
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
