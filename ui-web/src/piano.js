// Piano roll renderer. Same pooling and guarded-write discipline as the others.

// The wheel's unit conversion, shared with the arrangement and the tracker rather
// than re-derived here. A wheel does not speak one unit — see WHEEL_LINE_PX — and
// a third private copy of that rule is a third place for Firefox to be sixteen
// times slower than the machine it was written on.
import { wheelPixels } from './arrangemodel.js';

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

/**
 * The pool initialisers, at module scope because none of them reads anything
 * from the draw.
 *
 * Written inline in `render` they were five fresh closures per frame — the pool
 * only calls them when it grows, but the function object is built at the call
 * whether the pool grows or not, so a pool that had settled at its high-water
 * mark long ago still built five of them every 16 ms to hand them to a loop that
 * did not run. `_shown` is the element's inline display as a number: see the
 * comment on the display guard in `render`.
 */
function initKey(el) {
  el.appendChild(document.createTextNode(''));
  el._y = -1; el._black = null; el._label = null; el._shown = 1;
}
function initRow(el) { el._y = -1; el._shown = 1; }
function initTick(el) {
  el.appendChild(document.createTextNode(''));
  el._x = -1; el._bar = -1; el._shown = 1;
}
function initGridline(el) { el._x = -1; el._shown = 1; }
function initNote(el) {
  el._x = -1; el._y = -1; el._w = -1; el._h = -1; el._vel = -1; el._shown = 1;
}

/**
 * Every opacity string a velocity can produce, built once.
 *
 * `(0.35 + 0.65 * v / 127).toFixed(2)` ran unguarded once per note per frame —
 * a few hundred strings every 16 ms, computed only to discover that the note's
 * velocity had not changed since the last frame and neither had the string.
 * MIDI velocity is 0..127, so there are 128 possible answers and they can be
 * spelled out once at load; the per-frame work is then an integer compare.
 */
const VELOCITY_OPACITY = (() => {
  const a = new Array(128);
  for (let v = 0; v < 128; v++) a[v] = (0.35 + 0.65 * (v / 127)).toFixed(2);
  return Object.freeze(a);
})();

/**
 * Bar numbers as strings, memoised on the first frame that shows each one.
 *
 * The ruler pool is indexed by position rather than recycled as a ring, so
 * scrolling past one bar shifts every label along by one and rebuilds all of
 * them — up to twenty `String(bar)` calls a frame while a scroll is in flight,
 * spelling out numbers this ruler has already spelled out on the way past and
 * will spell out again on the way back. A bar number is a small integer whose
 * string can never be anything else, so an entry can never go stale. Bounded so
 * that scrolling to the far end of a very long project cannot grow the table
 * without limit; past the bound the string is built per use, as it was before.
 */
const BAR_LABELS = [];
const BAR_LABEL_MAX = 4096;
function barLabel(bar) {
  if (bar < 0 || bar > BAR_LABEL_MAX) return String(bar);
  return BAR_LABELS[bar] || (BAR_LABELS[bar] = String(bar));
}

export class Piano {
  constructor(host, { onNote, onSelect, onMarquee, onMarqueeEnd, onDrag, onDragEnd,
                      onNav } = {}) {
    this.host = host;
    this.host.className = 'pr';
    this.onNote = onNote; this.onSelect = onSelect;
    this.onMarquee = onMarquee; this.onMarqueeEnd = onMarqueeEnd;
    this.onDrag = onDrag; this.onDragEnd = onDragEnd;
    this.onNav = onNav;
    /** How close to a note's right edge counts as "resize" rather than "move". */
    this.EDGE = 7;
    /*
     * THE VELOCITY READOUT, one element, shown only while a velocity drag is live.
     *
     * Opacity is not a readable quantity — you can see that a note got louder and not that
     * it is now 96 — so a gesture whose whole output is an opacity needs a number beside it
     * or it cannot be aimed. Hidden the rest of the time rather than blank: an empty box
     * floating over the roll is a control that looks broken.
     */
    this.velReadout = div('pr-vel-readout', this.host);
    this.velReadout.appendChild(document.createTextNode(''));
    this.velReadout.style.display = 'none';
    this._velShown = 0;

    this.keysEl = div('pr-keys', host);
    this.band = div('pr-band', host);
    this.ruler = div('pr-ruler', this.band);
    // The three strips that SCROLL, and the things that do not.
    //
    // `rticks`, `gridEl` and `notesEl` hold everything positioned in time, at
    // its absolute content x, and the whole strip slides under the viewport by
    // one transform (GUIDELINES 3.3, and the coordinate note in pianomodel.js).
    // `rticks` exists only because the ruler itself carries the header's
    // background and bottom rule, which must stay still while its labels move —
    // the grid and the notes were already their own containers and needed no
    // new element.
    //
    // `rowsEl` stays outside: pitch rows span the full width and a row that slid
    // sideways would leave a gap at one edge. So does the key ladder, in
    // `keysEl` — a keyboard that scrolls with the music is not a keyboard. So do
    // the playhead and the marquee, which are drawn in viewport pixels.
    this.rticks = div('pr-rticks', this.ruler);
    this.rowsEl = div('pr-rows', this.band);
    this.gridEl = div('pr-grid', this.band);
    this.notesEl = div('pr-notes', this.band);
    this.playhead = div('pr-playhead', this.band);
    this.marquee = div('pr-marquee', this.band);
    // Both are created with no inline display, i.e. shown; the render below is
    // the only thing that writes one, so tracking it here is enough and it never
    // has to be read back off the CSSOM.
    this._phShown = 1; this._mqShown = 1;

    this.keyPool = [];
    this.rowPool = [];
    this.gridPool = [];
    this.rulerPool = [];
    this.notePool = [];
    this.vm = null;

    this.band.addEventListener('pointerdown', (e) => this._down(e));
    this.band.addEventListener('pointermove', (e) => this._move(e));
    this.band.addEventListener('pointerup', (e) => this._up(e));
    this.band.addEventListener('pointercancel', () => this._cancel());
    /*
     * ESCAPE ABANDONS A DRAG MID-GESTURE, which the roll could not do — Escape here cleared
     * the SELECTION and left a live drag running, so a note you had started moving (or, now,
     * started making louder) had to be finished and undone rather than called off.
     *
     * On the WINDOW rather than the band, for the reason the arrangement gives: a captured
     * pointer can leave the element entirely, and by then the band is not where keys go. It
     * is also the only way out of a drag whose pointerup was eaten.
     */
    this._onEsc = (e) => { if (e.key === 'Escape' && (this._note || this._drag)) this._cancel(); };
    window.addEventListener('keydown', this._onEsc);
    this._drag = null;
  }

  _local(e) {
    const r = this.rowsEl.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  _down(e) {
    const vm = this.vm;
    if (!vm) return;
    const noteEl = e.target.closest('.pr-note');
    if (noteEl) {
      const id = Number(noteEl.dataset.id);
      this.onSelect && this.onSelect(id);
      const r = noteEl.getBoundingClientRect();
      const p = this._local(e);
      // Grabbing the right edge resizes, anywhere else moves. A note narrower
      // than twice the edge zone is all edge, so it can only be resized —
      // otherwise a short note would be impossible to lengthen.
      /*
       * VELOCITY IS A MODE, not a modifier — the same call the arrangement's automation
       * editing makes, and for the same stated reason: a modifier is invisible, so there
       * is no way to look at the screen and know whether the next drag will move a note
       * or change how hard it is played, and that is a bad property for a gesture that
       * changes the music. While the mode is on, a drag on a note is vertical-only and
       * sets velocity; the note does not move in pitch or in time.
       */
      const kind = this.velocityEdit ? 'velocity'
        : (r.right - e.clientX) <= this.EDGE ? 'resize' : 'move';
      this._note = { id, kind, x0: p.x, y0: p.y, x1: p.x, y1: p.y };
      this.band.setPointerCapture(e.pointerId);
      return;
    }
    const p = this._local(e);
    if (p.x < 0 || p.y < 0) return;
    // Shift starts a marquee; a plain click writes a note. Writing on every drag
    // would make selecting impossible, and selecting on every click would make
    // the fastest way to enter notes a modifier chord.
    if (e.shiftKey) {
      this._drag = { x0: p.x, y0: p.y, x1: p.x, y1: p.y };
      this.band.setPointerCapture(e.pointerId);
      this.onMarquee && this.onMarquee(this._drag);
      return;
    }
    const high = vm.view.lowPitch + vm.keyCount;
    const pitch = high - 1 - Math.floor(p.y / vm.view.keyHeight);
    const tick = vm.view.startTick + p.x * vm.view.ticksPerPixel;
    this.onNote && this.onNote(pitch, tick);
  }

  _move(e) {
    const p = this._local(e);
    if (this._note) {
      this._note.x1 = p.x; this._note.y1 = p.y;
      this.onDrag && this.onDrag(this._note);
      return;
    }
    if (!this._drag) return;
    this._drag.x1 = p.x; this._drag.y1 = p.y;
    this.onMarquee && this.onMarquee(this._drag);
  }

  _up(e) {
    if (this._note) {
      const n = this._note;
      this._note = null;
      // A click is a drag of zero distance; committing one would rewrite a note
      // to exactly where it already is and burn a clip version for nothing.
      // A velocity drag is vertical, so the horizontal half of this would veto it — a
      // careful straight-down drag moves x by zero and y by plenty.
      const moved = n.kind === 'velocity'
        ? Math.abs(n.y1 - n.y0) > 2
        : (Math.abs(n.x1 - n.x0) > 2 || Math.abs(n.y1 - n.y0) > 2);
      this.onDragEnd && this.onDragEnd(moved ? n : null);
      return;
    }
    if (!this._drag) return;
    const d = this._drag;
    this._drag = null;
    this.onMarqueeEnd && this.onMarqueeEnd(d);
  }

  _cancel() {
    this._drag = null; this._note = null;
    this.onMarqueeEnd && this.onMarqueeEnd(null);
    this.onDragEnd && this.onDragEnd(null);
  }

  _pool(arr, cls, parent, n, init) {
    while (arr.length < n) {
      const el = div(cls, parent);
      if (init) init(el);
      arr.push(el);
    }
    return arr;
  }

  render(vm) {
    this.vm = vm;
    /*
     * The mode arrives on the view model like every other piece of state, rather than being
     * set on the renderer from outside: it changes what a drag MEANS, and a flag the model
     * does not carry is one `probe()` cannot report and a test cannot read.
     */
    this.velocityEdit = !!vm.velocityEdit;
    this.host.classList.toggle('vel-edit', this.velocityEdit);
    // The proposed value, while a drag is proposing one. Guarded on the number, so a frame
    // where it has not moved writes nothing.
    const showVel = vm.dragVel >= 0 ? 1 : 0;
    if (this._velShown !== showVel) {
      this._velShown = showVel;
      this.velReadout.style.display = showVel ? '' : 'none';
    }
    if (showVel && this._velV !== vm.dragVel) {
      this._velV = vm.dragVel;
      this.velReadout.firstChild.nodeValue = 'vel ' + vm.dragVel;
    }
    const kh = vm.view.keyHeight;

    // Scroll by moving the strips, not their contents (GUIDELINES 3.3).
    //
    // Everything bound below sits at its ABSOLUTE content x — `tick /
    // ticksPerPixel`, with no `startTick` in it — so a horizontal pan changes
    // no element's own geometry at all and every per-element guard below stays
    // unfired. It used to change all of them: a pan rebuilt a transform string
    // for every visible note, gridline and ruler label — 3,726 bytes a frame in
    // this function, against 22 for the same redraw standing still — to move a
    // picture that had not otherwise changed. It is 209 now.
    //
    // Guarded on the NUMBER, so a redraw at the same offset writes nothing, and
    // one string serves all three strips. The guard is on the model's own
    // `scrollX` and the sign is applied inside it, not on a negated copy: at
    // the left edge `scrollX` is 0, `-0` is not a small integer, and negating it
    // before the compare boxed a number every single draw — 23 bytes a frame to
    // stand still, which is the whole of what this surface costs at rest.
    //
    // A zoom is a full rebind and must be: `ticksPerPixel` is the denominator of
    // every content x, so a zoom moves both the strip and everything on it. Both
    // sides come from the same `tpp` in the same pass (see `buf.view.scrollX`),
    // and each element's own guard names its content x — which is computed from
    // the zoom — so there is no key here that could stand still while the
    // content behind it moved (GUIDELINES 2.1).
    const sx = vm.view.scrollX;
    if (this._sx !== sx) {
      this._sx = sx;
      const xf = `translateX(${-sx}px)`;
      this.rticks.style.transform = xf;
      this.gridEl.style.transform = xf;
      this.notesEl.style.transform = xf;
    }

    const keys = this._pool(this.keyPool, 'pr-key', this.keysEl, vm.keyCount, initKey);
    const rows = this._pool(this.rowPool, 'pr-row', this.rowsEl, vm.keyCount, initRow);
    for (let i = 0; i < keys.length; i++) {
      const on = i < vm.keyCount;
      const kEl = keys[i], rEl = rows[i];
      // Shown-ness as a number on the element, not read back from the CSSOM.
      // `el.style.display` mints a fresh DOMString on every read, so the old
      // compare cost one string per pooled element per frame — hundreds a frame
      // across five pools, all of them thrown away having proved nothing had
      // changed. The element is created visible, so 1 is true at construction
      // and this cache is the only thing that ever writes the inline value.
      const shown = on ? 1 : 0;
      if (kEl._shown !== shown) { kEl._shown = shown; kEl.style.display = on ? '' : 'none'; }
      if (rEl._shown !== shown) { rEl._shown = shown; rEl.style.display = on ? '' : 'none'; }
      if (!on) continue;
      const key = vm.keys[i];
      if (kEl._y !== key.y) {
        kEl._y = key.y;
        kEl.style.transform = `translateY(${key.y}px)`;
        kEl.style.height = `${key.h}px`;
      }
      if (rEl._y !== key.y) {
        rEl._y = key.y;
        rEl.style.transform = `translateY(${key.y}px)`;
        rEl.style.height = `${key.h}px`;
      }
      if (kEl._black !== key.black) {
        kEl._black = key.black;
        kEl.classList.toggle('black', key.black);
        rEl.classList.toggle('black', key.black);
      }
      if (kEl._label !== key.label) { kEl._label = key.label; kEl.firstChild.nodeValue = key.label; }
    }

    const ruler = this._pool(this.rulerPool, 'pr-tick', this.rticks, vm.rulerCount, initTick);
    for (let i = 0; i < ruler.length; i++) {
      const el = ruler[i];
      const on = i < vm.rulerCount;
      const shown = on ? 1 : 0;
      if (el._shown !== shown) { el._shown = shown; el.style.display = on ? '' : 'none'; }
      if (!on) continue;
      if (el._x !== vm.ruler[i]) { el._x = vm.ruler[i]; el.style.transform = `translateX(${vm.ruler[i]}px)`; }
      if (el._bar !== vm.rulerBar[i]) { el._bar = vm.rulerBar[i]; el.firstChild.nodeValue = barLabel(vm.rulerBar[i]); }
    }

    // The gridlines, recycled as a RING rather than indexed by position
    // (GUIDELINES 3.4). Slot is the line's absolute step index mod the pool
    // size, so scrolling by one step moves one line into the pool and one out
    // and leaves the other ninety holding the same line at the same x.
    //
    // Indexed by position, slot i held "the i-th visible line", which is a
    // different line the moment the left edge crosses a step — so a scroll of
    // eight pixels rebound the entire grid. That survived the band transform
    // above untouched and was, measured, the whole of what remained: 27 fresh
    // transform strings per panning frame against the one the band costs.
    //
    // Absolute x is what makes this legal, exactly as absolute `top` does in the
    // tracker: an element is placed by the line it holds, so which slot holds it
    // does not matter. The pool only ever grows, and growing it reindexes the
    // ring — every guard below then misses once and rebinds, which is correct
    // and happens at the high-water mark, not per frame.
    // Same shape as arrange's ruler and grid, deliberately: `gBase` is the
    // ring's phase, the shown lines walk on from it, and the slots the frame did
    // not claim walk on from where those stopped — which visits each of them
    // exactly once whatever the phase is. Shown-ness is still the cached number
    // rather than a `style.display` read, which is this file's own rule.
    const grid = this._pool(this.gridPool, 'pr-gridline', this.gridEl, vm.gridCount, initGridline);
    const gn = grid.length;
    const gShown = Math.min(vm.gridCount, gn);
    const gBase = gn > 0 ? ((vm.gridFirst % gn) + gn) % gn : 0;
    for (let i = 0; i < gShown; i++) {
      const el = grid[(gBase + i) % gn];
      if (el._shown !== 1) { el._shown = 1; el.style.display = ''; }
      if (el._x !== vm.grid[i]) { el._x = vm.grid[i]; el.style.transform = `translateX(${vm.grid[i]}px)`; }
      const bar = vm.gridIsBar[i] === 1;
      if (el._bar !== bar) { el._bar = bar; el.classList.toggle('bar', bar); }
    }
    for (let i = gShown; i < gn; i++) {
      const el = grid[(gBase + i) % gn];
      if (el._shown !== 0) { el._shown = 0; el.style.display = 'none'; }
    }

    const notes = this._pool(this.notePool, 'pr-note', this.notesEl, vm.noteCount, initNote);
    for (let i = 0; i < notes.length; i++) {
      const el = notes[i];
      const on = i < vm.noteCount;
      const shown = on ? 1 : 0;
      if (el._shown !== shown) { el._shown = shown; el.style.display = on ? '' : 'none'; }
      if (!on) continue;
      const n = vm.notes[i];
      if (el._x !== n.x || el._y !== n.y) {
        el._x = n.x; el._y = n.y;
        el.style.transform = `translate(${n.x}px, ${n.y}px)`;
      }
      if (el._w !== n.w) { el._w = n.w; el.style.width = `${n.w}px`; }
      if (el._h !== n.h) { el._h = n.h; el.style.height = `${n.h}px`; }
      // Velocity as opacity: it is the one note property with no other home here,
      // and a piano roll where every note looks equally loud hides the dynamics.
      // Keyed on the velocity, which is the whole input to the opacity, and then
      // on the string, because two velocities can round to the same two decimals
      // and writing the same opacity back dirties the node for nothing.
      if (el._vel !== n.velocity) {
        el._vel = n.velocity;
        const op = VELOCITY_OPACITY[n.velocity] !== undefined
          ? VELOCITY_OPACITY[n.velocity]
          : VELOCITY_OPACITY[n.velocity < 0 ? 0 : 127];
        if (el._op !== op) { el._op = op; el.style.opacity = op; }
      }
      if (el._muted !== n.muted) { el._muted = n.muted; el.classList.toggle('muted', n.muted); }
      if (el._add !== n.isAdd) { el._add = n.isAdd; el.classList.toggle('add', n.isAdd); }
      if (el._sel !== n.selected) { el._sel = n.selected; el.classList.toggle('sel', n.selected); }
      if (el._id !== n.id) { el._id = n.id; el.dataset.id = String(n.id); }
    }

    // A ghost of the note being dragged, so you can see where it will land
    // before you let go. Drawn as a class on the note itself rather than a
    // second element: the note IS the preview.
    for (let i = 0; i < vm.noteCount; i++) {
      const el = this.notePool[i];
      if (!el) continue;
      const dragging = vm.dragId !== undefined && vm.notes[i].id === vm.dragId;
      if (el._dragging !== dragging) { el._dragging = dragging; el.classList.toggle('dragging', dragging); }
    }

    // The four corners as four numbers, not one joined string. A marquee only
    // exists while a drag is in flight, and a drag draws every frame, so the key
    // that decided whether anything had moved was itself a string built every
    // frame of the drag. Four numbers name the same four inputs and name them
    // exactly; `_mqOn` covers the null case the joined key used '' for.
    const mq = vm.marquee;
    const mqOn = mq ? 1 : 0;
    if (this._mqOn !== mqOn || (mq && (this._mqX0 !== mq.x0 || this._mqY0 !== mq.y0
                                       || this._mqX1 !== mq.x1 || this._mqY1 !== mq.y1))) {
      this._mqOn = mqOn;
      if (mq) { this._mqX0 = mq.x0; this._mqY0 = mq.y0; this._mqX1 = mq.x1; this._mqY1 = mq.y1; }
      if (!mq) { this._mqShown = 0; this.marquee.style.display = 'none'; }
      else {
        if (this._mqShown !== 1) { this._mqShown = 1; this.marquee.style.display = ''; }
        const x = Math.min(mq.x0, mq.x1), y = Math.min(mq.y0, mq.y1);
        this.marquee.style.transform = `translate(${x}px, ${y}px)`;
        this.marquee.style.width = Math.abs(mq.x1 - mq.x0) + 'px';
        this.marquee.style.height = Math.abs(mq.y1 - mq.y0) + 'px';
      }
    }

    // The playhead moves every frame of playback, so the display read inside
    // this guard fired every frame of playback too — one CSSOM string per frame
    // to re-establish that the playhead was still visible. The transform below
    // is the residual: it is a value that genuinely changed.
    const px = vm.playheadX;
    if (this._px !== px) {
      this._px = px;
      if (px < 0) { if (this._phShown !== 0) { this._phShown = 0; this.playhead.style.display = 'none'; } }
      else {
        if (this._phShown !== 1) { this._phShown = 1; this.playhead.style.display = ''; }
        this.playhead.style.transform = `translateX(${px}px)`;
      }
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    let lo = 128, hi = -1;
    for (let i = 0; i < vm.noteCount; i++) {
      const p = vm.notes[i].pitch;
      if (p < lo) lo = p; if (p > hi) hi = p;
    }
    return {
      notes: vm.noteCount, keys: vm.keyCount, zoom: vm.zoom.label, track: vm.trackName,
      lowPitch: vm.view.lowPitch, startTick: vm.view.startTick,
      ticksPerPixel: vm.view.ticksPerPixel, width: vm.view.width,
      pitchRange: hi < 0 ? null : [lo, hi],
      gridLines: vm.gridCount, playheadX: Math.round(vm.playheadX),
      selected: vm.selectedCount,
      /*
       * The mode, the value a live drag proposes, and the VELOCITIES AS DRAWN.
       *
       * The velocities matter more than they look: this roll draws velocity as opacity and
       * nothing else, so "the drag changed the note" and "the drag changed the picture" are
       * different claims and only the second one is what a person sees. Read from the view
       * model rather than from the engine, which is the whole point.
       */
      velocityEdit: !!vm.velocityEdit,
      dragVel: vm.dragVel,
      velocities: (() => { const out = []; for (let i = 0; i < vm.noteCount; i++)
        out.push({ id: vm.notes[i].id, vel: vm.notes[i].velocity }); return out; })(),
      readout: this.velReadout ? this.velReadout.textContent : null,
      domNodes: this.notePool.length + this.keyPool.length * 2 + this.gridPool.length,
    };
  }
}
