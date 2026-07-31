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
 * On since the engine gained cross-track MovePlacement. It was off for exactly
 * one reason, worth keeping written down: the engine used to refuse a
 * cross-track move WHOLE — not just the lane part — so a diagonal drag moved the
 * clip neither across nor along, which is the worst answer available. The
 * gesture looks like it worked right up until nothing happens.
 *
 * `test/placement.mjs` is what turned this back on: it asserted the refusal, and
 * failing was the signal. If the engine ever loses cross-track again, that test
 * fails first and says to come back here.
 */
const CROSS_TRACK_DRAG = true;

/**
 * How far the pointer must travel before a press becomes a drag.
 *
 * NOT cosmetic, and not about shaky hands. Capturing the pointer on pointerdown
 * RETARGETS every later pointer event to the capturing element — which means the
 * two clicks of a double-click no longer land on the clip, `dblclick` fires with
 * the container as its target, and `e.target.closest('.ar-clip')` returns null.
 * Double-clicking a clip silently stopped opening the piano roll, and the only
 * thing that noticed was the e2e suite.
 *
 * So a press arms nothing. The drag — and the capture — begins on the first move
 * past this threshold, which is also what a person means: pressing on a clip is
 * how you select it, and a hand that moves two pixels while clicking has not
 * asked for anything to move.
 */
const CLIP_DRAG_SLOP_PX = 3;

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
 * The narrowest rail that can carry the shared badge.
 *
 * Below this the badge's box hangs off the block and into the gutter beside it — visually
 * clipped by the block's own overflow, and still hit-testable out there, over another surface's
 * controls. Wide enough for "×12" plus its padding.
 */
const BADGE_MIN_W_PX = 26;

/**
 * A marker stores no length, so the `+` button has nothing to ask for.
 *
 * It puts one at the playhead, which is where "here" is, and the span it begins runs to whatever
 * comes next. That is the simplification markers brought: adding one is a single unambiguous
 * act, where adding a section had to choose a length before it could exist.
 */

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
              { onLoop, onNav, onClipSelect, onClipOpen, onClipEdit, onClipFork,
                onMarkerSelect, onTimeEdit, onMarkerRename,
                onMarkerAdd, onMarkerDelete, onAutomationWrite } = {}) {
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
    /*
     * Pressing the shared/forked badge. Absent, the badge is a READOUT and nothing more, which
     * is what it was before scratch clips existed and is still a useful thing for it to be.
     */
    this.onClipFork = onClipFork;
    /*
     * Where a point dragged on the curve goes. Absent, the mode can still be entered and the
     * lane still takes the pointer — and nothing is written, which is the right behaviour for a
     * surface bound without a writer and is exactly what a read-only arrangement should do.
     */
    this.onAutomationWrite = onAutomationWrite;
    /*
     * THE SPINE's three gestures. Absent, the strip still DRAWS — a read-only spine is
     * a useful spine, and it is what this surface was before the engine had marker ops — so
     * each is guarded independently rather than the strip being conditional on having a
     * controller.
     */
    this.onMarkerSelect = onMarkerSelect;
    this.onTimeEdit = onTimeEdit;
    this.onMarkerRename = onMarkerRename;
    this.onMarkerAdd = onMarkerAdd;
    this.onMarkerDelete = onMarkerDelete;

    this.gutter = div('ar-gutter', host);
    // Two boxes, not one: the outer clips at the lane strip's top edge so a head
    // scrolled off slides UNDER the ruler, and the inner is the thing that moves.
    // Translating the clipper would take its own clip rectangle with it.
    /*
     * The spine's gutter cell: what the row IS, and the two buttons that make one.
     *
     * In the GUTTER at the spine's own y, so it lines up with the strip it controls and
     * scrolls with neither axis. Add and remove were the two things the spine could
     * only be given from the console — a drag can change a length and a double-click a
     * name, but neither can bring a marker into existence — and console-only is not done.
     */
    this.spineHead = div('ar-spine-head', this.gutter);
    this.spineHeadLabel = div('ar-spine-label', this.spineHead);
    this.spineHeadLabel.appendChild(document.createTextNode('MARKERS'));
    this.spineAdd = div('ar-spine-btn', this.spineHead);
    this.spineAdd.appendChild(document.createTextNode('+'));
    this.spineAdd.title = 'add a marker at the playhead';
    this.spineDel = div('ar-spine-btn', this.spineHead);
    this.spineDel.appendChild(document.createTextNode('−'));
    this.spineDel.title = 'remove the selected marker — the music stays put';
    // pointerdown, not click: `element.click()` never fires pointerdown, so a handler
    // bound to the latter is unreachable from a driver that clicks — which is how a
    // bypass test once passed having never toggled anything (GUIDELINES 2.15).
    this.spineAdd.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      if (this.onMarkerAdd) this.onMarkerAdd();
    });
    this.spineDel.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      // Which marker: the SELECTED one, from the model — the same id the console
      // names. With none selected this says so rather than guessing at the last.
      // Which marker: the SELECTED one, from the model — the same id the console names.
      if (this.onMarkerDelete) this.onMarkerDelete(this.vm ? this.vm.selectedMarker : 0);
    });

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
    /*
     * THE SPINE STRIP, between the bar numbers and the lanes.
     *
     * Its own strip rather than a row inside the ruler, because the ruler owns the
     * loop drag across its whole height and a marker is a different target with a
     * different gesture on it — a boundary drag that ripples the song. Sharing one
     * box would mean one pointerdown deciding between two edits by y, which is the
     * shape of every "the click did the other thing" report in this file's history.
     *
     * Above the lanes and below the numbers, because that is where the structure is:
     * a marker names a point in TIME, so it belongs on the time axis, not in the
     * gutter where the tracks are named.
     */
    this.spine = div('ar-spine', this.band);
    this.spineIn = div('ar-scroll', this.spine);
    /**
     * The "there are more markers than I could publish" note. Pinned to the right
     * of the strip and OUTSIDE the scrolled wrapper: it is a statement about the list,
     * not about a position on the timeline, so it must not pan away.
     */
    /**
     * The unnamed tail: material the spine does not reach. Inside the SCROLLED wrapper,
     * because unlike the truncation marker it IS a position on the timeline.
     */
    this.spineTail = div('ar-span-tail', this.spineIn);
    this.spineMore = div('ar-spine-more', this.spine);
    this.spineMore.appendChild(document.createTextNode(''));
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
    /*
     * THE AUTOMATION LAYER, over the clips and under the playhead.
     *
     * Its own canvas rather than the waveform's, because the two are cached on entirely
     * different things: the waveform repaints when decoded audio or the view window moves, and
     * a curve repaints when somebody edits a point. Sharing one canvas would mean every
     * automation edit repainting every waveform on screen, and the wave layer's guard exists
     * precisely to stop that.
     *
     * OVER the clips, because a curve that runs under a region's wash is a curve you cannot
     * read across the loudest part of the song — which is where automation usually is.
     */
    this.autoCanvas = document.createElement('canvas');
    this.autoCanvas.className = 'ar-auto';
    this.clipsEl.appendChild(this.autoCanvas);
    this.autoCtx = this.autoCanvas.getContext('2d');
    /** What the last automation paint was a picture OF. Every input, or it goes stale. */
    this._auSig = '';
    this._auColor = '';
    this.autoRepaints = 0;
    /*
     * EDITING THE CURVE WITH A POINTER.
     *
     * The canvas is `pointer-events: none` by default and that is correct: the lane belongs to
     * the clips, and a curve that swallowed clicks would make every region in an automated track
     * undraggable. So editing is a MODE — `automationEdit` in the options — and while it is on
     * the canvas takes the pointer and the clips below do not.
     *
     * A mode rather than a modifier key because a modifier is invisible: there is no way to look
     * at the screen and know whether the next click will move a region or write a point, and
     * that is a bad property for a gesture that changes the music. The mode has a lit chip in
     * the chrome, the lane is tinted while it is on, and the console can turn it on and off —
     * which is also what makes it testable without inventing a keyboard event.
     *
     * WHAT IT CAN DO IS DELIBERATELY HALF OF WHAT IT SHOULD. `WriteAutomationPoint` can add a
     * point and replace one at the same tick, and there is no opcode to REMOVE a point. So a
     * point can be created and its value dragged; it cannot be moved in time (that is a write at
     * the new tick plus a remove at the old one, and without the remove it litters) and it
     * cannot be deleted. Backend has the request. The mode says so rather than offering a
     * gesture that half-works.
     */
    this._auDrag = null;
    this.autoWrites = 0;
    this.autoCanvas.addEventListener('pointerdown', (e) => this._autoDown(e));
    this.autoCanvas.addEventListener('pointermove', (e) => this._autoMove(e));
    this.autoCanvas.addEventListener('pointerup', (e) => this._autoUp(e));
    this.autoCanvas.addEventListener('pointercancel', () => this._autoCancel());

    this.playhead = div('ar-playhead', this.band);

    this.lanePool = [];
    this.headPool = [];
    this.clipPool = [];
    /** Which clip slots this frame claimed. Grown with the pool, never per frame. */
    this._clipSeen = new Uint8Array(0);
    this.gridPool = [];
    this.rulerPool = [];
    this.spinePool = [];
    this._spineMoreN = -1;
    this._tailOn = null; this._tailX = -1; this._tailW = -1;
    this.laneCount = 0;
    this.vm = null;

    // The waveform layer's state. Every one of these is a guard: the canvas is a
    // cache of a picture, and the picture is a function of exactly these.
    this.waveCache = null;              // the page's Map of windows; see bindAudio
    this.waveRequest = null;            // the page's requestWaveform
    this.waveColors = { body: '', fail: '', note: '' };
    // The engine's published notes, for the material drawn inside clips.
    this.engine = null;
    this.waveThemed = false;
    this._wvDevW = 0; this._wvDevH = 0; this._wvDpr = 0;
    this._wvSpanX = 0; this._wvSpanW = 0; this._wvTx = -1;
    this._wvTpp = -1; this._wvLaneH = -1; this._wvLanes = -1; this._wvRev = -1;
    this._wvIncomplete = false; this._wvNextTry = 0;
    this._wvNoteRev = -1;
    // The visible clips as the last paint saw them, six numbers each. A pan
    // inside the painted band must not repaint, so "did the clips change?" has to
    // be answerable without a string and without a revision the model does not
    // have — and it has to name every input, not just the count (GUIDELINES 2.1).
    this._wvSig = new Float64Array(0); this._wvSigN = -1;
    this.waveRepaints = 0; this.waveDrawn = 0;
    this.waveWanted = 0; this.waveHeld = 0;

    /*
     * The spine's own handlers, on the spine's own box.
     *
     * NOT on the ruler, and this is the mechanical reason the strip is a separate strip:
     * `_loopDown` never looks at `e.target`, so ANY pointerdown inside `.ar-ruler`
     * starts a loop drag — the bar numbers and the loop bracket only avoid stealing it
     * by being `pointer-events: none`. A boundary handle has to be grabbable, so a grip
     * living in the ruler would force a `closest()` early-out into an existing gesture
     * whose failure is silent in both directions: either the edge drag also sets a loop,
     * or the ruler stops setting loops wherever a marker happens to be. Two boxes
     * cannot alias.
     */
    this.spine.addEventListener('pointerdown', (e) => this._spineDown(e));
    this.spine.addEventListener('pointermove', (e) => this._spineMove(e));
    this.spine.addEventListener('pointerup', (e) => this._spineUp(e));
    this.spine.addEventListener('pointercancel', () => this._spineCancel());
    this.spine.addEventListener('dblclick', (e) => this._spineDbl(e));

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

  /**
   * The automation lane under the pointer, with the geometry the curve is DRAWN with.
   *
   * Returns the same `top` and `height` `_paintAutomation` computes, from one place, because a
   * hit test that derived them separately would drift from the picture the moment either changed
   * — and "the point I grabbed is not the point I clicked" is an unfixable-feeling bug.
   */
  _autoLaneAt(e) {
    const vm = this.vm;
    if (!vm || !this.autoCurves) return null;
    const r = this.clipsEl.getBoundingClientRect();
    const y = e.clientY - r.top;
    for (let i = 0; i < vm.laneCount; i++) {
      const lane = vm.lanes[i];
      const top = lane.y - (vm.laneScroll || 0) + 3;
      const height = lane.height - 6;
      if (y < top || y > top + height) continue;
      const curve = this.autoCurves.forTrack(lane.track);
      // No curve means nothing automates this track — there is no lane to edit, and inventing
      // one here would be this surface deciding which parameter a click meant.
      if (!curve || !curve.param) return null;
      return { track: lane.track, param: curve.param, curve, top, height,
               value: Math.max(0, Math.min(1, 1 - (y - top) / height)) };
    }
    return null;
  }

  /**
   * Grab the point under the pointer, or make one there.
   *
   * The grabbed point's TICK is fixed for the whole gesture: a drag changes its value and never
   * its time, because moving it in time needs a remove that does not exist yet. Snapping the
   * tick at grab time rather than following the pointer is what keeps that honest — the point
   * stays where it was and only its height follows the hand.
   */
  _autoDown(e) {
    if (e.button !== 0) return;
    const at = this._autoLaneAt(e);
    if (!at) return;
    const vm = this.vm;
    const tpp = vm.view.ticksPerPixel;
    const r = this.clipsEl.getBoundingClientRect();
    const x = e.clientX - r.left;
    const tick = Math.max(0, Math.round(vm.view.startTick + x * tpp));
    // The nearest existing point within a finger's width, in PIXELS rather than ticks: the
    // tolerance a person feels is a distance on the glass, and at a different zoom the same tick
    // window is a different target.
    const HIT_PX = 6;
    let grabbed = -1, best = HIT_PX;
    const pts = (at.curve.points) || [];
    for (let k = 0; k < pts.length; k++) {
      const d = Math.abs((pts[k][0] - vm.view.startTick) / tpp - x);
      if (d <= best) { best = d; grabbed = k; }
    }
    const atTick = grabbed >= 0 ? pts[grabbed][0] : tick;
    // The lane's box is captured WITH the drag: see `_autoMove` for why the pointer wandering
    // into the next lane must not start reading that lane's geometry.
    this._auDrag = { track: at.track, param: at.param, tick: atTick, value: at.value,
                     created: grabbed < 0, top0: at.top, h0: at.height };
    this.autoCanvas.setPointerCapture(e.pointerId);
    this._autoWrite();
    e.preventDefault();
  }

  _autoMove(e) {
    const d = this._auDrag;
    if (!d) return;
    /*
     * The value comes from the ORIGINAL lane's box, not from whatever lane the pointer has
     * wandered into. A drag that crossed into the track below would otherwise start reading its
     * geometry and the value would jump — and the point being edited never changed lanes.
     */
    const r = this.clipsEl.getBoundingClientRect();
    const y = e.clientY - r.top;
    const v = Math.max(0, Math.min(1, 1 - (y - d.top0) / d.h0));
    if (Math.abs(v - d.value) < 0.002) return;   // below what a pixel can show
    d.value = v;
    this._autoWrite();
    // The overlay is what the hand is doing; repaint it now rather than waiting for the
    // engine's answer, or the point would lag the pointer by a round trip.
    this._paintNow();
    e.preventDefault();
  }

  _autoUp() { this._auDrag = null; this._paintNow(); }

  _autoCancel() { this._auDrag = null; this._paintNow(); }

  /**
   * Send the point, at most once per frame.
   *
   * A pointermove can fire far more often than the display refreshes, and every write is a
   * message the engine answers with a whole republished lane. Coalescing to the frame is the
   * difference between a drag and a flood; the value that lands is the last one, which is the
   * one the hand ended on.
   */
  _autoWrite() {
    const d = this._auDrag;
    if (!d) return;
    /*
     * THE PAYLOAD IS CAPTURED NOW, not read inside the frame.
     *
     * Reading `this._auDrag` in the callback lost every CLICK: a press and release inside one
     * frame sets the drag, schedules the write, and clears the drag before the frame runs — so
     * the callback found null and sent nothing. Dragging worked, because a drag keeps producing
     * moves across frames, which is exactly the kind of bug a console-driven test cannot see
     * and a pointer-driven one finds on its first click.
     *
     * The pending record is MUTATED rather than replaced, so a drag at 120Hz allocates nothing.
     */
    if (!this._auSend) this._auSend = { track: 0, param: '', tick: 0, value: 0 };
    this._auSend.track = d.track;
    this._auSend.param = d.param;
    this._auSend.tick = d.tick;
    this._auSend.value = d.value;
    if (this._auPending) return;
    this._auPending = true;
    requestAnimationFrame(() => {
      this._auPending = false;
      if (!this.onAutomationWrite) return;
      this.autoWrites++;
      this.onAutomationWrite(this._auSend);
    });
  }

  /** Repaint the curve layer now, bypassing the signature — the drag overlay changed. */
  _paintNow() {
    this._auSig = '';
    if (this.vm) this._paintAutomation(this.vm);
  }

  /**
   * The marker under the pointer, as the MODEL has it — not as the DOM does.
   *
   * From the model because the element is a pooled slot and its identity is the ring's
   * phase, so `e.target` alone answers "which element" and not "which marker". The id
   * is on the element for exactly this hop, and the model row is what carries the span
   * the drag needs.
   */
  _markerAt(e) {
    if (!this.vm) return null;
    const el = e.target && e.target.closest && e.target.closest('.ar-span');
    if (!el) return null;
    const id = Number(el.dataset.marker || 0);
    if (!id) return null;
    for (let i = 0; i < this.vm.markerCount; i++) {
      if (this.vm.markers[i].id === id) return { el, sec: this.vm.markers[i] };
    }
    return null;
  }

  /**
   * A pointerdown on the spine: the GRIP starts a boundary drag, anywhere else selects.
   *
   * The grip is told apart by its own dataset flag rather than by measuring the pointer
   * against the element's right edge — the edge test is the same arithmetic done twice,
   * once in CSS to draw the handle and once here to hit it, and the two drift the moment
   * the handle's width changes in only one of them.
   */
  _spineDown(e) {
    const hit = this._markerAt(e);
    if (!hit) return;
    const isGrip = !!(e.target && e.target.dataset && e.target.dataset.grip);
    if (isGrip && this.onTimeEdit) {
      /*
       * THE GRIP IS THE BOUNDARY, AND THE BOUNDARY IS THE NEXT MARKER.
       *
       * Dragging it does not resize anything — nothing stores a length any more. It INSERTS OR
       * REMOVES ARRANGEMENT TIME at the next marker's tick, which moves that marker and
       * everything at or after it: every placement on every track, the tempo points, the key
       * changes, the automation points, the meter points and the later markers, in one
       * transaction the engine can refuse whole and undo whole.
       *
       * So the gesture is unchanged and its meaning is now literal: the thing you grab is the
       * thing that moves.
       */
      if (!(hit.sec.bars > 0)) return;   // the last marker has no boundary to its right
      /*
       * THE SPAN'S OWN BAR LENGTH, from the ticks the engine resolved: `w * tpp / bars`. Not
       * `ticksPerBar(songMeter())` — a span that crosses a meter change has bars this file
       * cannot compute from one signature, and the drag would land on lengths the song cannot
       * have, which reads as the boundary sticking rather than as a wrong unit.
       */
      const tpp = this.vm.view.ticksPerPixel;
      const barTicks = (hit.sec.w * tpp) / hit.sec.bars;
      if (!(barTicks > 0)) return;
      this._secDrag = {
        // WHERE the time is inserted: the next marker's tick, which is this span's end.
        atTick: Math.round(hit.sec.x * tpp + hit.sec.w * tpp),
        barTicks, x0: e.clientX, el: hit.el, w0: hit.sec.w, bars: 0,
      };
      this.spine.setPointerCapture(e.pointerId);
      e.preventDefault();
      return;
    }
    if (this.onMarkerSelect) this.onMarkerSelect(hit.sec.id);
  }

  /**
   * The boundary, following the pointer — as a PREVIEW on this element's width only.
   *
   * Nothing is sent while dragging and nothing else moves: the engine plans the ripple across
   * the whole song and refuses it whole if a removal would take bars that hold material, so the
   * later markers and the clips must not slide until that answer comes back. What the strip
   * shows meanwhile is a width, which is honest — it is where the boundary WOULD go — and the
   * model overwrites it on the next frame after the engine speaks.
   */
  _spineMove(e) {
    const d = this._secDrag;
    if (!d) return;
    const bars = Math.round((e.clientX - d.x0) * this.vm.view.ticksPerPixel / d.barTicks);
    if (bars === d.bars) return;
    d.bars = bars;
    // At least a sliver: a span dragged to nothing would vanish under the pointer, and there is
    // no width at which the grip stops being grabbable.
    const w = Math.max(2, d.w0 + bars * d.barTicks / this.vm.view.ticksPerPixel);
    d.el._w = w;                       // keep the render guard in step with the preview
    d.el.style.width = `${w}px`;
  }

  _spineUp(e) {
    const d = this._secDrag;
    if (!d) return;
    this._secDrag = null;
    this.spine.releasePointerCapture(e.pointerId);
    // Unchanged is not an edit. Sending it would spend a whole-song transaction and an undo
    // entry on a click that landed back where it started.
    if (d.bars === 0) { d.el._w = d.w0; d.el.style.width = `${d.w0}px`; return; }
    this.onTimeEdit({ tick: d.atTick, bars: d.bars });
  }

  /** A cancelled drag reverts the preview. It must not commit an edit nobody chose. */
  _spineCancel() {
    const d = this._secDrag;
    if (!d) return;
    this._secDrag = null;
    d.el._w = d.w0;
    d.el.style.width = `${d.w0}px`;
  }

  /** Double-click renames. The page owns the prompt: this file knows about pixels. */
  _spineDbl(e) {
    const hit = this._markerAt(e);
    if (!hit || !this.onMarkerRename) return;
    this.onMarkerRename(hit.sec.id, hit.sec.name);
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
    /*
     * THE SHARED BADGE IS THE CONTROL.
     *
     * Claimed before the drag, because the badge sits inside the block and a press on it is also
     * a press on the block — without this order, pressing it would arm a move and then open the
     * thing it was pressed for on top of a drag.
     *
     * Making the READOUT the control is the point rather than a saving: the ×4 is where a person
     * finds out an edit would reach four regions, so it is where they will look for the way to
     * stop it. A separate menu item somewhere else would be a second thing to find, at the
     * moment they have just learned there was something to worry about.
     */
    if (e.target && e.target.classList && e.target.classList.contains('ar-clip-share')) {
      if (this.onClipFork) {
        this.onClipFork({ track: el._pTrack, placement: el._pId,
                          forked: el.classList.contains('forked') });
      }
      e.preventDefault();
      return;
    }
    if (!this.vm || !this.onClipEdit) return;
    // Only the primary button. A right-click is a context menu everywhere else
    // and starting a drag under one is how a clip ends up somewhere nobody
    // meant to put it.
    if (e.button !== 0) return;
    const r = el.getBoundingClientRect();
    const mode = clipZoneAt(e.clientX - r.left, r.width);
    // Armed, not started. Nothing is captured and no ghost is drawn until the
    // pointer moves — see CLIP_DRAG_SLOP_PX.
    this._clipDrag = {
      id: el._pId, mode, pointerId: e.pointerId,
      x0: e.clientX, y0: e.clientY,
      clip: { startTick: el._pTick, endTick: el._pEnd, track: el._pTrack },
      at: null, live: false,
    };
  }

  /** Where the current pointer position puts the clip. */
  _clipAt(e) { return this._clipAt2(this._clipDrag, e); }

  _clipAt2(d, e) {
    const tpp = this.vm.view.ticksPerPixel;
    const lh = this.vm.laneHeight || 1;
    const lanes = CROSS_TRACK_DRAG && d.mode === 'move'
      ? Math.round((e.clientY - d.y0) / lh) : 0;
    return dragPlacement(d.clip, d.mode, (e.clientX - d.x0) * tpp, lanes,
                         { fine: e.shiftKey, laneCount: this.laneCount,
                           meter: this.vm.meter });
  }

  _clipMove(e) {
    const d = this._clipDrag;
    if (!d) return;
    /*
     * NO BUTTON, NO DRAG.
     *
     * `pointerup` is bound to the band, and the pointer is only CAPTURED once the
     * gesture passes the slop threshold — deliberately, so a click stays a click and
     * `dblclick` still lands on the clip. Which leaves a window: press on a clip,
     * release before moving 3px and outside the band, and no pointerup ever reaches
     * this element. The drag stays armed. Move back over the arrangement later, with
     * no button held, and the clip follows the pointer — and the next click commits
     * the move.
     *
     * Escape already abandoned a drag whose pointerup was eaten, but that requires
     * knowing you are in one. `buttons` is the truth the browser gives us on every
     * move: zero means nothing is pressed, whatever we think we are doing.
     */
    if (e.buttons === 0) { this._clipCancel(); return; }
    if (!d.live) {
      if (Math.abs(e.clientX - d.x0) < CLIP_DRAG_SLOP_PX
          && Math.abs(e.clientY - d.y0) < CLIP_DRAG_SLOP_PX) return;
      d.live = true;
      // Captured HERE rather than on the press, so a click stays a click and
      // `dblclick` still lands on the clip. See CLIP_DRAG_SLOP_PX.
      this.clipsIn.setPointerCapture(d.pointerId);
      // Drawn at the clip's own position first, so the ghost appears where the
      // clip is and travels from there rather than materialising ahead of it.
      this._clipGhost(d.clip);
    }
    const at = this._clipAt(e);
    d.at = at;
    this._clipGhost(at);
  }

  _clipUp(e) {
    const d = this._clipDrag;
    if (!d) return;
    this._clipDrag = null;
    // A press that never moved: a selection, not an edit. Nothing was captured
    // and nothing is committed.
    if (!d.live) return;
    const at = this._clipAt2(d, e);
    try { this.clipsIn.releasePointerCapture(e.pointerId); } catch (err) { /* gone */ }
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
    const d = this._clipDrag;
    if (!d) return;
    this._clipDrag = null;
    if (!d.live) return;
    try { this.clipsIn.releasePointerCapture(d.pointerId); } catch (err) { /* already gone */ }
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

  /**
   * The spine's element pool. One block per marker, each with a name and a boundary
   * grip at its right edge — the grip is a child rather than a pseudo-element because
   * it is a TARGET, and a pseudo-element cannot be hit.
   */
  _spine(n) {
    while (this.spinePool.length < n) {
      const el = div('ar-span', this.spineIn);
      const label = div('ar-span-name', el);
      label.appendChild(document.createTextNode(''));
      const grip = div('ar-span-grip', el);
      grip.dataset.grip = '1';
      el._nm = label.firstChild;
      el._x = -1; el._w = -1; el._name = null; el._color = -1; el._id = 0;
      this.spinePool.push(el);
    }
    return this.spinePool;
  }

  _clip(n) {
    while (this.clipPool.length < n) {
      const el = div('ar-clip', this.clipsIn);
      const label = div('ar-clip-name', el);
      label.appendChild(document.createTextNode(''));
      el._label = label.firstChild;
      // The shared-count badge, at the right end. Its own node so it and the name do not
      // rebuild each other.
      const share = div('ar-clip-share', el);
      share.appendChild(document.createTextNode(''));
      el._share = share.firstChild;
      el._shared = null; el._forked = null; el._badge = null; el._title = null;
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
   * The engine store, for the notes drawn inside clips.
   *
   * Separate from `bindAudio` because they are separate dependencies with
   * separate lifetimes: the audio cache is the page's, filled by round trips,
   * and this is the published frame. Binding both through one call would make a
   * project with no audio unable to draw its notes.
   */
  /**
   * Where the automation curves come from.
   *
   * A reader object rather than the data, for the reason `bindAudio` takes a cache: the page owns
   * the fetching — a curve arrives on the ack channel after a request — and this surface owns the
   * drawing. `revision` is what the paint guards on, so the page can say "something changed"
   * without this file diffing point arrays every frame.
   *
   * Absent, nothing is drawn and nothing throws: a surface bound without a reader is what the
   * arrangement was before automation had a read-back at all.
   */
  bindAutomation(curves) {
    this.autoCurves = curves;
    // Force the next paint: the reader changing is exactly the case a signature built from the
    // OLD reader's revision cannot see.
    this._auSig = '';
  }

  bindEngine(engine) {
    this.engine = engine || null;
    this._wvNoteRev = -1;                // repaint against whatever just arrived
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
    // Notes are drawn BRIGHTER than the waveform body: inside a clip they are
    // the foreground, and at one device pixel tall they need the contrast to
    // read at all.
    this.waveColors.note = pick('--base-accent-ramp-200') || pick('--base-accent');
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
    // NOT gated on `waveCache` any more. That cache is the AUDIO half; the same
    // canvas now also draws the notes inside symbolic clips, and those need no
    // cache and no engine round trip — the published clip window is the whole
    // song (windowStart 0, windowEnd UINT64_MAX), so the notes are already here.
    // Gating on the cache meant a project with no audio painted nothing at all.
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
    // `noteRev` is in the guard for the same reason `waveRevision` is: an edit
    // that changes the notes changes the picture, and nothing else in this list
    // would notice. Without it, writing a note left the arrangement showing the
    // previous bar's material until something else forced a repaint.
    const noteRev = vm.noteRevision;
    if (!moved && !outside && !stale
        && this._wvTpp === tpp && this._wvLaneH === laneH
        && this._wvLanes === vm.laneCount && this._wvDpr === dpr
        && this._wvRev === vm.waveRevision && this._wvNoteRev === noteRev
        && this._wvDevW === devW && this._wvDevH === devH) return;
    this._wvNoteRev = noteRev;

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

    // The symbolic half of the same canvas. AFTER the waveforms so a clip that is both
    // (audio with an event lane) reads as notes over material rather than under it.
    this._notes(vm, ctx, spanX, dpr, devW, laneDev, insetDev);
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

  /*
   * THE NOTES INSIDE A CLIP.
   *
   * An arrangement whose blocks are blank rectangles tells you a part exists and
   * nothing about what it does. The whole reason to look at an arrangement
   * rather than a track list is to see the SHAPE of the music — where it is
   * busy, where it rests, where the line rises — and that is the note material,
   * drawn small.
   *
   * ON THE CANVAS, not as elements. A busy song publishes twenty thousand notes;
   * as DOM that is twenty thousand nodes with one style write each, which is the
   * exact shape GUIDELINES 3 exists to keep out of this app. On the canvas it is
   * one pass and it happens only when the guard above says the picture moved.
   *
   * ONE PASS OVER THE NOTES, not one per clip. Per clip it is O(clips x notes) —
   * on the stress fixtures that is six clips against twenty thousand notes for
   * every repaint. Walking the notes once and asking which clip each falls in is
   * the same picture for a sixth of the work.
   */
  _notes(vm, ctx, spanX, dpr, devW, laneDev, insetDev) {
    const eng = this.engine;
    if (!eng || !eng.noteCount) return;
    const tpp = vm.view.ticksPerPixel;
    // Inset further than the waveform: a note touching the lane's edge reads as
    // part of the clip's border rather than as material inside it.
    const pad = insetDev + Math.max(1, Math.round(dpr));
    ctx.fillStyle = this.waveColors.note || this.waveColors.body;

    for (let i = 0; i < eng.noteCount; i++) {
      const n = eng.notes[i];
      if (n.track >= vm.laneCount) continue;
      const x0 = Math.floor((n.tOn / tpp - spanX) * dpr);
      if (x0 >= devW) continue;
      // A note's END, not a fixed width: an arrangement where a whole note and a
      // grace note look identical is not showing the music's shape, which is the
      // only thing it is for.
      let x1 = Math.ceil((n.tOff / tpp - spanX) * dpr);
      if (x1 <= 0) continue;
      // A MINIMUM OF ONE DEVICE PIXEL. Zoomed out, most notes are narrower than
      // a pixel, and a rectangle of zero width draws nothing — so the busiest
      // passages would be the emptiest on screen.
      if (x1 - x0 < 1) x1 = x0 + 1;
      const dx0 = Math.max(0, x0), dx1 = Math.min(devW, x1);
      if (dx1 <= dx0) continue;

      const lo = vm.pitchLo[n.track], hi = vm.pitchHi[n.track];
      const span = hi - lo;
      if (span <= 0) continue;
      const usable = laneDev - pad * 2;
      if (usable < 2) continue;
      // High notes at the TOP. Screen y grows downward and pitch does not.
      const frac = 1 - (Math.max(lo, Math.min(hi, n.pitch)) - lo) / span;
      const h = Math.max(1, Math.round(dpr));
      const y = n.track * laneDev + pad + Math.round(frac * (usable - h));
      ctx.fillRect(dx0, y, dx1 - dx0, h);
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
      this.spineIn.style.transform = xf;
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

    /*
     * The spine. Ring-slotted by the marker's index in the SONG, like the ruler above
     * — `vm.markerFirst` is that index for the first visible one.
     *
     * The selected marker gets the class rather than an inline colour, so a theme
     * change repaints it without this file knowing a single colour value. The marker's
     * OWN colour is inline, because it is data the engine published and not a theme
     * decision — and it is written as a custom property so the CSS decides how much of
     * it to use for the fill, the border and the label.
     */
    const spine = this._spine(vm.markerCount);
    const sn = spine.length, sShown = Math.min(vm.markerCount, sn);
    const sBase = sn ? (((vm.markerFirst % sn) + sn) % sn) : 0;
    for (let i = 0; i < sShown; i++) {
      const el = spine[(sBase + i) % sn], sec = vm.markers[i];
      if (el.style.display === 'none') el.style.display = '';
      if (el._x !== sec.x) { el._x = sec.x; el.style.transform = `translateX(${sec.x}px)`; }
      /*
       * THE DRAGGED SPAN KEEPS ITS PREVIEW WIDTH.
       *
       * Without this exclusion the next frame — and there is always a next frame, the
       * playhead alone schedules one — rebinds the width from the model, which is still
       * the OLD length because nothing has been sent yet. So the boundary snapped back
       * under the pointer while the drag was live, the gesture read as broken, and the
       * command it finally sent was correct: the edit worked and the preview lied about
       * it, which is the worse way round.
       *
       * Only the width, and only this one element. Its x is still the model's, because
       * a marker's position is its own and dragging the boundary to its right cannot
       * move that.
       */
      // The element being dragged keeps its preview width; see `_spineMove`.
      const dragging = this._secDrag && this._secDrag.el === el;
      if (!dragging && el._w !== sec.w) { el._w = sec.w; el.style.width = `${sec.w}px`; }
      if (el._name !== sec.name) { el._name = sec.name; el._nm.nodeValue = sec.name; }
      if (el._color !== sec.color) {
        el._color = sec.color;
        // 0 means "the engine gave this one no colour", which is not black — it is
        // "use the theme's". Writing #000000 for it would paint an invisible marker.
        el.style.removeProperty('--sec');
        if (sec.color) {
          el.style.setProperty('--sec',
            `#${sec.color.toString(16).padStart(6, '0')}`);
        }
      }
      // The id lives on the element because the pointer handlers read it from the
      // event target: a boundary drag has to know WHICH boundary it is moving, and
      // recovering that from x would re-derive what the model already bound.
      if (el._id !== sec.id) {
        el._id = sec.id;
        el.dataset.marker = String(sec.id);
      }
      const sel = sec.id !== 0 && sec.id === vm.selectedMarker;
      if (el._sel !== sel) { el._sel = sel; el.classList.toggle('sel', sel); }
    }
    for (let i = sShown; i < sn; i++) {
      const el = spine[(sBase + i) % sn];
      if (el.style.display !== 'none') el.style.display = 'none';
    }
    // The unnamed tail. Guarded field by field like everything else here, so a song
    // whose end has not moved costs three integer compares.
    if (this._tailOn !== vm.tailOn) {
      this._tailOn = vm.tailOn;
      this.spineTail.style.display = vm.tailOn ? '' : 'none';
    }
    if (vm.tailOn) {
      if (this._tailX !== vm.tailX) {
        this._tailX = vm.tailX;
        this.spineTail.style.transform = `translateX(${vm.tailX}px)`;
      }
      if (this._tailW !== vm.tailW) {
        this._tailW = vm.tailW;
        this.spineTail.style.width = `${vm.tailW}px`;
      }
    }
    // TRUNCATION IS DRAWN. A short list that says nothing reads as the end of the song.
    if (this._spineMoreN !== vm.markersTruncated) {
      this._spineMoreN = vm.markersTruncated;
      this.spineMore.firstChild.nodeValue =
        vm.markersTruncated ? `+${vm.markersTruncated} not shown` : '';
      this.spineMore.style.display = vm.markersTruncated ? '' : 'none';
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
      /*
       * SHARED, FORKED, OR THE ONLY ONE — three states, drawn as three.
       *
       * The arrangement could not say any of this: two placements of one clip drew as two rails
       * with the same name, which is indistinguishable from two different clips that happen to
       * share a name. So an edit inside one silently changed the others and nothing on screen
       * said it would — the single worst thing this surface did not report.
       *
       * `.shared` is a hatched left edge and a count; `.forked` is the third state and is drawn
       * DIFFERENTLY rather than as a kind of shared, because "this one has its own copy and
       * another version behind it" is what you need to know before you swap, and folding it into
       * either of the other two is the lie the readout exists to stop.
       */
      const shared = c.appearances > 1;
      if (el._shared !== shared) { el._shared = shared; el.classList.toggle('shared', shared); }
      if (el._forked !== c.hasAlternate) {
        el._forked = c.hasAlternate;
        el.classList.toggle('forked', c.hasAlternate);
      }
      /*
       * The COUNT, in the rail, because "shared" without "with how many" is half an answer — and
       * the number is what tells you whether an edit here changes one other thing or eleven.
       * Its own text node, so a name change does not rebuild it and it does not rebuild a name.
       */
      /*
       * The badge is shown for a FORKED placement too, as `\u21c4` — because that is where the
       * swap lives, and a control that appears for one state and vanishes for the next is a
       * control people stop reaching for. Shared shows the count; forked shows that there is
       * another version to exchange with.
       */
      /*
       * NOT ON A RAIL TOO NARROW TO HOLD IT.
       *
       * The badge is `right: 3px` inside the block, so on a 4px rail its box hangs off the left
       * edge and into whatever is beside it — the gutter — where `.ar-clip`'s own overflow hides
       * it but its hit box is still out there, over another surface's controls. MEASURED: a
       * placement whose length the engine has not published draws at the 2px floor, and the
       * badge's box landed at x=355 inside a gutter that ends at 371.
       *
       * So it is drawn only where it fits. The state is still reachable — the chrome chip says
       * it, and `shared` and `fork` say it at the console — which is the right trade: a control
       * you cannot see is worse than one you reach another way.
       */
      const roomForBadge = c.w >= BADGE_MIN_W_PX;
      const badge = !roomForBadge ? ''
        : c.hasAlternate ? '\u21c4' : shared ? `\u00d7${c.appearances}` : '';
      if (el._badge !== badge) { el._badge = badge; el._share.nodeValue = badge; }
      // A tooltip that says it in words, because a hatch and a number are a convention somebody
      // has to learn once — and the first time is best spent reading rather than guessing.
      const title = c.hasAlternate
        ? `${c.name} — forked: this placement has its own copy, with another version behind it`
        : shared ? `${c.name} — shared by ${c.appearances} placements; editing here changes all of them`
        : '';
      if (el._title !== title) { el._title = title; el.title = title; }
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
      const dragging = drag !== null && drag.live && drag.id === c.id;
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
    // After the waves, so a curve is drawn OVER the material it is shaping rather than under it.
    this._paintAutomation(vm);

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
  /**
   * Draw the automation curves over the clips.
   *
   * ONE POLYLINE PER TRACK, for the lane that track has open. Not every lane at once: two curves
   * in one 44px lane are two lines nobody can attribute, and the point of drawing automation is
   * to see what a parameter DOES.
   *
   * GUARDED ON EVERY INPUT. The signature names the automation version, the window, the lane
   * geometry and which lane each track has open — a picture that is a function of exactly those,
   * so anything that can change it changes the key. GUIDELINES 2.1: content moving while the key
   * stands still is this codebase's signature bug.
   */
  _paintAutomation(vm) {
    const curves = this.autoCurves;
    if (!curves) return;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(this.clipsEl.clientWidth));
    const h = Math.max(1, Math.round(this.clipsEl.clientHeight));
    // The signature includes the CURVE REVISION rather than the curves themselves: comparing
    // point arrays per frame would cost more than the paint it is trying to avoid.
    const d = this._auDrag;
    const sig = `${curves.revision}|${vm.view.startTick}|${vm.view.ticksPerPixel}|${w}x${h}`
              + `|${vm.laneHeight}|${vm.laneCount}|${vm.laneScroll}|${dpr}|${vm.automationEdit}`
              // The GESTURE is part of the picture: the dragged point is drawn where the hand
              // has it, not where the engine last said it was, so the guard must see it move.
              + (d ? `|${d.track}:${d.param}@${d.tick}=${Math.round(d.value * 1000)}` : '|-');
    if (sig === this._auSig) return;
    this._auSig = sig;
    this.autoRepaints++;

    if (this.autoCanvas.width !== w * dpr || this.autoCanvas.height !== h * dpr) {
      this.autoCanvas.width = w * dpr;
      this.autoCanvas.height = h * dpr;
      this.autoCanvas.style.width = `${w}px`;
      this.autoCanvas.style.height = `${h}px`;
    }
    /*
     * THE MODE IS WHAT MAKES THE LAYER CLICKABLE. Toggled here rather than in the handler,
     * because a canvas that swallowed the pointer while the mode was off would make every clip
     * in an automated track undraggable — the failure would look like broken clip dragging and
     * nothing would point at automation.
     */
    this.autoCanvas.classList.toggle('edit', !!vm.automationEdit);

    const ctx = this.autoCtx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    if (!this._auColor) {
      // Read from the THEME once, not hardcoded: the accent is a token and a canvas cannot use
      // a CSS variable. Re-read when the canvas is rebuilt, so a theme change is picked up.
      this._auColor = getComputedStyle(this.host).getPropertyValue('--base-accent').trim()
                      || '#9184d9';
    }
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = this._auColor;

    const tpp = vm.view.ticksPerPixel;
    const startTick = vm.view.startTick;
    for (let i = 0; i < vm.laneCount; i++) {
      const lane = vm.lanes[i];
      const c = curves.forTrack(lane.track);
      if (!c || !c.points || c.points.length === 0) continue;
      // The lane's box in the strip's own space, minus the vertical scroll. Inset so a value of
      // 1 does not sit exactly on the lane border above it and read as belonging to that lane.
      const top = lane.y - (vm.laneScroll || 0) + 3;
      const height = lane.height - 6;
      if (top + height < 0 || top > h) continue;
      /*
       * THE POINT UNDER THE HAND, drawn at the value the hand has it.
       *
       * The engine is authoritative and this does not pretend otherwise: the override lives only
       * while the pointer is down and is dropped on release, when the republished lane takes
       * over. Without it the curve would lag the pointer by a full round trip — and worse,
       * `writeAutomation` drops the cached curve on every write, so the line would vanish and
       * reappear on every frame of a drag.
       */
      const drag = (d && d.track === lane.track && d.param === c.param) ? d : null;
      ctx.beginPath();
      let started = false;
      for (let k = 0; k < c.points.length; k++) {
        const [tick, raw] = c.points[k];
        const value = (drag && tick === drag.tick) ? drag.value : raw;
        const x = (tick - startTick) / tpp;
        const y = top + (1 - Math.max(0, Math.min(1, value))) * height;
        if (!started) { ctx.moveTo(x, y); started = true; continue; }
        /*
         * A DISCRETE clip STEPS and a ramped one does not, and drawing the wrong one is worse
         * than drawing neither: a stepped curve drawn as a ramp says the parameter passes
         * through values it never takes. The engine publishes which, per lane.
         */
        if (c.discrete) {
          const prev = c.points[k - 1];
          // The previous point may be the one being dragged too — read it through the same
          // override, or the step would be drawn from a value no longer on screen.
          const pv = (drag && prev[0] === drag.tick) ? drag.value : prev[1];
          const py = top + (1 - Math.max(0, Math.min(1, pv))) * height;
          ctx.lineTo(x, py);
        }
        ctx.lineTo(x, y);
      }
      // ...and CARRY ON to the right edge. A curve that stops at its last point looks like a
      // parameter that stops being automated there; it holds that value to the end of the song.
      const last = c.points[c.points.length - 1];
      const lastV = (drag && last[0] === drag.tick) ? drag.value : last[1];
      const lastY = top + (1 - Math.max(0, Math.min(1, lastV))) * height;
      ctx.lineTo(w, lastY);
      ctx.stroke();

      /*
       * A HANDLE UNDER THE HAND.
       *
       * Drawn separately from the polyline rather than spliced into it, for the case that made
       * the line alone insufficient: a point being CREATED is not in `c.points` at all until the
       * engine answers, so a drag that started on empty lane would show nothing moving for a
       * whole round trip. A dot at the gesture's own position is immediate and is true for both
       * cases — the grabbed point and the new one.
       */
      if (drag) {
        const dx = (drag.tick - startTick) / tpp;
        const dy = top + (1 - Math.max(0, Math.min(1, drag.value))) * height;
        ctx.beginPath();
        ctx.arc(dx, dy, 3, 0, Math.PI * 2);
        ctx.fillStyle = this._auColor;
        ctx.fill();
      }
    }
  }

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
       * The SPINE as the strip drew it, read back from the DOM and not from the model.
       *
       * From the DOM on purpose: the model having a marker in it proves the decode,
       * and the whole failure this feature can have is a span the engine published and
       * the strip did not draw. `visible` walks the pool and reports what is actually
       * on screen, in the ring's own order resolved back to reading order, so a test
       * can compare it against the engine's list directly.
       */
      spine: {
        count: vm.markerCount,
        truncated: vm.markersTruncated,
        endTick: vm.spineEndTick,
        selected: vm.selectedMarker,
        pool: this.spinePool.length,
        // Only the shown slots, in reading order — the ring's phase is an internal
        // detail and a test comparing against the engine must not have to undo it.
        visible: (() => {
          const out = [];
          const sn = this.spinePool.length;
          if (!sn) return out;
          const base = ((vm.markerFirst % sn) + sn) % sn;
          for (let i = 0; i < Math.min(vm.markerCount, sn); i++) {
            const el = this.spinePool[(base + i) % sn];
            out.push({ id: el._id, name: el._nm.nodeValue,
                       x: Math.round(el._x), w: Math.round(el._w),
                       shown: el.style.display !== 'none' });
          }
          return out;
        })(),
        // What the truncation marker SAYS, so "it is drawn" is assertable rather than
        // inferred from a count that only the model has.
        moreText: this.spineMore.firstChild.nodeValue,
        // The unnamed tail, as the DOM has it. `on` from the element rather than the
        // model, so "the model wants a tail" and "a tail is drawn" stay separable.
        tail: { on: this.spineTail.style.display !== 'none',
                x: Math.round(vm.tailX), w: Math.round(vm.tailW) },
      },
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
      /*
       * COUNTED, NOT SUMMED FROM POOLS.
       *
       * Three pool lengths added up is the count of POOLED things, not of nodes: at 64 tracks it
       * came to 184 while the host held 541. The mixer had the same defect six times worse
       * (`pool.length * 12`, a per-strip constant from when a strip had twelve nodes), and it
       * understates in the direction that makes a scaling problem look solved.
       *
       * `pooled` keeps the old number, which is the one that answers a different and still
       * useful question — whether the pools are growing rather than being reused.
       */
      domNodes: this.host ? this.host.querySelectorAll('*').length : 0,
      pooled: this.clipPool.length + this.lanePool.length + this.gridPool.length,
      /**
       * The waveform layer, in numbers a test can assert on before it goes near a
       * pixel — and the geometry it needs to find one when it does.
       *
       * `repaints` is the number this surface is really about: a pan inside the
       * painted band must not move it. `wanted` vs `held` says how much of what is
       * on screen has data behind it, so "the waveform is missing" and "the
       * waveform is empty" are different answers rather than the same blank lane.
       */
      /** The automation layer, in numbers a test can assert before it looks at a pixel. */
      automation: {
        bound: !!this.autoCurves,
        repaints: this.autoRepaints,
        /** The mode, and what the pointer has done through it. */
        edit: !!vm.automationEdit,
        clickable: this.autoCanvas.classList.contains('edit'),
        writes: this.autoWrites,
        dragging: this._auDrag
          ? { track: this._auDrag.track, param: this._auDrag.param,
              tick: this._auDrag.tick, value: Math.round(this._auDrag.value * 1000) / 1000,
              created: this._auDrag.created }
          : null,
        drawn: (() => {
          if (!this.autoCurves) return 0;
          let n = 0;
          for (let i = 0; i < vm.laneCount; i++) {
            const c = this.autoCurves.forTrack(vm.lanes[i].track);
            if (c && c.points && c.points.length) n++;
          }
          return n;
        })(),
      },
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
