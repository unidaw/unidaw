// The tracker's minimap: the whole song as a 30px column pinned to the right of
// the tracker pane, outside the horizontally scrolled strip.
//
// CANVAS, not pooled DOM, and the reasoning is the opposite of scope.js's.
// scope.js is a canvas because a scope redraws every pixel every frame; this
// redraws rarely. It is a canvas because of what a mark IS. The column holds up
// to 256 density marks, and a mark carries no text, no identity and nothing to
// click an individual one of — it is a rectangle and an alpha. As DOM that is
// 256 pooled divs whose only per-draw write is `style.opacity`, and every one of
// those writes stringifies a number (GUIDELINES 3, the table: an unguarded or
// stringifying style write is the regression this project measures for). As
// canvas it is `ctx.globalAlpha = a; ctx.fillRect(...)` — four numbers, no
// string, no node, one element in the tree instead of 258.
//
// The cost of a canvas here is the one GUIDELINES 6 names: pixels are opaque to
// an agent where a DOM tracker's 1,024 cells are readable. That is paid back
// directly — probe() returns the model's own numbers AND a column of alphas read
// back out of the canvas with getImageData, so a test can assert that what was
// computed is what was painted. That readback is the thing a DOM minimap would
// have given for free, and it is the only thing.
//
// Everything else follows the same rules as the DOM renderers: nothing allocated
// per draw, every write guarded by a cached number, and a draw skipped entirely
// when neither the marks nor the viewport moved.

import { tickAtFraction } from './minimapmodel.js';

/** Insets measured off design/redesign/Uni.dc.html at 1680x1000, in CSS px, out
 *  of a 29px content box: marks left/right 4 (21 wide), the viewport rectangle
 *  left/right 1 (27 wide) with a 2px radius and a 1px inset ring, the playhead
 *  the full width at 2px tall. */
const MARK_INSET = 4;
const VIEW_INSET = 1;
const VIEW_RADIUS = 2;
const VIEW_RING = 1;
const HEAD_HEIGHT = 2;

/** The design's viewport wash: rgba(145,132,217,0.06) over the column ground. */
const VIEW_FILL_ALPHA = 0.06;

/**
 * The shortest the viewport rectangle is allowed to draw. At 1/48 zoom on a
 * 512-bar song the visible window is a quarter of one percent of the column —
 * under two pixels, and a rectangle thinner than its own ring is a rectangle
 * nobody can find. Better slightly wrong about the extent than invisible.
 */
const MIN_VIEW_PX = 4;

export class Minimap {
  /**
   * @param {HTMLElement} host
   * @param {{onSeek?:(tick:number)=>void}} opts
   *
   * `onSeek` is optional and nothing is bound without it — a column that follows
   * the pointer but moves nothing is the silently-ignoring control GUIDELINES 4.5
   * rules out, and the same pattern the other panels use for their callbacks.
   */
  constructor(host, { onSeek } = {}) {
    this.host = host;
    this.host.className = 'mm';
    this.onSeek = onSeek || null;

    this.canvas = document.createElement('canvas');
    this.canvas.className = 'mm-canvas';
    host.appendChild(this.canvas);
    this.ctx = this.canvas.getContext('2d', { alpha: false });

    this.vm = null;
    this.w = 0; this.h = 0; this.dpr = 0;
    this.draws = 0;
    this.themed = false;
    this.colors = { bg: '', mark: '', view: '', head: '' };
    this.readTheme();

    // Layout reads are the one thing here that costs a synchronous layout, so
    // they happen on resize and never in the draw (GUIDELINES 3.11). The
    // observer only sets a flag; the measurement happens inside the next draw,
    // where it is already a layout boundary.
    this._needSize = true;
    this._ro = new ResizeObserver(() => { this._needSize = true; });
    this._ro.observe(host);

    // Guards. Every one is a number or a boolean, compared before anything is
    // written — a redraw of an unchanged column is a full canvas repaint.
    this._rev = -1; this._top = -1; this._vh = -1; this._ph = -1; this._known = null;

    if (this.onSeek) {
      this._down = false;
      this._dragTop = 0; this._dragH = 0;
      host.addEventListener('pointerdown', (e) => {
        // The box is measured ONCE per drag, not per move: getBoundingClientRect
        // both forces layout and hands back a fresh DOMRect, and a scrub emits
        // moves at pointer rate. The column cannot move while a pointer is
        // captured on it.
        const r = host.getBoundingClientRect();
        this._dragTop = r.top; this._dragH = r.height;
        if (this._dragH <= 0) return;
        this._down = true;
        host.setPointerCapture(e.pointerId);
        this._seek(e);
      });
      host.addEventListener('pointermove', (e) => { if (this._down) this._seek(e); });
      host.addEventListener('pointerup', (e) => {
        this._down = false;
        if (host.hasPointerCapture(e.pointerId)) host.releasePointerCapture(e.pointerId);
      });
    }
  }

  /**
   * The column's colours, from the stylesheet rather than repeated here — same
   * as scope.js. An empty value means tokens.css is not loaded, and then nothing
   * is drawn and `themed` says so: a canvas is the one surface where a missing
   * colour would otherwise paint plausible black rectangles.
   */
  readTheme() {
    const s = getComputedStyle(this.host);
    const pick = (n) => (s.getPropertyValue(n) || '').trim();
    this.colors.bg = pick('--uni-minimap-bg');
    this.colors.mark = pick('--uni-minimap-mark');
    this.colors.view = pick('--uni-minimap-viewport');
    this.colors.head = pick('--uni-minimap-playhead');
    this.themed = !!(this.colors.bg && this.colors.mark && this.colors.view && this.colors.head);
    this._rev = -1;                      // colours changed: the last draw is stale
  }

  /** Pointer y to a tick, via the model's own inverse projection — so the
   *  renderer never has to know what a tick is. */
  _seek(e) {
    const vm = this.vm;
    if (!vm) return;
    this.onSeek(tickAtFraction(vm, (e.clientY - this._dragTop) / this._dragH));
  }

  /** Match the backing store to the box and the display. Layout-reading, so it
   *  runs only when the observer or the display says something moved. */
  _size() {
    const dpr = window.devicePixelRatio || 1;
    if (!this._needSize && dpr === this.dpr) return false;
    this._needSize = false;
    const w = this.host.clientWidth, h = this.host.clientHeight;
    if (w === this.w && h === this.h && dpr === this.dpr) return false;
    this.w = w; this.h = h; this.dpr = dpr;
    this.canvas.width = Math.max(1, Math.round(w * dpr));
    this.canvas.height = Math.max(1, Math.round(h * dpr));
    return true;
  }

  render(vm) {
    this.vm = vm;
    const resized = this._size();
    // A host constructed before it was put in the document has no computed style
    // yet, so the read in the constructor comes back empty and a canvas that
    // never retries stays blank forever with nothing to see or log. Retrying
    // while unthemed costs one style read per frame in a state that resolves on
    // the first attached draw — or never, if tokens.css is genuinely missing,
    // which `themed` reports.
    if (!this.themed) this.readTheme();
    if (!this.themed) return;
    if (!resized && this._rev === vm.revision && this._top === vm.viewTop
        && this._vh === vm.viewHeight && this._ph === vm.playhead
        && this._known === vm.known) return;
    this._rev = vm.revision; this._top = vm.viewTop; this._vh = vm.viewHeight;
    this._ph = vm.playhead; this._known = vm.known;

    const ctx = this.ctx;
    const W = this.canvas.width, H = this.canvas.height;
    const d = this.dpr;
    this.draws++;

    ctx.globalAlpha = 1;
    ctx.fillStyle = this.colors.bg;
    ctx.fillRect(0, 0, W, H);

    // Density. Marks tile the column with no gaps, so each one's bottom edge is
    // the next one's top — rounded once per boundary rather than per rectangle,
    // or the seams show as light and dark hairlines.
    const n = vm.markCount;
    if (n > 0) {
      const x = Math.round(MARK_INSET * d);
      const mw = Math.max(1, W - 2 * x);
      ctx.fillStyle = this.colors.mark;
      let y0 = 0;
      for (let i = 0; i < n; i++) {
        const y1 = Math.round(((i + 1) * H) / n);
        const a = vm.marks[i];
        // The design rounds a mark's corners by 1px. Dropped deliberately: the
        // marks are contiguous, so a radius only notches the joins between
        // them, and at 512 bars a mark is under three pixels tall.
        if (a > 0) {
          ctx.globalAlpha = a;
          ctx.fillRect(x, y0, mw, Math.max(1, y1 - y0));
        }
        y0 = y1;
      }
      ctx.globalAlpha = 1;
    }

    // The viewport rectangle: a wash and an inset ring, drawn inside its own box
    // so the ring lands on whole device pixels.
    const vx = Math.round(VIEW_INSET * d);
    const vw = Math.max(1, W - 2 * vx);
    const vh = Math.max(Math.round(MIN_VIEW_PX * d), Math.round(vm.viewHeight * H));
    const vy = Math.min(Math.round(vm.viewTop * H), Math.max(0, H - vh));
    const ring = Math.max(1, Math.round(VIEW_RING * d));
    ctx.beginPath();
    ctx.roundRect(vx + ring / 2, vy + ring / 2, vw - ring, vh - ring, VIEW_RADIUS * d);
    ctx.globalAlpha = VIEW_FILL_ALPHA;
    ctx.fillStyle = this.colors.mark;
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.strokeStyle = this.colors.view;
    ctx.lineWidth = ring;
    ctx.stroke();

    // The playhead spans the full width, unlike everything above it, because it
    // is the one line that has to be findable at a glance.
    const hh = Math.max(1, Math.round(HEAD_HEIGHT * d));
    const hy = Math.min(Math.round(vm.playhead * H), Math.max(0, H - hh));
    ctx.fillStyle = this.colors.head;
    ctx.fillRect(0, hy, W, hh);
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    const marks = [];
    const counts = [];
    for (let i = 0; i < vm.markCount; i++) { marks.push(vm.marks[i]); counts.push(vm.counts[i]); }
    return {
      known: vm.known,
      themed: this.themed,
      draws: this.draws,
      width: this.w, height: this.h, dpr: this.dpr,
      deviceWidth: this.canvas.width, deviceHeight: this.canvas.height,
      markCount: vm.markCount,
      beatsPerMark: vm.beatsPerMark,
      ticksPerMark: vm.ticksPerMark,
      songTicks: vm.songTicks,
      songBeats: vm.songBeats,
      contentTicks: vm.contentTicks,
      events: vm.events,
      dropped: vm.dropped,
      peak: vm.peak,
      saturated: vm.saturated,
      revision: vm.revision,
      viewTop: vm.viewTop,
      viewHeight: vm.viewHeight,
      beyond: vm.beyond,
      playhead: vm.playhead,
      playheadTick: vm.playheadTick,
      // Geometry in CSS pixels, so a test can assert the rectangle tracks a
      // scroll without reading the DOM or trusting the fractions above.
      viewRect: { y: vm.viewTop * this.h, h: Math.max(MIN_VIEW_PX, vm.viewHeight * this.h) },
      playheadY: vm.playhead * this.h,
      marks,
      counts,
      // What was actually painted, sampled down the middle of the mark band, one
      // reading per mark. This is the canvas paying back the legibility a DOM
      // surface would have given for free (GUIDELINES 6) — it is off the draw
      // path, so allocating here is free.
      painted: this._paintedColumn(vm),
    };
  }

  /**
   * Read one pixel per mark out of the backing store and turn it back into the
   * alpha it was drawn with, so `painted[i]` can be compared to `marks[i]`.
   *
   * `null` where the viewport rectangle or the playhead is drawn over the mark
   * band. Those are opaque enough to swamp the mark underneath — the first read
   * of this returned 1.0 for mark 0 because the viewport ring sat on it — and
   * reporting a number there would be reporting the overlay, not the density.
   * The overlays have their own entries in the probe; this one is about marks.
   */
  _paintedColumn(vm) {
    const out = [];
    const W = this.canvas.width, H = this.canvas.height;
    if (!this.themed || W < 1 || H < 1 || !vm.markCount) return out;
    // Just inside the mark band, where only marks are drawn.
    const x = Math.min(W - 1, Math.round((MARK_INSET + 2) * this.dpr));
    const strip = this.ctx.getImageData(x, 0, 1, H).data;
    const bg = this._rgb(this.colors.bg);
    const mark = this._rgb(this.colors.mark);
    if (!bg || !mark) return out;
    const d = this.dpr;
    const vh = Math.max(Math.round(MIN_VIEW_PX * d), Math.round(vm.viewHeight * H));
    const vy = Math.min(Math.round(vm.viewTop * H), Math.max(0, H - vh));
    const hh = Math.max(1, Math.round(HEAD_HEIGHT * d));
    const hy = Math.min(Math.round(vm.playhead * H), Math.max(0, H - hh));
    for (let i = 0; i < vm.markCount; i++) {
      const y = Math.min(H - 1, Math.floor(((i + 0.5) * H) / vm.markCount));
      if ((y >= vy && y < vy + vh) || (y >= hy && y < hy + hh)) { out.push(null); continue; }
      // One channel is enough to invert the composite, and blue is the one the
      // accent and the ground differ most in.
      const a = (strip[y * 4 + 2] - bg[2]) / (mark[2] - bg[2]);
      out.push(Math.round(a * 1000) / 1000);
    }
    return out;
  }

  /** '#rrggbb' or 'rgb(r, g, b)' to [r,g,b]. Probe-only; never in a draw. */
  _rgb(s) {
    const hex = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(s);
    if (hex) return [parseInt(hex[1], 16), parseInt(hex[2], 16), parseInt(hex[3], 16)];
    const fn = /rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)/.exec(s);
    return fn ? [+fn[1], +fn[2], +fn[3]] : null;
  }
}
