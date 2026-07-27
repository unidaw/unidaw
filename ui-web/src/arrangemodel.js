// The arrange view-model: the same engine data as the tracker, projected onto a
// horizontal time axis instead of a vertical row axis.
//
// This file exists to test a claim the tracker made but could not prove: that
// the boundary is "plain data describing what is on screen" and not a shape that
// happens to suit a tracker. Nothing here re-reads the engine differently — it
// consumes the identical store — and nothing in the tracker had to change.
//
// The one thing it does NOT share is rows. A row is a projection of the timeline
// at a zoom, and arrange has no rows: it has lanes (one per track) and a
// continuous x axis. Ticks are the only coordinate both views agree on, which is
// exactly why GUIDELINES 2 says anything durable is expressed in ticks.

/**
 * The fallback names, built once each and kept.
 *
 * `'T' + String(t + 1).padStart(2, '0')` is three strings per call, and the call
 * happens once per track per frame from four surfaces — arrange's lanes, the
 * mixer's strips, the piano roll's header and the tracker's breadcrumb — for as
 * long as the engine has published no names, which is every fixture run and
 * every frame before the first engine frame lands. There is exactly one possible
 * answer per track index, so the index is the whole key and an entry can never
 * go stale; a name the engine did publish still wins over it, below.
 */
const FALLBACK_NAMES = [];

/**
 * A track's name. The engine publishes these (SHM v13) and falls back to
 * "Track N" itself, so an empty string here means the engine has not spoken yet
 * — not that the track is unnamed.
 */
export function trackName(engine, t) {
  const n = engine && engine.names && engine.names[t];
  if (n) return n;
  return FALLBACK_NAMES[t] || (FALLBACK_NAMES[t] = 'T' + String(t + 1).padStart(2, '0'));
}

export const TICKS_PER_BAR = 3840000;
const TICKS_PER_BEAT = 960000;

/**
 * Horizontal zoom, in nanoticks per pixel. Coarser than the tracker's because
 * arrange is for seeing structure — at the finest level a bar is 512px, at the
 * coarsest it is 16px and eighty bars fit on a screen.
 */
export const ARRANGE_ZOOM = [
  { index: 0, ticksPerPixel: 7500, label: 'bar/512px' },
  { index: 1, ticksPerPixel: 15000, label: 'bar/256px' },
  { index: 2, ticksPerPixel: 30000, label: 'bar/128px' },
  { index: 3, ticksPerPixel: 60000, label: 'bar/64px' },
  { index: 4, ticksPerPixel: 120000, label: 'bar/32px' },
  { index: 5, ticksPerPixel: 240000, label: 'bar/16px' },
];

/**
 * Wheel deltas, in pixels. A wheel does not speak one unit: Chrome sends pixels,
 * Firefox sends lines, and a few devices send pages, so `deltaMode` decides what
 * the number means. Reading it as pixels everywhere makes the identical gesture
 * sixteen times smaller on Firefox — and looks perfectly correct on the machine
 * it was written on, which is why this is a named conversion rather than a
 * multiplier buried in the handler.
 */
const DOM_DELTA_LINE = 1, DOM_DELTA_PAGE = 2;

/** One line, when the device counts in lines. Chrome's own line height. */
export const WHEEL_LINE_PX = 16;

/**
 * A delta at or above this is one discrete notch of a mouse wheel.
 *
 * A mouse notch arrives as a single 100-120px delta; a trackpad pinch arrives as
 * a stream of one- and two-pixel deltas. One zoom step per EVENT makes a pinch
 * cross the whole zoom table before the fingers have moved a centimetre; a fixed
 * pixel accumulator makes a mouse feel stuck. So the two are told apart by size
 * and handled differently, and which device the author owns stops deciding
 * whether the other one is usable.
 */
export const WHEEL_NOTCH_PX = 50;

/** Accumulated pixels per zoom step for fine devices (a trackpad pinch). */
export const WHEEL_PINCH_PX = 24;

/** A wheel delta in pixels, whatever unit the device used to express it. */
export function wheelPixels(delta, deltaMode, pageSize) {
  if (deltaMode === DOM_DELTA_LINE) return delta * WHEEL_LINE_PX;
  if (deltaMode === DOM_DELTA_PAGE) return delta * pageSize;
  return delta;
}

/** A zoom index clamped into the table above. */
export function clampZoom(i) {
  return Math.max(0, Math.min(ARRANGE_ZOOM.length - 1, i));
}

/** Nanoticks per pixel at a zoom index, clamped. */
export function ticksPerPixelAt(zoomIndex) {
  return ARRANGE_ZOOM[clampZoom(zoomIndex)].ticksPerPixel;
}

/**
 * The start tick that leaves `anchorTick` under `pointerX` at `ticksPerPixel`.
 *
 * This is the whole difference between zoom that feels like zoom and zoom that
 * feels like teleporting. The bar under the pointer is the one being looked at;
 * anchoring on the viewport edge instead slides it out from under the cursor,
 * so every step has to be followed by a scroll to find the material again.
 *
 * Clamped at zero, which is the one place the anchor is NOT preserved: zooming
 * out near the start of the song would need negative time to hold it. Losing the
 * anchor there is right — showing time before the beginning is not.
 *
 * Takes a ticks-per-pixel rather than a zoom index on purpose. The arrangement
 * and the piano roll have different zoom tables and the identical axis, so the
 * arithmetic that keeps a tick still belongs to neither table; passing the index
 * would have made this the arrangement's, and the piano roll would have grown a
 * second copy that drifts.
 */
export function anchoredStart(anchorTick, pointerX, ticksPerPixel) {
  return Math.max(0, anchorTick - pointerX * ticksPerPixel);
}

/**
 * Snap a dragged loop span to bars, or to beats when `fine`.
 *
 * Here rather than in the page because it is a musical decision with edge cases
 * worth testing: a backwards drag is the same span rather than an empty one, a
 * click is one unit rather than zero (the engine refuses end <= start), and a
 * drag off the left edge stops at zero.
 */
export function snapLoop(a, b, fine) {
  const unit = fine ? TICKS_PER_BAR / 4 : TICKS_PER_BAR;
  const start = Math.max(0, Math.round(Math.min(a, b) / unit) * unit);
  const end = Math.max(start + unit, Math.round(Math.max(a, b) / unit) * unit);
  return { start, end };
}

/**
 * Reusable buffer. Same discipline as the tracker's: the draw path allocates
 * nothing, so the pools are sized once and mutated in place.
 */
export function createArrangeBuffer(laneCount, clipCapacity = 128) {
  const lanes = new Array(laneCount);
  for (let i = 0; i < laneCount; i++) {
    lanes[i] = { track: i, name: '', lpb: 0, y: 0, height: 0 };
  }
  const clips = new Array(clipCapacity);
  for (let i = 0; i < clipCapacity; i++) {
    clips[i] = { id: 0, clipId: 0, track: 0, x: 0, w: 0, name: '',
                 audio: false, selected: false, startTick: 0, endTick: 0 };
  }
  return {
    lanes, laneCount,
    clips, clipCount: 0,
    view: { startTick: 0, ticksPerPixel: 60000, width: 0 },
    /**
     * How far the time axis is scrolled, in pixels: `startTick / ticksPerPixel`.
     *
     * Every x below — gridlines, ruler ticks, clips — is ABSOLUTE, measured from
     * tick 0 and not from the window's left edge, so that panning changes none of
     * them and the renderer moves the whole axis with one transform instead of
     * rebuilding a position string per element per frame (GUIDELINES 3.3). This
     * is the offset that transform is built from, and the only thing here that
     * moves during a pan. Zoom is the exception and has to be: it changes
     * `ticksPerPixel`, so every absolute x moves and a zoom IS a full rebind.
     */
    scrollX: 0,
    /**
     * Bar and beat lines to draw, as ABSOLUTE x positions, plus the index of the
     * first one. The index is the line's identity — `tick / step` — and the
     * renderer slots the pool by it modulo the pool size (GUIDELINES 3.4), so a
     * pan past one line rebinds one element rather than shuffling every line
     * down by one and rebinding all of them.
     */
    grid: new Float64Array(256), gridCount: 0, gridIsBar: new Uint8Array(256),
    gridFirst: 0,
    /**
     * The loop region in pixels, or null when there is none. Deliberately in
     * VIEWPORT pixels, not absolute: it is clamped to the window so that a loop
     * longer than the screen stays a screen-sized box, and it moves every frame
     * of a drag or a pan by nature. One guarded string is its floor.
     */
    loop: { x: 0, w: 0, on: false },
    /**
     * Numbered bar ticks for the ruler: absolute x plus the bar number to print,
     * and the identity index of the first, which is `bar / every` — see `grid`.
     */
    ruler: new Float64Array(128), rulerBar: new Int32Array(128), rulerCount: 0,
    rulerFirst: 0,
    /** Also viewport pixels, and for the same reason as `loop`. */
    playheadX: -1,
    /**
     * How far the lane strip is scrolled down, in pixels.
     *
     * Kept OUT of `lanes[].y` and `clips[].x/w` on purpose: those stay in
     * content space, so the renderer can move the whole strip with one transform
     * rather than rebinding every lane and clip (GUIDELINES 3.3). It is also the
     * only coordinate here that is legitimately in pixels — a scroll offset is a
     * viewport fact, not a durable one, and nothing stores it past the session.
     */
    laneScroll: 0,
    cursor: { track: 0, tick: 0 },
    /**
     * The selection, split into the two numbers it is made of. See the clip
     * loop in `buildArrangeModel` for why it is not compared as a string.
     */
    _selKey: undefined, _selTrack: -1, _selTick: -1,
    _shape: `${laneCount}x${clipCapacity}`,
  };
}

/**
 * These renderers deliberately have NO content revision.
 *
 * The tracker needs one because it binds ~1,700 cells and cannot afford to touch
 * them all every frame. Arrange, the piano roll and the mixer bind tens of
 * elements and guard every individual write, so a per-frame pass is already
 * cheap (0.1 ms measured) and a revision would buy nothing.
 *
 * They each HAD one, computed and then read by nobody. That is worse than not
 * having one: arrange's omitted the per-lane grids, so the first person to trust
 * it would have found lane labels going stale on a project load — this codebase's
 * signature bug, lying in wait behind something that looked like it was handled.
 * If one of these ever needs a revision, write it then, against what the model
 * actually reads at that point.
 */

/**
 * The cursor a caller that has none gets. A `{track, tick}` literal in the
 * default position below would be built on every call that omits it, and this
 * function runs once a frame; the two fields are only ever read from here.
 */
const NO_CURSOR = Object.freeze({ track: 0, tick: 0 });

/**
 * Build the arrange model for a visible time window.
 *
 * `loop` is the caller's in-flight loop span, which wins over the engine's.
 *
 * @param {{startTick:number, width:number, zoomIndex:number, tracks:number,
 *          engine:object|null, laneHeight:number, cursor:object,
 *          selectedPlacement:number}} opts
 */
export function buildArrangeModel(opts, buf) {
  const {
    startTick = 0, width = 1200, zoomIndex = 3, tracks: laneCount = 8, loop = null,
    engine = null, laneHeight = 44, cursor = NO_CURSOR,
    selectedPlacement = -1, laneScroll = 0,
  } = opts;

  const zoom = ARRANGE_ZOOM[Math.max(0, Math.min(ARRANGE_ZOOM.length - 1, zoomIndex))];
  const tpp = zoom.ticksPerPixel;
  const endTick = startTick + width * tpp;

  buf.view.startTick = startTick;
  buf.view.ticksPerPixel = tpp;
  buf.view.width = width;
  buf.scrollX = startTick / tpp;
  buf.zoom = zoom;
  // Only the floor is enforced here. The ceiling is how much lane strip does not
  // fit, which is a measured box and not something this file may re-derive
  // (GUIDELINES 3.11) — the renderer clamps it against the real one.
  buf.laneScroll = Math.max(0, laneScroll);

  for (let t = 0; t < laneCount && t < buf.lanes.length; t++) {
    const lane = buf.lanes[t];
    lane.track = t;
    lane.y = t * laneHeight;
    lane.height = laneHeight;
    lane.name = trackName(engine, t);
    lane.lpb = engine && engine.lpb[t] ? engine.lpb[t] : 0;
  }
  buf.laneCount = Math.min(laneCount, buf.lanes.length);

  // Bar and beat lines. Only the ones in view, so this is O(visible) rather than
  // O(timeline) — the timeline is unbounded and there is no zoom at which
  // enumerating all of it becomes acceptable.
  let g = 0;
  const beatPx = TICKS_PER_BEAT / tpp;
  // Below ~6px a beat line is noise, so drop to bars only. Deciding this from
  // the projection rather than from the zoom index means it stays right if the
  // zoom table changes.
  const step = beatPx >= 6 ? TICKS_PER_BEAT : TICKS_PER_BAR;
  const firstLine = Math.floor(startTick / step);
  const first = firstLine * step;
  // ABSOLUTE x: `tick / tpp`, with no `startTick` in it. See `scrollX`.
  for (let tick = first; tick < endTick && g < buf.grid.length; tick += step) {
    buf.grid[g] = tick / tpp;
    buf.gridIsBar[g] = tick % TICKS_PER_BAR === 0 ? 1 : 0;
    g++;
  }
  buf.gridCount = g;
  buf.gridFirst = firstLine;

  // Ruler: bar numbers only, and thinned so the labels never collide. Deciding
  // the stride from the measured pixel width rather than the zoom index means it
  // stays correct if the zoom table changes.
  const barPx = TICKS_PER_BAR / tpp;
  const every = barPx >= 48 ? 1 : barPx >= 24 ? 2 : barPx >= 12 ? 4 : 8;
  let r = 0;
  const firstBar = Math.floor(startTick / TICKS_PER_BAR);
  // The identity of a LABEL is `bar / every`, not the bar: at every > 1 the bars
  // in between have no label at all, so numbering the pool by the bar would
  // leave `every - 1` slots of every `every` permanently unclaimed and shift the
  // rest on every pan. Set from the first label actually emitted, because
  // `firstBar` itself usually is not one.
  buf.rulerFirst = Math.ceil(firstBar / every);
  for (let bar = firstBar; r < buf.ruler.length; bar++) {
    const tick = bar * TICKS_PER_BAR;
    if (tick >= endTick) break;
    if (bar % every !== 0) continue;
    buf.ruler[r] = tick / tpp;                 // absolute; see `scrollX`
    buf.rulerBar[r] = bar + 1;                 // bars are 1-based to the user
    r++;
  }
  buf.rulerCount = r;

  // The selection arrives as the string key "track:startTick" (see
  // `placementKey`), and asking "is this clip the selected one?" used to build
  // that string for every visible clip on every frame — a string per clip per
  // frame to compare two numbers that were already in hand. Split it once
  // instead, keyed on the string itself, which is the entire input: nothing can
  // change the answer without changing the key, and the split only re-runs when
  // the user selects something.
  if (buf._selKey !== selectedPlacement) {
    buf._selKey = selectedPlacement;
    const colon = typeof selectedPlacement === 'string' ? selectedPlacement.indexOf(':') : -1;
    // -1 for "nothing selected": no extent has a negative track or start tick,
    // so a cleared selection cannot accidentally match a clip.
    buf._selTrack = colon > 0 ? +selectedPlacement.slice(0, colon) : -1;
    buf._selTick = colon > 0 ? +selectedPlacement.slice(colon + 1) : -1;
  }

  // Clips overlapping the window. Placements are already bounded by the engine
  // (64 extents), but windowing here keeps the renderer's work proportional to
  // what is visible rather than to what exists.
  let c = 0;
  if (engine) {
    for (let i = 0; i < engine.extentCount && c < buf.clips.length; i++) {
      const e = engine.extents[i];
      if (e.track >= laneCount) continue;
      if (e.endTick <= startTick || e.startTick >= endTick) continue;
      const cl = buf.clips[c++];
      cl.id = e.placementId;
      cl.clipId = e.clipId;
      cl.track = e.track;
      cl.startTick = e.startTick;
      cl.endTick = e.endTick;
      // Absolute, like the grid: a clip that nobody moved does not move, so a
      // pan writes nothing for it. Its WIDTH was already startTick-independent.
      cl.x = e.startTick / tpp;
      // A placement narrower than a pixel still has to be visible: a clip you
      // cannot see is a clip you cannot click, and zooming out must not make
      // material silently disappear.
      cl.w = Math.max(2, (e.endTick - e.startTick) / tpp);
      cl.name = e.name;
      cl.audio = !!e.audio;
      // Keyed on (track, startTick), not placement_id. Backend's note: the id is
    // currently the extent's INDEX, so it shifts whenever the list changes — a
    // selection keyed on it would silently jump to a different clip after an
    // edit. Position is stable and disappears when the clip does, which is what
    // a selection should do. Switch to the id when it becomes stable.
    cl.selected = e.track === buf._selTrack && e.startTick === buf._selTick;
    }
  }
  buf.clipCount = c;

  // The loop, if the engine has one. Clamped to the window rather than skipped
  // when it runs off the edge: a loop you are inside should still show its edge.
  //
  // `loop` overrides the engine while a drag is in flight or a set command has
  // not come back yet — the bracket has to follow the pointer, and the engine is
  // a round trip away.
  const ls = loop ? loop.start : (engine ? engine.loopStart : 0);
  const le = loop ? loop.end : (engine ? engine.loopEnd : 0);
  buf.loop.on = le > ls && le > startTick && ls < endTick;
  if (buf.loop.on) {
    const x0 = Math.max(0, (ls - startTick) / tpp);
    const x1 = Math.min(width, (le - startTick) / tpp);
    buf.loop.x = x0;
    buf.loop.w = Math.max(1, x1 - x0);
  }

  buf.playheadX = engine && engine.playheadTick >= startTick && engine.playheadTick < endTick
    ? (engine.playheadTick - startTick) / tpp
    : -1;

  buf.cursor.track = cursor.track;
  buf.cursor.tick = cursor.tick;

  return buf;
}

/**
 * The stable identity of a placement for selection purposes. See above.
 *
 * This is the definition of the format; the draw path deliberately does not
 * call it, because building the key per clip per frame is a string per clip per
 * frame. It compares the two numbers the key is made of instead.
 */
export function placementKey(e) { return e.track + ':' + e.startTick; }

/** x -> tick, for hit testing and click-to-position. */
export function tickAtX(view, x) {
  return view.startTick + x * view.ticksPerPixel;
}
