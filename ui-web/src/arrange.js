// Arrange renderer: lanes of clip blocks on a horizontal time axis.
//
// Same rules as the tracker renderer, for the same reasons (GUIDELINES 3):
// pooled elements that are hidden rather than removed, every style write guarded
// by a cached number, and no string building unless the string actually changed.
// A DAW's arrange page is where a user drags things around, so it is the one
// surface where a dropped frame is felt directly.

import { anchoredStart, clampZoom, ticksPerPixelAt, wheelPixels,
         WHEEL_NOTCH_PX, WHEEL_PINCH_PX } from './arrangemodel.js';

const GRID_POOL = 256;

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
  constructor(host, metrics, { onLoop, onNav } = {}) {
    this.host = host;
    this.metrics = metrics;
    this.host.className = 'ar';
    // Raw ticks, unsnapped: where a loop is ALLOWED to start is a musical
    // question, and this file knows about pixels. The caller snaps.
    this.onLoop = onLoop;
    this.onNav = onNav;

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
    this.playhead = div('ar-playhead', this.band);

    this.lanePool = [];
    this.headPool = [];
    this.clipPool = [];
    this.gridPool = [];
    this.rulerPool = [];
    this.laneCount = 0;
    this.vm = null;

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
      el._pTrack = -1; el._pTick = -1;
      this.clipPool.push(el);
    }
    return this.clipPool;
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

    const clips = this._clip(vm.clipCount);
    for (let i = 0; i < clips.length; i++) {
      const el = clips[i];
      if (i >= vm.clipCount) {
        if (el.style.display !== 'none') el.style.display = 'none';
        continue;
      }
      if (el.style.display === 'none') el.style.display = '';
      const c = vm.clips[i];
      const y = c.track * lh;
      if (el._x !== c.x || el._y !== y) {
        el._x = c.x; el._y = y;
        el.style.transform = `translate(${c.x}px, ${y}px)`;
      }
      if (el._w !== c.w) { el._w = c.w; el.style.width = `${c.w}px`; }
      if (el._h !== lh) { el._h = lh; el.style.height = `${lh}px`; }
      if (el._name !== c.name) { el._name = c.name; el._label.nodeValue = c.name; }
      if (el._audio !== c.audio) { el._audio = c.audio; el.classList.toggle('audio', c.audio); }
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
      playheadX: Math.round(vm.playheadX),
      domNodes: this.clipPool.length + this.lanePool.length + this.gridPool.length,
    };
  }
}
