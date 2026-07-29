// Arrange renderer: lanes of clip blocks on a horizontal time axis.
//
// Same rules as the tracker renderer, for the same reasons (GUIDELINES 3):
// pooled elements that are hidden rather than removed, every style write guarded
// by a cached number, and no string building unless the string actually changed.
// A DAW's arrange page is where a user drags things around, so it is the one
// surface where a dropped frame is felt directly.

import { anchoredStart, clampZoom, ticksPerPixelAt, wheelPixels, dragPlacement,
         clipZoneAt, CLIP_HANDLE_PX,
         WHEEL_NOTCH_PX, WHEEL_PINCH_PX } from './arrangemodel.js';

const GRID_POOL = 256;

/**
 * Whether a clip can be dragged to another LANE.
 *
 * Off, because the engine refuses a cross-track MovePlacement — and refuses the
 * WHOLE command when it sees one, not just the lane part. So a diagonal drag
 * would move the clip neither across nor along, which is the worst answer
 * available: the gesture looks like it worked right up until nothing happens.
 *
 * With this off the vertical component is simply ignored, so a drag that wanders
 * still repositions in time, and the page says why the lane did not change.
 *
 * Flip it when the engine's own comment stops saying "cross-track is a v2"
 * (apps/event_payloads.h, UiCommandType::MovePlacement).
 */
const CROSS_TRACK_DRAG = false;

// --- the waveform layer ----------------------------------------------------
//
// One CANVAS for every clip, not one canvas per clip and not DOM.
//
// DOM is out on volume: a waveform is one filled column per device pixel, so a
// screen-wide clip is a few thousand nodes whose only per-frame write is a
// height — the exact shape GUIDELINES 3 exists to keep out of this app.
//
// A canvas PER CLIP is out on arithmetic. At the finest zoom a bar is 512 px, so
// a four-minute region is ~250,000 CSS px and half a million device pixels wide,
// and every browser refuses a canvas dimension past ~16,384. A per-clip canvas
// would therefore have to be windowed anyway — at which point it is this, once
// per clip instead of once per screen, with N contexts and N clears.
//
// So: ONE canvas, the width of the viewport plus overscan, living inside
// `.ar-clips > .ar-scroll` at the painted band's absolute origin. Being inside
// the scrolled wrapper is the whole point — a pan translates the wrapper and the
// canvas goes with it for free, exactly as the clips, gridlines and ruler ticks
// do (GUIDELINES 3.3), and costs NO repaint until the viewport reaches the edge
// of what has been painted. It is the same band-with-overscan trick `tracker.js`
// uses for rows, turned on its side.
//
// It is sized to the full lane strip vertically (laneCount x laneHeight) rather
// than to the visible band, so a lane scroll is also free: `.ar-clips` is already
// translated by `-laneScroll` and the canvas rides that too. The strip is bounded
// — the page caps arrange at 16 lanes — so this is a bounded canvas, which the
// horizontal axis can never be.

/** CSS pixels painted beyond each edge of the viewport. Bigger costs memory and
 *  repaint time; smaller repaints more often. 384 px is about a third of a
 *  viewport, so a fast trackpad pan repaints every ten-odd frames. */
const WAVE_OVERSCAN_PX = 384;

/**
 * How far past the viewport the arrange model must window its clips, in pixels.
 *
 * TWICE the overscan, and the factor of two is the whole point. The painted band
 * is re-centred only when the viewport reaches its edge, so at the moment before
 * a repaint the band already extends a full overscan beyond the viewport on one
 * side — and the viewport has itself moved an overscan since the band was placed.
 * A model windowed to the viewport would therefore hand the painter a clip set
 * that changes every time a region crosses the SCREEN edge, which during a pan is
 * constantly: measured, that repainted 37% of frames for a picture that had not
 * changed. Windowed to the band, the set changes only when the band moves.
 */
export const ARRANGE_CLIP_MARGIN_PX = WAVE_OVERSCAN_PX * 2;

/**
 * Columns per requested window.
 *
 * Requests are TILED — anchored to a multiple of `decimation * WAVE_TILE_COLS`
 * rather than to wherever the viewport happens to start. The cache is keyed on
 * (source, decimation, firstFrame), so a window asked for at the pan's current
 * position is a key nobody will ever ask for again: every repaint would miss,
 * re-request, and evict. Anchored to a tile grid, a pan across a file asks for
 * each tile once and hits the cache from then on.
 *
 * 4096 columns is 8192 pairs in stereo, inside the engine's 24,576-pair slot, and
 * at the coarsest arrange zoom one tile covers about two hours of audio.
 */
const WAVE_TILE_COLS = 4096;

/** Both channels. The engine clamps to what the source actually has. */
const WAVE_MASK_BOTH = 3;

/** No browser will allocate a canvas dimension past this. */
const WAVE_MAX_DEVICE_PX = 16384;

/** CSS pixels of clearance between the waveform and the lane's edges. */
const WAVE_INSET_PX = 2;

/** Milliseconds between repaints while a window we want has not arrived. An
 *  answer bumps the revision and repaints immediately, so this only paces the
 *  case where nothing is coming back — a dropped request, or no engine. */
const WAVE_RETRY_MS = 125;

/** Device pixels of on/off in the stripe drawn over a source that would not
 *  decode. Long enough to read as deliberate, short enough to fit a narrow clip. */
const WAVE_FAIL_DASH = 6;

/**
 * Bar numbers as text, made once each and kept.
 *
 * The label write is guarded on the bar number, so playback — where the view
 * holds still between page jumps — never builds one. The gesture this is for is
 * a PAN: every visible tick takes a new bar number on every frame of it, so a
 * two-second trackpad swipe called `String(bar)` some three thousand times to
 * produce a few hundred distinct labels, and produced them again on the way
 * back. The number is the whole of the label, so an entry can never be stale.
 * Past the cap it falls back rather than growing without bound — a song is not
 * eight thousand bars long, and if one is, the tail costs what it used to.
 */
const BAR_LABEL = new Array(8192);
function barLabel(bar) {
  if (bar < 0 || bar >= BAR_LABEL.length) return String(bar);
  return BAR_LABEL[bar] || (BAR_LABEL[bar] = String(bar));
}

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

export class Arrange {
  /**
   * @param {HTMLElement} host
   * @param {object} metrics
   * @param {{onLoop?: Function, onNav?: (zoomIndex:number, startTick:number,
   *          laneScroll:number, kind:string) => void}} opts
   *
   * `onNav` is the wheel gesture, in the same shape as `onLoop`: this file knows
   * pixels and modifier flags, the page knows where the view state lives. It is
   * handed ABSOLUTE values rather than deltas, and all three of them every time,
   * because a zoom that keeps the pointer's bar still is one atomic change to
   * two of them — sending a zoom and a scroll separately draws one frame with the
   * new zoom and the old start, which is exactly the flicker the anchoring is
   * there to avoid. `kind` is 'zoom', 'pan' or 'lanes', for a caller that wants
   * to log or distinguish them; every one of them means the user is driving, so
   * any playhead-following should stop on all three.
   */
  constructor(host, metrics,
              { onLoop, onNav, onClipSelect, onClipOpen, onClipEdit } = {}) {
    this.host = host;
    this.metrics = metrics;
    this.host.className = 'ar';
    // Raw ticks, unsnapped: where a loop is ALLOWED to start is a musical
    // question, and this file knows about pixels. The caller snaps.
    this.onLoop = onLoop;
    this.onNav = onNav;
    this.onClipSelect = onClipSelect;
    this.onClipOpen = onClipOpen;
    // Where a finished drag goes. Absent, clips are still selectable and
    // openable and simply cannot be moved — which is what the arrangement was
    // before the engine had placement ops, and is still the right behaviour for
    // a surface bound without one.
    this.onClipEdit = onClipEdit;

    this.gutter = div('ar-gutter', host);
    // Two boxes, not one: the outer clips at the lane strip's top edge so a head
    // scrolled off slides UNDER the ruler, and the inner is the thing that moves.
    // Translating the clipper would take its own clip rectangle with it.
    this.headsClip = div('ar-heads', this.gutter);
    this.headsEl = div('ar-heads-in', this.headsClip);
    this.band = div('ar-band', host);
    this.ruler = div('ar-ruler', this.band);
    this.loop = div('ar-loop', this.ruler);
    // The three strips that carry the time axis, each in a wrapper that is the
    // thing that scrolls. Everything inside them is positioned at its ABSOLUTE
    // x — `tick / ticksPerPixel`, with no `startTick` in it — so a pan writes
    // ONE transform string instead of a position string per clip, gridline and
    // ruler tick (GUIDELINES 3.3). Measured: 3,969 -> 128 bytes per pan frame.
    //
    // The wrapper goes INSIDE each strip rather than around all three, for the
    // reason `.ar-heads-in` gives one level up: the strips are what clips, and a
    // transform on a clipper takes its clip rectangle along with it. Created
    // after `this.loop` so the bar numbers still paint over the loop bracket.
    this.rulerIn = div('ar-scroll', this.ruler);
    this.lanesEl = div('ar-lanes', this.band);
    this.gridEl = div('ar-grid', this.band);
    this.gridIn = div('ar-scroll', this.gridEl);
    this.clipsEl = div('ar-clips', this.band);
    this.clipsIn = div('ar-scroll', this.clipsEl);
    // FIRST child of the clip wrapper, so every pooled clip block created later
    // paints over it. The blocks are a translucent wash, a border and a name, so
    // the waveform reads as the material INSIDE the region rather than as
    // something drawn next to it.
    this.waveCanvas = document.createElement('canvas');
    this.waveCanvas.className = 'ar-wave';
    this.clipsIn.appendChild(this.waveCanvas);
    this.waveCtx = this.waveCanvas.getContext('2d');
    this.playhead = div('ar-playhead', this.band);

    this.lanePool = [];
    this.headPool = [];
    this.clipPool = [];
    /** Which clip slots this frame claimed. Grown with the pool, never per frame. */
    this._clipSeen = new Uint8Array(0);
    this.gridPool = [];
    this.rulerPool = [];
    this.laneCount = 0;
    this.vm = null;

    // The waveform layer's state. Every one of these is a guard: the canvas is a
    // cache of a picture, and the picture is a function of exactly these.
    this.waveCache = null;              // the page's Map of windows; see bindAudio
    this.waveRequest = null;            // the page's requestWaveform
    this.waveColors = { body: '', fail: '' };
    this.waveThemed = false;
    this._wvDevW = 0; this._wvDevH = 0; this._wvDpr = 0;
    this._wvSpanX = 0; this._wvSpanW = 0; this._wvTx = -1;
    this._wvTpp = -1; this._wvLaneH = -1; this._wvLanes = -1; this._wvRev = -1;
    this._wvIncomplete = false; this._wvNextTry = 0;
    // The visible clips as the last paint saw them, six numbers each. A pan
    // inside the painted band must not repaint, so "did the clips change?" has to
    // be answerable without a string and without a revision the model does not
    // have — and it has to name every input, not just the count (GUIDELINES 2.1).
    this._wvSig = new Float64Array(0); this._wvSigN = -1;
    this.waveRepaints = 0; this.waveDrawn = 0;
    this.waveWanted = 0; this.waveHeld = 0;

    // The ruler is where a loop is set, because that is where it is drawn.
    this.ruler.addEventListener('pointerdown', (e) => this._loopDown(e));
    this.ruler.addEventListener('pointermove', (e) => this._loopMove(e));
    this.ruler.addEventListener('pointerup', (e) => this._loopUp(e));
    this.ruler.addEventListener('pointercancel', () => this._loopCancel());
    this._loopDrag = null;

    // {passive: false} is load-bearing, not decoration. A passive listener's
    // preventDefault() is IGNORED — silently, behind a console warning nobody
    // reads — and wheel listeners are passive by default on window, document and
    // body, which is exactly where a delegated version of this would end up. The
    // page would then page-zoom on ctrl+wheel underneath a surface that believed
    // it had handled the gesture. Stating it here also survives the move.
    this.host.addEventListener('wheel', (e) => this._wheel(e), { passive: false });

    /**
     * Clips answer to the pointer.
     *
     * The band had no pointer handling at all — only the ruler, for the loop, and
     * the wheel. So a clip could be looked at and nothing else: not selected, not
     * opened, not addressed by any gesture. An arrangement you cannot point at is
     * a picture of an arrangement.
     *
     * A clip is identified here by (track, startTick) — what the element already
     * carries. That is positional, and it is what there is: the engine publishes
     * no placement id, so a stable one has been requested. Content-derived beats
     * an index into a list that reorders, which is the failure that has now
     * turned up three times in this codebase.
     */
    this.clipsIn.addEventListener('pointerdown', (e) => {
      const el = e.target.closest('.ar-clip');
      if (!el) return;
      if (this.onClipSelect) this.onClipSelect({ track: el._pTrack, tick: el._pTick });
      this._clipDown(e, el);
    });
    this.clipsIn.addEventListener('pointermove', (e) => this._clipMove(e));
    this.clipsIn.addEventListener('pointerup', (e) => this._clipUp(e));
    this.clipsIn.addEventListener('pointercancel', () => this._clipCancel());
    /**
     * Escape abandons a drag mid-gesture.
     *
     * On the WINDOW, not the band: a drag with the pointer captured can leave
     * the element entirely, and by then the band is not where keys go. This is
     * also the only way out of a drag whose pointerup was eaten — a plugin
     * window taking focus mid-gesture, say.
     */
    this._onEsc = (e) => { if (e.key === 'Escape' && this._clipDrag) this._clipCancel(); };
    window.addEventListener('keydown', this._onEsc);
    this._clipDrag = null;
    // dblclick, not a hand-rolled double pointerdown: the browser already knows
    // the platform's interval and its slop radius, and both differ per OS.
    this.clipsIn.addEventListener('dblclick', (e) => {
      const el = e.target.closest('.ar-clip');
      if (!el || !this.onClipOpen) return;
      this.onClipOpen({ track: el._pTrack, tick: el._pTick });
    });
    this._zoomAccum = 0;
    this._lastNav = '';

    // The controller, on its own host element — the same idiom this file already
    // uses for `el._x` and `head._nm`, one level up. Every other surface is
    // reachable through `window.__uni`, which the page builds from its own
    // closures; this is the first thing here that does NOTHING until a caller
    // wires it, so without a handle a test cannot tell "wired and broken" from
    // "not wired yet" — and an unreachable listener is the failure GUIDELINES
    // 2.15 is about.
    host._arrange = this;
  }

  /** Tick under the pointer, from the ruler's own box. */
  _tickAt(e) {
    if (!this.vm) return 0;
    const r = this.ruler.getBoundingClientRect();
    return this.vm.view.startTick + (e.clientX - r.left) * this.vm.view.ticksPerPixel;
  }

  _loopDown(e) {
    if (!this.vm || !this.onLoop) return;
    const t = this._tickAt(e);
    this._loopDrag = { a: t, b: t };
    this.ruler.setPointerCapture(e.pointerId);
    this.onLoop(t, t, false, { fine: e.shiftKey });
  }

  _loopMove(e) {
    if (!this._loopDrag) return;
    this._loopDrag.b = this._tickAt(e);
    this.onLoop(this._loopDrag.a, this._loopDrag.b, false, { fine: e.shiftKey });
  }

  _loopUp(e) {
    if (!this._loopDrag) return;
    const { a } = this._loopDrag;
    const b = this._tickAt(e);
    this._loopDrag = null;
    this.ruler.releasePointerCapture(e.pointerId);
    this.onLoop(a, b, true, { fine: e.shiftKey });
  }

  /** A cancelled drag reverts: it must not commit a span the pointer left. */
  _loopCancel() {
    if (!this._loopDrag) return;
    this._loopDrag = null;
    this.onLoop(0, 0, true, { cancelled: true });
  }

  /*
   * ── DRAGGING A CLIP ────────────────────────────────────────────────────────
   *
   * Move it, trim either end, or drag it to another lane. Which of those a
   * gesture is depends on where in the block it started, so there is one
   * pointerdown rather than three overlapping hit targets.
   *
   * THE PENDING POSITION IS A GHOST, and the real block does not move until the
   * engine says it did. Moving the block optimistically would fight the layout
   * pass, which rewrites every clip's transform from the model each frame — so
   * the clip would snap back to its published position on the very next
   * published frame and jitter for the length of the drag. It also makes
   * clamping legible: the block stays where the clip IS while the ghost shows
   * where it will land, and if the engine clamps, the difference is on screen
   * rather than a surprise on release.
   */
  _clipDown(e, el) {
    if (!this.vm || !this.onClipEdit) return;
    // Only the primary button. A right-click is a context menu everywhere else
    // and starting a drag under one is how a clip ends up somewhere nobody
    // meant to put it.
    if (e.button !== 0) return;
    const r = el.getBoundingClientRect();
    const mode = clipZoneAt(e.clientX - r.left, r.width);
    this._clipDrag = {
      id: el._pId, mode, pointerId: e.pointerId,
      x0: e.clientX, y0: e.clientY,
      clip: { startTick: el._pTick, endTick: el._pEnd, track: el._pTrack },
      at: null,
    };
    this.clipsIn.setPointerCapture(e.pointerId);
    // Drawn immediately, at the clip's own position: a ghost that only appears
    // once the pointer has travelled a bar looks like the drag did not take.
    this._clipGhost(this._clipDrag.clip);
  }

  /** Where the current pointer position puts the clip. */
  _clipAt(e) {
    const d = this._clipDrag;
    const tpp = this.vm.view.ticksPerPixel;
    const lh = this.vm.laneHeight || 1;
    const lanes = CROSS_TRACK_DRAG && d.mode === 'move'
      ? Math.round((e.clientY - d.y0) / lh) : 0;
    return dragPlacement(d.clip, d.mode, (e.clientX - d.x0) * tpp, lanes,
                         { fine: e.shiftKey, laneCount: this.laneCount,
                           meter: this.vm.meter });
  }

  _clipMove(e) {
    if (!this._clipDrag) return;
    const at = this._clipAt(e);
    this._clipDrag.at = at;
    this._clipGhost(at);
  }

  _clipUp(e) {
    const d = this._clipDrag;
    if (!d) return;
    const at = this._clipAt(e);
    this._clipDrag = null;
    this.clipsIn.releasePointerCapture(e.pointerId);
    this._clipGhost(null);
    // Did the pointer try to change lane? Worth reporting either way: with
    // cross-track off, that half of the gesture did not happen, and a drag that
    // reads as ignored is worse than one that says what it could not do.
    const triedLane = !CROSS_TRACK_DRAG && d.mode === 'move'
      && Math.abs(e.clientY - d.y0) > (this.vm.laneHeight || 44) / 2;
    // A click is not an edit. Sending an unchanged placement would dirty the
    // project and cost an undo step for the act of selecting something — but a
    // refused lane change is still news.
    if (!at.changed) {
      if (triedLane) this.onClipEdit({ laneRefused: true });
      return;
    }
    this.onClipEdit({
      op: d.mode === 'move' ? 'move' : 'resize',
      id: d.id, track: d.clip.track,
      at: at.startTick, len: at.endTick - at.startTick,
      toTrack: at.track === d.clip.track ? undefined : at.track,
      // A right-edge drag left the start alone, and saying so is what keeps it
      // one command instead of a move followed by a resize.
      startUnchanged: d.mode === 'trim-r',
      laneRefused: triedLane,
    });
  }

  /** Abandoned: the ghost goes and nothing is sent. */
  _clipCancel() {
    if (!this._clipDrag) return;
    const id = this._clipDrag.pointerId;
    this._clipDrag = null;
    try { this.clipsIn.releasePointerCapture(id); } catch (err) { /* already gone */ }
    this._clipGhost(null);
  }

  /** The pending position, or null to take it away. Created on first use. */
  _clipGhost(at) {
    if (!at) {
      if (this._ghost) this._ghost.style.display = 'none';
      return;
    }
    if (!this._ghost) {
      this._ghost = div('ar-ghost', this.clipsIn);
      this._ghost.style.display = 'none';
    }
    const tpp = this.vm.view.ticksPerPixel;
    const lh = this.vm.laneHeight || 1;
    const x = at.startTick / tpp;
    const w = Math.max(2, (at.endTick - at.startTick) / tpp);
    if (this._ghost.style.display === 'none') this._ghost.style.display = '';
    this._ghost.style.transform = `translate(${x}px, ${at.track * lh}px)`;
    this._ghost.style.width = `${w}px`;
    this._ghost.style.height = `${lh}px`;
  }

  /** How far the lane strip is scrolled right now, per the model. */
  _laneScroll() { return this.vm ? this.vm.laneScroll || 0 : 0; }

  /**
   * How far it is ALLOWED to be scrolled: the part of the strip that does not
   * fit. Measured from the real box rather than recomputed from tokens
   * (GUIDELINES 3.11) — the ruler's height, the band's height and the window are
   * three separate CSS facts, and a hand-rolled copy of their sum goes stale the
   * first time any of them moves, leaving the last lane unreachable exactly the
   * way the tracker's missing 2px track border did.
   */
  _maxLaneScroll() {
    if (!this.vm) return 0;
    const lh = this.vm.lanes.length ? this.vm.lanes[0].height : 0;
    return Math.max(0, this.vm.laneCount * lh - this.lanesEl.clientHeight);
  }

  /**
   * Wheel deltas to zoom steps.
   *
   * Sign follows the zoom table, where index 0 is the finest: a wheel pushed
   * away (deltaY < 0) and a trackpad pinch OPENED both zoom in, which is one
   * index down. That is also what `+`/`=` does on the keyboard, so the two routes
   * to a zoom agree.
   */
  _zoomSteps(dy) {
    if (dy === 0) return 0;
    if (Math.abs(dy) >= WHEEL_NOTCH_PX) { this._zoomAccum = 0; return dy > 0 ? 1 : -1; }
    // A reversal starts a new gesture. Carrying the previous direction's
    // remainder makes the first step back feel stuck for no reason the user can
    // see — they pinched, nothing moved, and the pinch before it is why.
    if (this._zoomAccum !== 0 && (dy > 0) !== (this._zoomAccum > 0)) this._zoomAccum = 0;
    this._zoomAccum += dy;
    let steps = 0;
    while (Math.abs(this._zoomAccum) >= WHEEL_PINCH_PX) {
      const s = this._zoomAccum > 0 ? 1 : -1;
      steps += s;
      this._zoomAccum -= s * WHEEL_PINCH_PX;
    }
    return steps;
  }

  /**
   * The one wheel gesture, decoded.
   *
   *   Cmd/Ctrl + wheel   zoom, anchored on the tick under the pointer
   *   Shift    + wheel   pan the time axis
   *   wheel              scroll the lanes; a horizontal delta pans time
   *
   * Both Cmd and Ctrl because the same build ships on macOS and Windows, and the
   * match is on the event's modifier FLAGS rather than on a key (GUIDELINES
   * 2.16): there is no synthesised key here to get wrong, and metaKey/ctrlKey are
   * what the platform actually sets. A macOS trackpad PINCH arrives as
   * ctrl+wheel with no ctrl key held, and that is the gesture this wants, so it
   * is deliberately not filtered out.
   *
   * Allocates nothing: `onNav` takes three numbers and a static string.
   */
  _wheel(e) {
    // Nothing wired means nothing handled, and the browser's own behaviour is a
    // more honest fallback than swallowing the event into a surface that will
    // not act on it.
    if (!this.vm || !this.onNav) return;
    e.preventDefault();

    const r = this.band.getBoundingClientRect();
    // Clamped into the band: a wheel over the track heads is still a wheel over
    // the arrangement, and a negative x would anchor a zoom on a tick that is
    // not on screen.
    const px = Math.max(0, Math.min(r.width, e.clientX - r.left));
    const view = this.vm.view;

    if (e.ctrlKey || e.metaKey) {
      const steps = this._zoomSteps(wheelPixels(e.deltaY, e.deltaMode, r.height));
      if (!steps) return;
      // Read the anchor BEFORE the zoom changes, from the projection currently on
      // screen. Deriving it afterwards from the new ticksPerPixel is the same
      // arithmetic with the wrong number in it, and it drifts a little further
      // every step — the failure looks like "zoom is slightly off" rather than
      // like a bug.
      const anchor = view.startTick + px * view.ticksPerPixel;
      const zi = clampZoom(this.vm.zoom.index + steps);
      // At either end of the table the anchored start is the start it already
      // has, so this would be a draw that changes nothing. Every branch below
      // makes the same check for the same reason: a callback that fires on a
      // no-op turns a held wheel at the limit into a redraw per event.
      if (zi === this.vm.zoom.index) return;
      this._lastNav = 'zoom';
      this.onNav(zi, anchoredStart(anchor, px, ticksPerPixelAt(zi)), this._laneScroll(), 'zoom');
      return;
    }

    // Which axis carries the value. Shift+wheel arrives as deltaX on macOS and on
    // Chrome for Windows, and as deltaY on devices that leave the swap to the
    // app, so the gesture reads whichever one is actually moving rather than the
    // one that happened to be plugged in when this was written.
    const horizontal = Math.abs(e.deltaX) > Math.abs(e.deltaY);

    if (e.shiftKey || horizontal) {
      // One pixel of wheel is one pixel of timeline, so the material tracks the
      // gesture exactly. A trackpad's horizontal swipe gets here with no modifier
      // at all, which is what a two-axis surface should do with a two-axis input.
      const dx = wheelPixels(horizontal ? e.deltaX : e.deltaY, e.deltaMode, r.width);
      const start = Math.max(0, view.startTick + dx * view.ticksPerPixel);
      if (start === view.startTick) return;
      this._lastNav = 'pan';
      this.onNav(this.vm.zoom.index, start, this._laneScroll(), 'pan');
      return;
    }

    // Plain wheel: the lanes, which is the axis this surface stacks tracks on.
    const dy = wheelPixels(e.deltaY, e.deltaMode, r.height);
    const ls = Math.max(0, Math.min(this._maxLaneScroll(), this._laneScroll() + dy));
    if (ls === this._laneScroll()) return;
    this._lastNav = 'lanes';
    this.onNav(this.vm.zoom.index, view.startTick, ls, 'lanes');
  }

  /** Grow pools to the shape being drawn. Called only when the shape changes. */
  resize(laneCount, laneHeight) {
    if (this.laneCount === laneCount && this._laneHeight === laneHeight) return;
    this.laneCount = laneCount;
    this._laneHeight = laneHeight;

    while (this.lanePool.length < laneCount) {
      const i = this.lanePool.length;
      const lane = div('ar-lane', this.lanesEl);
      lane.dataset.track = String(i);
      this.lanePool.push(lane);
      const head = div('ar-head', this.headsEl);
      head.dataset.track = String(i);
      const nm = div('ar-head-name', head);
      nm.appendChild(document.createTextNode(''));
      const lpb = div('ar-head-lpb', head);
      lpb.appendChild(document.createTextNode(''));
      head._nm = nm.firstChild; head._lpb = lpb.firstChild; head._lpbVal = -1;
      this.headPool.push(head);
    }
    for (let i = 0; i < this.lanePool.length; i++) {
      const on = i < laneCount;
      const lane = this.lanePool[i], head = this.headPool[i];
      const disp = on ? '' : 'none';
      if (lane.style.display !== disp) lane.style.display = disp;
      if (head.style.display !== disp) head.style.display = disp;
      if (!on) continue;
      const y = i * laneHeight;
      if (lane._y !== y) { lane._y = y; lane.style.transform = `translateY(${y}px)`; }
      if (lane._h !== laneHeight) { lane._h = laneHeight; lane.style.height = `${laneHeight}px`; }
      if (head._h !== laneHeight) { head._h = laneHeight; head.style.height = `${laneHeight}px`; }
    }
  }

  _grid(n) {
    while (this.gridPool.length < n && this.gridPool.length < GRID_POOL) {
      this.gridPool.push(div('ar-gridline', this.gridIn));
    }
    return this.gridPool;
  }

  _ruler(n) {
    while (this.rulerPool.length < n) {
      const el = div('ar-tick', this.rulerIn);
      el.appendChild(document.createTextNode(''));
      el._x = -1; el._bar = -1;
      this.rulerPool.push(el);
    }
    return this.rulerPool;
  }

  _clip(n) {
    while (this.clipPool.length < n) {
      const el = div('ar-clip', this.clipsIn);
      const label = div('ar-clip-name', el);
      label.appendChild(document.createTextNode(''));
      el._label = label.firstChild;
      el._x = -1; el._w = -1; el._y = -1; el._name = null;
      el._pTrack = -1; el._pTick = -1; el._bad = false;
      el._pId = -1; el._pEnd = -1;
      el._narrow = false; el._dragging = false;
      this.clipPool.push(el);
    }
    return this.clipPool;
  }

  /**
   * Where the waveform layer gets its data.
   *
   * Wired after construction rather than through the constructor because the
   * page declares `waveCache` and `requestWaveform` well below the renderer it
   * builds, and a constructor argument would read them in the temporal dead zone
   * — the silent failure GUIDELINES 2.2 is about. Nothing draws until this is
   * called, and `probe()` says so, so "not wired" is distinguishable from
   * "wired and drawing nothing".
   */
  bindAudio(cache, request) {
    this.waveCache = cache || null;
    this.waveRequest = request || null;
    this._wvRev = -1;                    // whatever is painted predates the data
  }

  /**
   * The waveform's colours, from the stylesheet rather than repeated here — the
   * same contract `scope.js` and `minimap.js` have, and for the same reason: a
   * canvas is the one surface where a missing token paints a plausible black
   * rectangle instead of failing. `waveThemed` reports it, and the read is
   * retried while it is false because a host that is not in the document yet has
   * no computed style to read.
   */
  _readWaveTheme() {
    const s = getComputedStyle(this.host);
    const pick = (n) => (s.getPropertyValue(n) || '').trim();
    this.waveColors.body = pick('--base-accent-ramp-400');
    this.waveColors.fail = pick('--uni-text-fx');
    this.waveThemed = !!(this.waveColors.body && this.waveColors.fail);
  }

  /**
   * Did the visible clips change since the last paint? Records them either way.
   *
   * Six numbers per clip, and they are the six the painting reads: which lane,
   * which span of the timeline, which source, where in that source, and whether
   * the source decoded. `x` and `w` are left out because both are
   * `tick / ticksPerPixel` and the zoom is already a guard — adding them would
   * be naming the same input twice.
   */
  _waveClipsMoved(vm, spanLoTick, spanHiTick) {
    const n = vm.clipCount * 6;
    if (this._wvSig.length < n) this._wvSig = new Float64Array(n + 64);
    const s = this._wvSig;
    let k = 0;
    let moved = false;
    for (let i = 0; i < vm.clipCount; i++) {
      const c = vm.clips[i];
      // Clips OUTSIDE the painted band are not part of the picture, so they must
      // not be part of its key. The model deliberately hands over more than the
      // screen holds (ARRANGE_CLIP_MARGIN_PX) so that this set is the band's and
      // not the viewport's — one moves when the band is re-centred, the other
      // every time a region crosses the screen edge.
      //
      // Compared in TICKS rather than in the pixels the band was measured in:
      // `startTick`/`endTick` are integers where `x`/`w` are fractions, and this
      // runs over every visible clip on every frame. Reading two double fields per
      // clip here cost 60-200 bytes a frame to answer a question the integers
      // answer exactly.
      if (c.endTick <= spanLoTick || c.startTick >= spanHiTick) continue;
      const o = k * 6;
      k++;
      if (s[o] !== c.track || s[o + 1] !== c.startTick || s[o + 2] !== c.endTick
          || s[o + 3] !== c.srcId || s[o + 4] !== c.startFrame
          || s[o + 5] !== c.srcStatus) moved = true;
      s[o] = c.track; s[o + 1] = c.startTick; s[o + 2] = c.endTick;
      s[o + 3] = c.srcId; s[o + 4] = c.startFrame; s[o + 5] = c.srcStatus;
    }
    if (this._wvSigN !== k) moved = true;
    this._wvSigN = k;
    return moved;
  }

  /**
   * Paint the waveform layer, if anything about it moved.
   *
   * The early return is the point of the whole file: at rest, and during a pan
   * that stays inside the painted band, this walks a handful of guards and does
   * nothing at all.
   */
  _waves(vm, laneH) {
    if (!this.waveCache) return;
    if (!this.waveThemed) this._readWaveTheme();
    if (!this.waveThemed) return;

    const dpr = window.devicePixelRatio || 1;
    const width = vm.view.width;
    const tpp = vm.view.ticksPerPixel;
    const sx = vm.scrollX;

    // The band to paint. Aligned to a whole DEVICE pixel so a column drawn at
    // canvas x lands on the same physical pixel it would have at any other pan
    // offset — a fractional origin resamples the whole waveform on every repaint
    // and makes it shimmer.
    let spanW = width + 2 * WAVE_OVERSCAN_PX;
    if (spanW * dpr > WAVE_MAX_DEVICE_PX) spanW = WAVE_MAX_DEVICE_PX / dpr;
    const outside = this._wvSpanW <= 0 || sx < this._wvSpanX
                 || sx + width > this._wvSpanX + this._wvSpanW;
    const spanX = outside
      ? Math.max(0, Math.floor((sx - WAVE_OVERSCAN_PX) * dpr) / dpr)
      : this._wvSpanX;

    const contentH = vm.laneCount * laneH;
    const devW = Math.max(1, Math.min(WAVE_MAX_DEVICE_PX, Math.round(spanW * dpr)));
    const devH = Math.max(1, Math.min(WAVE_MAX_DEVICE_PX, Math.round(contentH * dpr)));

    // Every input to the picture, compared before anything is written. The
    // retry clock is last because it is the only one that is not a fact about
    // what is on screen: it exists so a window that never arrives is asked for
    // again without turning the miss into a repaint per frame.
    const moved = this._waveClipsMoved(vm, spanX * tpp, (spanX + spanW) * tpp);
    const stale = this._wvIncomplete
               && (this._wvRev !== vm.waveRevision
                   || performance.now() >= this._wvNextTry);
    if (!moved && !outside && !stale
        && this._wvTpp === tpp && this._wvLaneH === laneH
        && this._wvLanes === vm.laneCount && this._wvDpr === dpr
        && this._wvRev === vm.waveRevision
        && this._wvDevW === devW && this._wvDevH === devH) return;

    this._wvSpanX = spanX; this._wvSpanW = spanW;
    this._wvTpp = tpp; this._wvLaneH = laneH; this._wvLanes = vm.laneCount;
    this._wvDpr = dpr; this._wvRev = vm.waveRevision;
    this._wvNextTry = performance.now() + WAVE_RETRY_MS;

    if (this._wvDevW !== devW || this._wvDevH !== devH) {
      this._wvDevW = devW; this._wvDevH = devH;
      this.waveCanvas.width = devW;
      this.waveCanvas.height = devH;
      this.waveCanvas.style.width = `${spanW}px`;
      this.waveCanvas.style.height = `${contentH}px`;
    }
    // The canvas sits at the band's ABSOLUTE origin inside the scrolled wrapper,
    // like everything else in this layer (GUIDELINES 3.3). Guarded on the number
    // so a repaint that did not move the band writes no string.
    if (this._wvTx !== spanX) {
      this._wvTx = spanX;
      this.waveCanvas.style.transform = `translateX(${spanX}px)`;
    }

    const ctx = this.waveCtx;
    ctx.clearRect(0, 0, devW, devH);
    this.waveRepaints++;
    this._wvIncomplete = false;
    this.waveDrawn = 0; this.waveWanted = 0; this.waveHeld = 0;

    const insetDev = Math.max(1, Math.round(WAVE_INSET_PX * dpr));
    const laneDev = laneH * dpr;

    ctx.fillStyle = this.waveColors.body;
    for (let i = 0; i < vm.clipCount; i++) {
      const c = vm.clips[i];
      if (!c.audio) continue;
      // The clip's span, clipped to the painted band, in canvas device pixels.
      const dx0 = Math.max(0, Math.floor((c.x - spanX) * dpr));
      const dx1 = Math.min(devW, Math.ceil((c.x + c.w - spanX) * dpr));
      if (dx1 <= dx0) continue;

      const top = c.track * laneDev + insetDev;
      const usable = laneDev - insetDev * 2;
      if (usable < 2) continue;

      if (c.srcStatus === 2) {
        // A file that would not decode. Blank would be indistinguishable from
        // silence, which is the one thing it must not look like.
        ctx.fillStyle = this.waveColors.fail;
        const y = Math.round(top + usable / 2) - 1;
        for (let x = dx0; x < dx1; x += WAVE_FAIL_DASH * 2) {
          ctx.fillRect(x, y, Math.min(WAVE_FAIL_DASH, dx1 - x), 2);
        }
        ctx.fillStyle = this.waveColors.body;
        this.waveDrawn++;
        continue;
      }
      // Status 1 is READY. 0 means the table has the source but the decode has not
      // finished, and nothing is asked for until it has — a "not ready" answer is
      // cached under the same key as the real one would be, so asking early is how
      // a source ends up permanently blank. The table republishes when the decode
      // lands, which clears the cache and repaints.
      if (c.srcStatus !== 1) continue;
      if (!c.srcId || !(c.ticksPerFrame > 0) || !(c.srcFrames > 0)) continue;

      // Source frames per DEVICE pixel, and the power of two at or below it. One
      // bucket is then at most one device pixel wide, so a column never has to
      // interpolate — it aggregates whole buckets, or at the finest levels reads
      // one. decimation 1 is raw samples, where min === max === the sample; the
      // 1-device-pixel floor below is what keeps that from drawing nothing.
      const framesPerPx = (tpp / dpr) / c.ticksPerFrame;
      let dec = 1;
      while (dec * 2 <= framesPerPx) dec *= 2;
      // Frame at canvas x = 0, and how fast frames run per device pixel.
      const frame0 = c.startFrame + (spanX * tpp - c.startTick) / c.ticksPerFrame;
      const fStart = Math.max(0, frame0 + dx0 * framesPerPx);
      const fEnd = Math.min(c.srcFrames, frame0 + dx1 * framesPerPx);
      if (fEnd <= fStart) continue;

      const tileFrames = dec * WAVE_TILE_COLS;
      const first = Math.floor(fStart / tileFrames);
      const last = Math.floor((fEnd - 1) / tileFrames);
      for (let t = first; t <= last; t++) {
        const tileFirst = t * tileFrames;
        if (tileFirst >= c.srcFrames) break;
        this.waveWanted++;
        const key = c.srcId + ':' + dec + ':' + tileFirst;
        const win = this.waveCache.get(key);
        if (!win || !win.pairs || win.columns === 0) {
          if (!win) {
            this._wvIncomplete = true;
            // Never blocks and never retries in a loop: the page's own request
            // is already bounded in flight and deduped against the cache.
            if (this.waveRequest) {
              this.waveRequest(c.srcId, dec, tileFirst, WAVE_TILE_COLS, WAVE_MASK_BOTH);
            }
          }
          continue;
        }
        this.waveHeld++;
        this._waveWindow(win, dec, frame0, framesPerPx, dx0, dx1, top, usable);
        this.waveDrawn++;
      }
    }
  }

  /**
   * One cached window, over the device pixels it covers.
   *
   * Q15 to lane: `y = mid - (q / 32768) * half`, where full scale is +/-1.0 and
   * NOT the file's own peak. Normalising to `absPeak` would make two clips of the
   * same material draw differently the moment one of them held a louder moment,
   * and would make a quiet recording indistinguishable from a loud one.
   *
   * Stereo splits the lane into two half-height bands, channel 0 above channel 1.
   * A mono downmix is not a lossy shortcut, it is FALSE: an out-of-phase pair
   * sums to silence, so the fixture's stereo file — whose right channel is the
   * exact negation of its left — would draw all eight seconds of a full-scale
   * signal as a flat line.
   */
  _waveWindow(win, dec, frame0, framesPerPx, dx0, dx1, top, usable) {
    const ctx = this.waveCtx;
    const pairs = win.pairs;
    const cols = win.columns;
    const ch = win.channels > 0 ? win.channels : 1;
    const winFirst = win.firstFrame;
    const winEnd = winFirst + cols * dec;

    // Only the device pixels this window actually covers. Walking the whole clip
    // per window and skipping would be O(clip x windows) for no gain.
    let a = Math.max(dx0, Math.ceil((winFirst - frame0) / framesPerPx));
    let b = Math.min(dx1, Math.ceil((winEnd - frame0) / framesPerPx));
    if (b <= a) return;

    const bandH = usable / ch;
    for (let c = 0; c < ch; c++) {
      const base = c * cols * 2;
      const bandTop = top + c * bandH;
      const bandBot = bandTop + bandH;
      const mid = bandTop + bandH / 2;
      const half = bandH / 2;
      for (let x = a; x < b; x++) {
        // The frames this device pixel covers, as bucket indices. `dec` is at or
        // below one pixel's worth of frames, so this is one or two buckets.
        const f0 = frame0 + x * framesPerPx;
        let i0 = Math.floor((f0 - winFirst) / dec);
        let i1 = Math.ceil((f0 + framesPerPx - winFirst) / dec);
        if (i0 < 0) i0 = 0;
        if (i1 > cols) i1 = cols;
        if (i1 <= i0) continue;
        let lo = 32767, hi = -32768;
        for (let i = i0; i < i1; i++) {
          const mn = pairs[base + i * 2], mx = pairs[base + i * 2 + 1];
          if (mn < lo) lo = mn;
          if (mx > hi) hi = mx;
        }
        let yt = mid - (hi / 32768) * half;
        let yb = mid - (lo / 32768) * half;
        if (yt < bandTop) yt = bandTop;
        if (yb > bandBot) yb = bandBot;
        let t = Math.round(yt), bt = Math.round(yb);
        // A MINIMUM OF ONE DEVICE PIXEL. min === max wherever the material is
        // constant — silence, DC, and every column at decimation 1 — and a
        // rectangle from min to max is then zero pixels tall, so the finest zoom
        // and every silent passage would draw an empty lane.
        if (bt - t < 1) {
          if (t >= bandBot) t = bandBot - 1;
          bt = t + 1;
        }
        ctx.fillRect(x, t, 1, bt - t);
      }
    }
  }

  render(vm) {
    this.vm = vm;
    const lh = vm.lanes.length ? vm.lanes[0].height : 44;
    this.resize(vm.laneCount, lh);

    // Three strips move together, by ONE transform each rather than by rebinding
    // every lane, head and clip (GUIDELINES 3.3). The grid and the playhead
    // deliberately do not move: they are full-height vertical lines and a bar
    // line that slid with the tracks would be a bar line pointing at the wrong
    // bar. Guarded on the number, so a redraw at the same offset writes nothing.
    const ls = vm.laneScroll || 0;
    if (this._ls !== ls) {
      this._ls = ls;
      const t = `translateY(${-ls}px)`;
      this.lanesEl.style.transform = t;
      this.clipsEl.style.transform = t;
      this.headsEl.style.transform = t;
    }

    // The time axis, the same way (GUIDELINES 3.3). Its contents are at absolute
    // x, so this ONE string is the whole of what a pan writes — nothing inside
    // the wrappers moves at all.
    //
    // The guard is the drawn number itself: `sx` is `startTick / ticksPerPixel`,
    // so it names both of its inputs and there is no way for the axis to move
    // while the key stands still (GUIDELINES 2.1). A ZOOM is the case worth
    // stating: it changes every element's absolute x, and every element is
    // guarded on that x below, so a zoom rebinds all of them — correctly, and as
    // a keystroke rather than as a frame.
    //
    // The cached number is the model's OWN `scrollX`, and the sign is flipped
    // inside the string. Caching `-vm.scrollX` instead reads like the same thing
    // and costs 23 bytes a frame FOREVER, including at rest: negating zero gives
    // -0, which is outside the small-integer range, so V8 boxed a heap number on
    // every draw to hold a view that had not moved. It is the guard rule one
    // step further in — cache the number you were GIVEN, not one derived from it,
    // because a derivation as small as a sign is still an allocation.
    const sx = vm.scrollX;
    if (this._sx !== sx) {
      this._sx = sx;
      const xf = `translateX(${-sx}px)`;
      this.rulerIn.style.transform = xf;
      this.gridIn.style.transform = xf;
      this.clipsIn.style.transform = xf;
    }

    for (let i = 0; i < vm.laneCount; i++) {
      const lane = vm.lanes[i], head = this.headPool[i];
      if (head._nm.nodeValue !== lane.name) head._nm.nodeValue = lane.name;
      if (head._lpbVal !== lane.lpb) {
        head._lpbVal = lane.lpb;
        head._lpb.nodeValue = lane.lpb ? lane.lpb + '/b' : '';
      }
    }

    // Ruler and grid are slotted as a RING, not as a list: an element's slot is
    // its own identity — the label index, the gridline index — modulo the pool
    // size, exactly as the tracker's rows are (GUIDELINES 3.4). Absolute x alone
    // is not enough without this. Indexed by position, panning past one gridline
    // shifts every later line down a slot, so every element takes a new tick and
    // rebinds; the whole saving would evaporate four frames into a pan. Slotted
    // by identity, the one line that scrolled off is the one element that moves.
    const ruler = this._ruler(vm.rulerCount);
    const rn = ruler.length, rShown = Math.min(vm.rulerCount, rn);
    const rBase = ((vm.rulerFirst % rn) + rn) % rn;
    for (let i = 0; i < rShown; i++) {
      const el = ruler[(rBase + i) % rn];
      if (el.style.display === 'none') el.style.display = '';
      const x = vm.ruler[i];
      if (el._x !== x) { el._x = x; el.style.transform = `translateX(${x}px)`; }
      const bar = vm.rulerBar[i];
      if (el._bar !== bar) { el._bar = bar; el.firstChild.nodeValue = barLabel(bar); }
    }
    // The slots this frame did not claim. Walking on from where the shown ones
    // stopped visits each of them exactly once, whatever the ring's phase is.
    for (let i = rShown; i < rn; i++) {
      const el = ruler[(rBase + i) % rn];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    const grid = this._grid(vm.gridCount);
    const gn = grid.length, gShown = Math.min(vm.gridCount, gn);
    const gBase = ((vm.gridFirst % gn) + gn) % gn;
    for (let i = 0; i < gShown; i++) {
      const el = grid[(gBase + i) % gn];
      if (el.style.display === 'none') el.style.display = '';
      const x = vm.grid[i];
      if (el._x !== x) { el._x = x; el.style.transform = `translateX(${x}px)`; }
      const bar = vm.gridIsBar[i] === 1;
      if (el._bar !== bar) { el._bar = bar; el.classList.toggle('bar', bar); }
    }
    for (let i = gShown; i < gn; i++) {
      const el = grid[(gBase + i) % gn];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    // Clips are slotted by the EXTENT'S index, not by their position in the
    // visible list — the same rule as the ruler and the grid above (GUIDELINES
    // 3.4), and for the same reason. By position, a region scrolling off the left
    // shifts every later region into a different element, so every one of them
    // takes a new transform, width and placement key on a pan that moved nothing.
    // By extent index, a clip keeps its element for as long as the engine keeps
    // its extent, and a pan writes nothing at all.
    const clips = this._clip(Math.max(vm.clipSlots, vm.clipCount));
    if (this._clipSeen.length < clips.length) this._clipSeen = new Uint8Array(clips.length + 16);
    const seen = this._clipSeen;
    // The clip being dragged is dimmed, so the ghost reads as the live one.
    const drag = this._clipDrag;
    seen.fill(0);
    for (let i = 0; i < vm.clipCount; i++) {
      const c = vm.clips[i];
      const slot = c.slot < clips.length ? c.slot : i;
      seen[slot] = 1;
      const el = clips[slot];
      if (el.style.display === 'none') el.style.display = '';
      const y = c.track * lh;
      if (el._x !== c.x || el._y !== y) {
        el._x = c.x; el._y = y;
        el.style.transform = `translate(${c.x}px, ${y}px)`;
      }
      if (el._w !== c.w) { el._w = c.w; el.style.width = `${c.w}px`; }
      if (el._h !== lh) { el._h = lh; el.style.height = `${lh}px`; }
      if (el._name !== c.name) { el._name = c.name; el._label.nodeValue = c.name; }
      if (el._audio !== c.audio) { el._audio = c.audio; el.classList.toggle('audio', c.audio); }
      // A source that would not decode, said on the block as well as on the
      // canvas: a region narrower than the stripe still has to look wrong.
      const bad = c.srcStatus === 2;
      if (el._bad !== bad) { el._bad = bad; el.classList.toggle('failed', bad); }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      // The placement key the page and `hitTest` compare against, guarded on the
      // two numbers it is made of rather than on itself: building the string to
      // find out whether it had changed cost one string per visible clip per
      // frame to conclude that a clip nobody moved is still where it was.
      // (track, startTick) is every input to the key, so the attribute still
      // follows the moment either of them moves.
      if (el._pTrack !== c.track || el._pTick !== c.startTick) {
        el._pTrack = c.track; el._pTick = c.startTick;
        el.dataset.placement = c.track + ':' + c.startTick;
      }
      // The STABLE id a drag is keyed on, and the far edge a trim needs. Neither
      // is drawn, so neither is guarded — these are two plain number writes, and
      // a comparison to avoid them costs more than they do.
      el._pId = c.id; el._pEnd = c.endTick;
      // Too narrow to hold two handles and a body: the handles stand down so the
      // whole block still moves. Guarded, because it is a class write.
      const narrow = c.w < CLIP_HANDLE_PX * 3;
      if (el._narrow !== narrow) { el._narrow = narrow; el.classList.toggle('narrow', narrow); }
      const dragging = drag !== null && drag.id === c.id;
      if (el._dragging !== dragging) {
        el._dragging = dragging; el.classList.toggle('dragging', dragging);
      }
    }
    // Slots nothing claimed this frame. Hidden, never removed (GUIDELINES 3.7).
    for (let s = 0; s < clips.length; s++) {
      if (seen[s]) continue;
      const el = clips[s];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    // Guarded on (on, x, w), which is the whole of what gets drawn. The key used
    // to be the string `x + ':' + w`, rebuilt on every frame the loop was set —
    // so a loop nobody is dragging, which is a loop's normal state, allocated a
    // string per frame for the length of the session.
    const lp = vm.loop;
    if (this._lpOn !== lp.on || this._lpX !== lp.x || this._lpW !== lp.w) {
      this._lpOn = lp.on; this._lpX = lp.x; this._lpW = lp.w;
      if (!lp.on) this.loop.style.display = 'none';
      else {
        if (this.loop.style.display === 'none') this.loop.style.display = '';
        this.loop.style.transform = `translateX(${lp.x}px)`;
        this.loop.style.width = `${lp.w}px`;
      }
    }

    // Last, after the clip blocks have been placed: the canvas is behind them and
    // both are read from the same view-model, so painting after them keeps the
    // two in step within one frame rather than one frame apart.
    this._waves(vm, lh);

    const px = vm.playheadX;
    if (this._px !== px) {
      this._px = px;
      if (px < 0) this.playhead.style.display = 'none';
      else {
        if (this.playhead.style.display === 'none') this.playhead.style.display = '';
        this.playhead.style.transform = `translateX(${px}px)`;
      }
    }
  }

  /** Which lane and tick a point falls on. Null outside the lanes. */
  hitTest(clientX, clientY, vm) {
    // The LANE STRIP's box, not the band's. The band starts at the top of the
    // ruler, so measuring from it put every click 24px — better than half a lane
    // — into the track above, and a click on the ruler resolved to lane 0. It is
    // also the box the lane scroll is applied to, so reading it here means the
    // hit test follows a scrolled strip for free instead of needing its own copy
    // of the offset to keep in step.
    const r = this.lanesEl.getBoundingClientRect();
    const ls = vm.laneScroll || 0;
    // The rect is already the TRANSFORMED one, so this is content space. Content
    // above `ls` is scrolled off the top and what is drawn there is the ruler,
    // which owns the loop drag — resolving it to a lane would steal the click.
    const x = clientX - r.left, y = clientY - r.top;
    if (x < 0 || y < ls) return null;
    const lh = vm.lanes.length ? vm.lanes[0].height : 44;
    const track = Math.floor(y / lh);
    if (track < 0 || track >= vm.laneCount) return null;
    const tick = vm.view.startTick + x * vm.view.ticksPerPixel;
    // `x` is a VIEWPORT pixel and `c.x` is an ABSOLUTE one, so the comparison has
    // to happen in one space or every click lands `scrollX` pixels off — which
    // is nothing at all until the view is panned, and then is silently wrong
    // rather than broken. The clip strip is translated by `-scrollX`, so undoing
    // that translation is what puts the pointer back in the clips' own space.
    const ax = x + (vm.scrollX || 0);
    // A hit on a placement is more specific than a hit on empty lane, and the
    // caller almost always wants the placement, so resolve it here.
    for (let i = 0; i < vm.clipCount; i++) {
      const c = vm.clips[i];
      if (c.track === track && ax >= c.x && ax < c.x + c.w) {
        return { track, tick, placement: c.track + ':' + c.startTick, id: c.id, clip: c };
      }
    }
    return { track, tick, placement: null, clip: null };
  }

  /** Structure for tests and agents — the same contract the tracker's probe has. */
  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      lanes: vm.laneCount,
      clips: vm.clipCount,
      audioClips: (() => { let n = 0; for (let i = 0; i < vm.clipCount; i++) if (vm.clips[i].audio) n++; return n; })(),
      zoom: vm.zoom.label,
      zoomIndex: vm.zoom.index,
      startTick: vm.view.startTick,
      ticksPerPixel: vm.view.ticksPerPixel, width: vm.view.width,
      // The wheel gesture, assertable without a pointer. The interaction itself
      // still has to be driven with a real wheel (GUIDELINES 2.15) — these say
      // what the surface believes, not that anything reached it.
      laneScroll: this._laneScroll(),
      maxLaneScroll: this._maxLaneScroll(),
      lastNav: this._lastNav,
      /** Ticks per pixel of band, so a test can convert without re-deriving. */
      bandLeft: Math.round(this.band.getBoundingClientRect().left),
      gridLines: vm.gridCount,
      loop: vm.loop.on ? { x: Math.round(vm.loop.x), w: Math.round(vm.loop.w) } : null,
      rulerTicks: vm.rulerCount,
      firstBar: vm.rulerCount ? vm.rulerBar[0] : -1,
      /**
       * Ticks between consecutive bar NUMBERS — the song's bar times whatever
       * stride the ruler thinned to.
       *
       * Here because "the ruler is counting in 7/8" was otherwise only assertable
       * by eye, and a ruler in the wrong meter is the failure that looks most like
       * success: evenly spaced numbers, ascending, all of them wrong. Divide by
       * `rulerEvery` for the bar itself.
       */
      rulerStrideTicks: vm.rulerCount >= 2
        ? Math.round((vm.ruler[1] - vm.ruler[0]) * vm.view.ticksPerPixel) : 0,
      rulerEvery: vm.rulerCount >= 2 ? vm.rulerBar[1] - vm.rulerBar[0] : 0,
      playheadX: Math.round(vm.playheadX),
      domNodes: this.clipPool.length + this.lanePool.length + this.gridPool.length,
      /**
       * The waveform layer, in numbers a test can assert on before it goes near a
       * pixel — and the geometry it needs to find one when it does.
       *
       * `repaints` is the number this surface is really about: a pan inside the
       * painted band must not move it. `wanted` vs `held` says how much of what is
       * on screen has data behind it, so "the waveform is missing" and "the
       * waveform is empty" are different answers rather than the same blank lane.
       */
      wave: {
        bound: !!this.waveCache,
        themed: this.waveThemed,
        deviceWidth: this._wvDevW, deviceHeight: this._wvDevH, dpr: this._wvDpr,
        spanX: this._wvSpanX, spanW: this._wvSpanW,
        repaints: this.waveRepaints,
        clips: this.waveDrawn,
        wanted: this.waveWanted, held: this.waveHeld,
        incomplete: this._wvIncomplete,
        laneHeight: this._wvLaneH,
        inset: Math.max(1, Math.round(WAVE_INSET_PX * (this._wvDpr || 1))),
      },
    };
  }
}
