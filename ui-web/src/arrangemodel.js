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
 * A track's name. The engine publishes these (SHM v13) and falls back to
 * "Track N" itself, so an empty string here means the engine has not spoken yet
 * — not that the track is unnamed.
 */
export function trackName(engine, t) {
  const n = engine && engine.names && engine.names[t];
  return n || 'T' + String(t + 1).padStart(2, '0');
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
    /** Bar lines to draw, as x positions. Pooled like everything else. */
    grid: new Float64Array(256), gridCount: 0, gridIsBar: new Uint8Array(256),
    /** The loop region in pixels, or null when there is none. */
    loop: { x: 0, w: 0, on: false },
    /** Numbered bar ticks for the ruler: x plus the bar number to print. */
    ruler: new Float64Array(128), rulerBar: new Int32Array(128), rulerCount: 0,
    playheadX: -1,
    cursor: { track: 0, tick: 0 },
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
    engine = null, laneHeight = 44, cursor = { track: 0, tick: 0 },
    selectedPlacement = -1,
  } = opts;

  const zoom = ARRANGE_ZOOM[Math.max(0, Math.min(ARRANGE_ZOOM.length - 1, zoomIndex))];
  const tpp = zoom.ticksPerPixel;
  const endTick = startTick + width * tpp;

  buf.view.startTick = startTick;
  buf.view.ticksPerPixel = tpp;
  buf.view.width = width;
  buf.zoom = zoom;

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
  const first = Math.floor(startTick / step) * step;
  for (let tick = first; tick < endTick && g < buf.grid.length; tick += step) {
    buf.grid[g] = (tick - startTick) / tpp;
    buf.gridIsBar[g] = tick % TICKS_PER_BAR === 0 ? 1 : 0;
    g++;
  }
  buf.gridCount = g;

  // Ruler: bar numbers only, and thinned so the labels never collide. Deciding
  // the stride from the measured pixel width rather than the zoom index means it
  // stays correct if the zoom table changes.
  const barPx = TICKS_PER_BAR / tpp;
  const every = barPx >= 48 ? 1 : barPx >= 24 ? 2 : barPx >= 12 ? 4 : 8;
  let r = 0;
  const firstBar = Math.floor(startTick / TICKS_PER_BAR);
  for (let bar = firstBar; r < buf.ruler.length; bar++) {
    const tick = bar * TICKS_PER_BAR;
    if (tick >= endTick) break;
    if (bar % every !== 0) continue;
    buf.ruler[r] = (tick - startTick) / tpp;
    buf.rulerBar[r] = bar + 1;                 // bars are 1-based to the user
    r++;
  }
  buf.rulerCount = r;

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
      cl.x = (e.startTick - startTick) / tpp;
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
    cl.selected = placementKey(e) === selectedPlacement;
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

/** The stable identity of a placement for selection purposes. See above. */
export function placementKey(e) { return e.track + ':' + e.startTick; }

/** x -> tick, for hit testing and click-to-position. */
export function tickAtX(view, x) {
  return view.startTick + x * view.ticksPerPixel;
}
