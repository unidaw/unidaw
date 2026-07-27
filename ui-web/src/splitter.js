// A drag handle that resizes one region of the shell.
//
// The shell lays its regions out from three numbers on `:root` —
// `--shell-rail-w`, `--shell-dock-w`, `--shell-chain-h` — so making a pane
// resizable is writing one of them. This file owns the pointer half of that: a
// thin handle on one edge of a host element, a clamp, and a callback. It does
// not know what a rail is, and it does not touch localStorage; the integrating
// agent names the property and folds `splitterSizes()` into the session record.
//
// Same rules as every other file here (GUIDELINES 3): NOTHING IS ALLOCATED ON
// THE MOVE PATH. A drag reads its geometry once, at pointerdown, and every
// event after that is arithmetic on cached numbers — no DOMRect per move (which
// is what `piano.js`'s `_local` would cost if it were copied here), no options
// object, no string. A move that does not change the rounded pixel size returns
// before it does anything at all, so a sub-pixel wobble costs one comparison.
//
// Pointer capture, as in `piano.js`: the pointer leaves a 9px handle within a
// frame of any real drag, and without capture the drag ends there.

/**
 * Which edge of the host the handle sits on, and which way the region grows
 * when the pointer moves along the positive axis.
 *
 * The signs are the whole reason `edge` exists rather than a bare axis: the
 * browser rail is dragged by its RIGHT edge and grows with +x, the right dock
 * is dragged by its LEFT edge and grows with −x, and the device-chain strip is
 * dragged by its TOP edge and grows with −y. An axis alone gets two of those
 * three backwards.
 */
const EDGES = {
  right: { axis: 'x', grow: 1 },
  left: { axis: 'x', grow: -1 },
  bottom: { axis: 'y', grow: 1 },
  top: { axis: 'y', grow: -1 },
};

/**
 * Every splitter that was given a key, so the app can save and restore sizes
 * without holding a reference to each one.
 */
const byKey = new Map();

/**
 * One transparent, viewport-filling element, shown only while a drag is live.
 *
 * It exists to carry the CURSOR. Pointer capture routes the events but not the
 * cursor, so once the pointer is off the 9px handle — which is most of any real
 * drag — the arrow reverts to whatever the region underneath draws, and a
 * resize that turns back into a text caret halfway through reads as a drag that
 * was dropped.
 *
 * The obvious alternative was a class on `:root` with a `* { cursor: ...
 * !important }` rule behind it. That works and it was the first version;
 * measured on the real shell it cost 5.73 ms to add and 5.30 ms to remove,
 * because a `*` rule invalidates the computed style of all 4,804 nodes. Twice
 * per drag is a third of a frame at each end, at the exact moment the user is
 * expecting the pane to start moving. This costs one `display` write.
 *
 * Built with the first splitter, never during a drag: a gesture must not
 * allocate DOM.
 */
let shield = null;
function makeShield() {
  if (shield) return;
  shield = document.createElement('div');
  shield.className = 'sp-shield';
  shield._y = false;
  document.body.appendChild(shield);
}
function showShield(axis) {
  const y = axis === 'y';
  if (shield._y !== y) { shield._y = y; shield.classList.toggle('y', y); }
  if (shield.style.display !== 'block') shield.style.display = 'block';
}
function hideShield() {
  if (shield && shield.style.display !== 'none') shield.style.display = 'none';
}

export class Splitter {
  /**
   * @param {HTMLElement} host  the region being resized; the handle is appended
   *   to it and positioned against it.
   * @param {object} opts
   * @param {'right'|'left'|'top'|'bottom'} opts.edge  which edge is grabbable.
   * @param {'x'|'y'} [opts.axis]  only consulted when `edge` is absent, and then
   *   it means the shell's shapes: a left rail grabbed on its right, a bottom
   *   strip grabbed on its top.
   * @param {string} [opts.key]  name under which `splitterSizes()` reports this
   *   one. Omit and the splitter works but is not persisted.
   * @param {number|(() => number)} opts.min  smallest size in px.
   * @param {number|(() => number)} opts.max  largest size in px. A function is
   *   allowed so a limit can follow the window — `() => innerWidth * 0.5`.
   * @param {string} [opts.prop]  CSS custom property to write, e.g.
   *   `--shell-rail-w`. This is what makes a drag a layout change.
   * @param {HTMLElement} [opts.target]  element the property is written on;
   *   defaults to the document root, which is where shell.css declares them.
   * @param {(px:number, s:Splitter) => void} [opts.onSize]  every size change,
   *   drag or key or restore. Where the app schedules its redraw.
   * @param {(px:number, s:Splitter) => void} [opts.onEnd]  once per completed
   *   drag or key press. Where the app saves.
   * @param {() => number} [opts.measure]  override for reading the current size;
   *   by default it is read from `prop`, and failing that from the host's box.
   * @param {number} [opts.step]  pixels per arrow key. Shift is always 1.
   * @param {number} [opts.home]  size a double-click (or Home) returns to.
   * @param {string} [opts.label]  accessible name.
   */
  constructor(host, opts = {}) {
    const {
      key = '', min = 0, max = Infinity, prop = '', target = document.documentElement,
      onSize = null, onEnd = null, measure = null, step = 8, home = null, label = '',
    } = opts;
    const edge = opts.edge || (opts.axis === 'y' ? 'top' : opts.axis === 'x' ? 'right' : '');
    const spec = EDGES[edge];
    // Thrown, not defaulted. An edge this file does not recognise is a handle
    // that drags the region the wrong way — it renders, it responds, and it is
    // wrong: the silent-but-plausible failure GUIDELINES 2.1 is a table of.
    if (!spec) throw new Error('Splitter: edge must be right|left|top|bottom, got ' + edge);

    this.host = host;
    this.key = key;
    this.edge = edge;
    this.axis = spec.axis;
    this.prop = prop;
    this.target = target;
    this.onSize = onSize;
    this.onEnd = onEnd;
    this.step = step;
    this.home = home;
    this._measure = measure;
    this._minOpt = min;
    this._maxOpt = max;
    this._grow = spec.grow;

    /** Last size applied, in whole px. Null until something reads or sets one. */
    this._size = null;
    /** Limits resolved at the start of a drag, so a move never calls a function. */
    this._min = 0;
    this._max = 0;
    /** Size and pointer position at pointerdown. The whole drag is these two. */
    this._from = 0;
    this._origin = 0;
    /** Active pointer id, −1 when idle. */
    this._id = -1;

    // The handle is positioned against the host, so the host has to be a
    // containing block. #browser and #chain already are; #rdock is not, and an
    // absolutely positioned child of a static parent lands against the page
    // instead — a handle in the top-left corner of the window. Set at runtime
    // rather than in splitter.css because this file has to work on any host and
    // shell.css belongs to somebody else.
    if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
    makeShield();

    const el = document.createElement('div');
    el.className = 'sp sp-' + spec.axis + ' sp-' + edge;
    // A separator with a value is what a resize handle IS, in the one vocabulary
    // both a screen reader and an agent already know how to read.
    el.setAttribute('role', 'separator');
    el.setAttribute('aria-orientation', spec.axis === 'x' ? 'vertical' : 'horizontal');
    if (label) el.setAttribute('aria-label', label);
    if (key) el.dataset.splitter = key;
    // Tab reaches it; a click does NOT focus it (see `_down`). A splitter that
    // took focus on click would swallow the arrow keys the tracker wants.
    el.tabIndex = 0;
    host.appendChild(el);
    this.el = el;

    el.addEventListener('pointerdown', (e) => this._down(e));
    el.addEventListener('pointermove', (e) => this._move(e));
    el.addEventListener('pointerup', (e) => this._up(e));
    el.addEventListener('pointercancel', () => this._cancel());
    // The safety net, and it is not theoretical: a drag that loses its capture
    // without a pointerup — the window backgrounded mid-gesture, the element
    // taken out of the tree — leaves `_id` set, and `_down` refuses every drag
    // after that. The pane silently stops being resizable until a reload, which
    // is the failure shape that looks like nothing happened. This ends the drag
    // whenever the capture goes, and does nothing when `_up` got there first.
    el.addEventListener('lostpointercapture', () => this._cancel());
    el.addEventListener('keydown', (e) => this._key(e));
    if (home !== null) el.addEventListener('dblclick', () => this._commit(this.setSize(home)));

    if (key) byKey.set(key, this);
  }

  /**
   * The size the region has right now, in px. MEASURED, not remembered.
   *
   * Read from the custom property first, because that is the number the shell
   * lays out from and it round-trips exactly. The host's box is the fallback and
   * it is a worse one: `#browser` is content-box with a 1px border, so its
   * bounding rect is a pixel wider than the property that sized it, and starting
   * each drag from the rect would walk the rail one pixel right per drag.
   */
  read() {
    if (this._measure) return this._measure();
    if (this.prop) {
      const v = parseFloat(getComputedStyle(this.target).getPropertyValue(this.prop));
      if (Number.isFinite(v)) return v;
    }
    const r = this.host.getBoundingClientRect();
    return this.axis === 'x' ? r.width : r.height;
  }

  /** Current size in px, reading it once if nothing has yet. */
  size() {
    if (this._size === null) this._size = Math.round(this.read());
    return this._size;
  }

  /**
   * Resolve `min`/`max` into plain numbers. Called at the start of a drag and
   * on every keyboard or restored change — never from `_move`, which is why the
   * limits may be functions at all.
   */
  limits() {
    const lo = typeof this._minOpt === 'function' ? this._minOpt() : this._minOpt;
    const hi = typeof this._maxOpt === 'function' ? this._maxOpt() : this._maxOpt;
    this._min = lo;
    // A max below the min means a window too small for the layout's own rules.
    // The min wins: a region collapsed to nothing is a region you cannot grab
    // again, and an unreachable handle is worse than a cramped one.
    this._max = hi < lo ? lo : hi;
  }

  /**
   * Apply a size: clamp, write the property, report. The one path a drag, an
   * arrow key and a restored session all go through, so the three cannot drift
   * — which is the same reason the dock's commands route through the functions
   * the keys do.
   * @returns {number} the size actually applied, after clamping.
   */
  setSize(px) {
    this.limits();
    let n = px;
    if (n < this._min) n = this._min; else if (n > this._max) n = this._max;
    this._apply(Math.round(n));
    this._aria();
    return this._size;
  }

  /** Re-clamp against limits that may have moved — call on window resize. */
  refresh() { this.setSize(this.size()); }

  /**
   * The only writer. Guarded on the cached NUMBER, so an unchanged size costs
   * one comparison: no style write, no callback, and no `n + 'px'` — the one
   * string this file cannot avoid, since a custom property is text.
   */
  _apply(n) {
    if (n === this._size) return false;
    this._size = n;
    if (this.prop) this.target.style.setProperty(this.prop, n + 'px');
    if (this.onSize) this.onSize(n, this);
    return true;
  }

  _down(e) {
    // One pointer at a time. A second finger arriving mid-drag would overwrite
    // the origin and the region would jump to meet it.
    if (this._id >= 0) return;
    this._id = e.pointerId;
    this.limits();
    // Everything the drag needs, read once. `read()` forces a style resolve and
    // `setPointerCapture` is a DOM call; both belong here and neither belongs in
    // `_move`.
    this._from = this.read();
    this._origin = this.axis === 'x' ? e.clientX : e.clientY;
    this.el.setPointerCapture(e.pointerId);
    this.el.classList.add('on');
    showShield(this.axis);
    // Suppresses the compatibility mouse events, and with them the focus a click
    // would otherwise take. Deliberate: a splitter holding focus after a drag
    // would eat the arrow keys the surface behind it is listening for.
    e.preventDefault();
    // Nothing is reported yet. A click that goes nowhere must not count as a
    // resize, or every stray click writes a session.
  }

  _move(e) {
    if (this._id < 0) return;
    // The hot path. No rect, no object, no string, no function call out of this
    // file unless the size genuinely changed.
    const at = this.axis === 'x' ? e.clientX : e.clientY;
    let n = this._from + this._grow * (at - this._origin);
    if (n < this._min) n = this._min; else if (n > this._max) n = this._max;
    this._apply(Math.round(n));
  }

  _up(e) {
    if (this._id < 0) return;
    // The pointerup position is applied before the drag ends, because pointerup
    // is the only event of a drag that is GUARANTEED to arrive. Chrome delivers
    // pointermove on an animation-frame cadence and coalesces the rest, so a
    // drag that finishes between two frames silently loses its last few pixels —
    // it showed up here as a 120px drag landing at 105 on one run in three, and
    // it is the same on a real trackpad, just harder to notice.
    this._move(e);
    this._release(e.pointerId);
    this._commit(this._size);
  }

  /**
   * A cancelled drag KEEPS its size, unlike the arrange ruler's loop drag which
   * reverts. The difference is that a half-drawn loop means nothing while a
   * half-dragged pane is exactly what the user has been looking at for the last
   * second; snapping it back would be the surprise.
   */
  _cancel() {
    if (this._id < 0) return;
    this._release(this._id);
    this._commit(this._size);
  }

  _release(id) {
    this._id = -1;
    if (id >= 0 && this.el.hasPointerCapture(id)) this.el.releasePointerCapture(id);
    this.el.classList.remove('on');
    hideShield();
    this._aria();
  }

  /** End of a gesture: publish the settled size once. This is the save point. */
  _commit(px) {
    if (this.onEnd) this.onEnd(px, this);
  }

  _key(e) {
    // e.code, not e.key: Shift is a modifier and on macOS a modifier can rewrite
    // e.key entirely (GUIDELINES 2.16). Arrows survive it — the rule is still
    // cheaper to keep than to re-derive the next time a binding is added.
    const code = e.code;
    let dir = 0;
    if (this.axis === 'x') {
      if (code === 'ArrowLeft') dir = -1;
      else if (code === 'ArrowRight') dir = 1;
    } else {
      if (code === 'ArrowUp') dir = -1;
      else if (code === 'ArrowDown') dir = 1;
    }

    if (dir === 0) {
      if (code !== 'Home' || this.home === null) return;
      e.preventDefault(); e.stopPropagation();
      this._commit(this.setSize(this.home));
      return;
    }
    // stopPropagation as well as preventDefault: the app's keydown listener is on
    // the window, and an arrow that resizes the rail must not also step the
    // tracker cursor. The handle only has focus because somebody tabbed to it.
    e.preventDefault(); e.stopPropagation();
    // Shift is the fine adjustment, the same way it is on the arrange ruler.
    // `_grow` maps the key's direction onto the region's: ArrowRight grows the
    // left rail and SHRINKS the right dock, because that is where the edge went.
    const by = e.shiftKey ? 1 : this.step;
    this._commit(this.setSize(this.size() + dir * this._grow * by));
  }

  /**
   * The accessible value, refreshed when a gesture SETTLES rather than during
   * one. `String(n)` allocates, and three attribute writes per pointermove to
   * keep a value nobody reads mid-drag is the trade backwards; what a screen
   * reader announces is where the handle came to rest.
   */
  _aria() {
    if (this._size === null) return;
    const el = this.el;
    if (el._an !== this._size) { el._an = this._size; el.setAttribute('aria-valuenow', String(this._size)); }
    if (el._ax !== this._min) { el._ax = this._min; el.setAttribute('aria-valuemin', String(this._min)); }
    const hi = Number.isFinite(this._max) ? this._max : this._size;
    if (el._ay !== hi) { el._ay = hi; el.setAttribute('aria-valuemax', String(hi)); }
  }

  /** Structure for a test or an agent to assert on. Allocates; not a draw path. */
  probe() {
    this.limits();
    return {
      key: this.key, edge: this.edge, axis: this.axis, prop: this.prop,
      size: this.size(), min: this._min, max: this._max,
      dragging: this._id >= 0,
      atMin: this.size() <= this._min,
      atMax: Number.isFinite(this._max) && this.size() >= this._max,
    };
  }

  destroy() {
    if (this.key && byKey.get(this.key) === this) byKey.delete(this.key);
    this.el.remove();
  }
}

/**
 * Every keyed splitter's size, as plain data for the session record.
 *
 * Allocates an object, deliberately: it is called when a drag ends and when the
 * app saves, never from a draw or a move. The shape is `{key: px}` so the
 * integrating agent can spread it into the JSON it already writes.
 */
export function splitterSizes() {
  const out = {};
  for (const [k, s] of byKey) out[k] = s.size();
  return out;
}

/**
 * Apply saved sizes.
 *
 * Clamped on the way in, exactly like the zoom and octave `restoreSession()`
 * already clamps: a 900px rail saved on a wide monitor, restored into a 1000px
 * window, would leave no centre at all — and the value came from disk, so it can
 * be anything. Unknown keys and non-numbers are ignored rather than trusted.
 *
 * @returns {number} how many splitters were given a size.
 */
export function restoreSplitterSizes(saved) {
  if (!saved || typeof saved !== 'object') return 0;
  let n = 0;
  for (const [k, s] of byKey) {
    const v = saved[k];
    if (!Number.isFinite(v)) continue;
    s.setSize(v);
    n++;
  }
  return n;
}

/** Re-clamp every splitter — for the window `resize` listener. */
export function refreshSplitters() {
  for (const s of byKey.values()) s.refresh();
}

/** The live splitter for a key, or undefined. */
export function splitter(key) { return byKey.get(key); }
