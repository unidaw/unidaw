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

import { DEFAULT_METER, NANOTICKS_PER_QUARTER, ticksPerBar, ticksPerBeat } from './meter.js';

/**
 * 4/4's bar and beat, kept as the DEFAULT for callers that have no meter to hand
 * and as the export the tests import.
 *
 * These used to be the only answer. The engine publishes a song time signature now
 * (kShmVersion 19), so anything that draws bar lines or numbers them takes the
 * meter as an argument and these are what it falls back to — a project in 7/8 drew
 * its bar lines every four quarters, which is not a rounding error but a different
 * piece of music.
 */
export const TICKS_PER_BAR = ticksPerBar(DEFAULT_METER);

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
 * Nanoticks per SOURCE frame, for a file at `rateHz` positioned at `bpmMilli`.
 *
 *   ticksPerSourceFrame = nanoticksPerQuarter * bpm / (60 * rateHz)
 *
 * Two things this must not do, both of which draw a waveform that looks right and
 * sits in the wrong place:
 *
 *  - It must not assume 120 BPM. The engine publishes the tempo the audio was
 *    POSITIONED at (`audioMapBpmMilli`), and mirroring that number is the whole of
 *    what makes the drawn waveform line up with the audio you hear. A hardcoded
 *    120 is correct on every fixture and wrong on every real project.
 *  - It must not assume the file's rate equals the engine's. `rate` in the source
 *    table is the FILE's, and a 48 kHz file in a 44.1 kHz session is a 9% error —
 *    small enough to read as "close enough" and large enough to be a bar out by
 *    the end of a song.
 *
 * `bpmMilli` is BPM x 1000, which is what the engine publishes; folding the 1000
 * in with the 60 leaves one multiply and one divide, and no chance of a stray
 * factor of a thousand in a per-clip path.
 */
export function ticksPerSourceFrame(bpmMilli, rateHz) {
  if (!(rateHz > 0) || !(bpmMilli > 0)) return 0;
  return (NANOTICKS_PER_QUARTER * bpmMilli) / (60 * 1000 * rateHz);
}

/**
 * Snap a dragged loop span to bars, or to beats when `fine`.
 *
 * Here rather than in the page because it is a musical decision with edge cases
 * worth testing: a backwards drag is the same span rather than an empty one, a
 * click is one unit rather than zero (the engine refuses end <= start), and a
 * drag off the left edge stops at zero.
 */
export function snapLoop(a, b, fine, meter = DEFAULT_METER) {
  // `fine` snaps to a BEAT. It used to compute a quarter of a bar, which is the
  // same thing in 4/4 and in nothing else — in 7/8 a quarter of a bar is 1.75
  // eighths, a position no note in the project can be on. The constant was hiding
  // the difference between "a beat" and "a quarter of a bar" by making them equal.
  const unit = fine ? ticksPerBeat(meter) : ticksPerBar(meter);
  const start = Math.max(0, Math.round(Math.min(a, b) / unit) * unit);
  const end = Math.max(start + unit, Math.round(Math.max(a, b) / unit) * unit);
  return { start, end };
}

/**
 * Where a clip drag lands.
 *
 * Here rather than in the view for the same reason `snapLoop` is: it is a pile
 * of musical edge cases, every one of which is wrong on the first attempt and
 * invisible in a screenshot. Dragging a clip off the left edge, trimming an edge
 * past the other one, moving a clip onto a lane that is not there — each has a
 * right answer, and none of them is the arithmetic.
 *
 * `mode` is 'move', 'trim-l' or 'trim-r'.
 *
 * Returns the placement's new geometry plus `changed`, which the caller uses to
 * decide whether to send anything at all. A gesture that ends where it started
 * is the commonest gesture there is — a click — and it must not become a command
 * that dirties the project and costs an undo step.
 */
export function dragPlacement(clip, mode, deltaTicks, deltaLanes, opts = {}) {
  const meter = opts.meter || DEFAULT_METER;
  const laneCount = opts.laneCount === undefined ? 1 : opts.laneCount;
  const unit = opts.fine ? ticksPerBeat(meter) : ticksPerBar(meter);
  const snap = (t) => Math.round(t / unit) * unit;
  const len = clip.endTick - clip.startTick;
  let { startTick, endTick, track } = clip;

  if (mode === 'move') {
    // LENGTH IS PRESERVED, including at the wall. Clamping the start to zero and
    // leaving the end where the pointer put it would silently shorten a clip
    // dragged off the left edge — a move that resizes, which is not a thing the
    // user asked for and not a thing they would notice until playback.
    startTick = Math.max(0, snap(clip.startTick + deltaTicks));
    endTick = startTick + len;
    track = Math.min(laneCount - 1, Math.max(0, clip.track + (deltaLanes | 0)));
  } else if (mode === 'trim-l') {
    // A clip may not be trimmed to nothing, and it may not be trimmed THROUGH
    // its other edge — dragging the left handle past the right one would produce
    // a negative length, which reaches the engine as an enormous unsigned one.
    startTick = Math.max(0, Math.min(endTick - unit, snap(clip.startTick + deltaTicks)));
  } else if (mode === 'trim-r') {
    endTick = Math.max(startTick + unit, snap(clip.endTick + deltaTicks));
  }

  return { startTick, endTick, track,
           changed: startTick !== clip.startTick || endTick !== clip.endTick
                    || track !== clip.track };
}

/**
 * How wide a trim handle is, in CSS pixels.
 *
 * Shared with `arrange.css`, which cannot import it — so a test reads the
 * stylesheet and asserts the two agree. A handle whose cursor appears somewhere
 * other than where the drag begins is the worst version of this: it looks
 * correct and does the wrong thing on the pixels either side of the seam.
 */
export const CLIP_HANDLE_PX = 6;

/**
 * The narrowest pitch range a lane's notes are drawn across, in semitones.
 *
 * An octave. A part that only ever plays two adjacent semitones would otherwise
 * be normalised to the full lane height, so a semitone would look like the
 * biggest leap in the song.
 */
export const MIN_PITCH_SPAN = 12;

/**
 * Which part of a clip the pointer is on: its body, or one of its trim handles.
 *
 * The handle is a fixed number of PIXELS, not a fraction of the width, because
 * it is a target for a finger or a mouse and neither gets more precise when the
 * clip is longer. The fraction version made a two-bar clip almost entirely
 * handle and a thirty-two-bar clip almost entirely body.
 *
 * A clip too narrow to hold two handles and a body is all body. Given the model
 * draws anything down to 2px wide, the alternative is a clip that can be trimmed
 * to nothing and never moved.
 */
export function clipZoneAt(localX, width, handlePx = CLIP_HANDLE_PX) {
  if (width < handlePx * 3) return 'move';
  if (localX < handlePx) return 'trim-l';
  if (localX > width - handlePx) return 'trim-r';
  return 'move';
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
                 audio: false, selected: false, startTick: 0, endTick: 0,
                 /**
                  * What the clip reads, when it reads audio. Zero srcId means
                  * "nothing to draw yet" and covers three different states —
                  * a clip that is not audio, an audio clip whose source table
                  * has not arrived, and an audio clip whose clip record the
                  * engine has not published. None of them is an error and all
                  * three draw the same thing, which is nothing.
                  *
                  * `srcStatus` is the source table's: 1 ready, 2 failed to
                  * decode. 2 is carried through rather than filtered out here,
                  * because a file that would not decode has to be VISIBLE as
                  * one — silently blank is the same picture as silence.
                  */
                 /**
                  * Which pooled element draws this clip: the extent's own index,
                  * not this record's. See `clipSlots` below.
                  */
                 slot: 0,
                 srcId: 0, srcStatus: 0, srcFrames: 0,
                 /** The in-point, in SOURCE frames. */
                 startFrame: 0,
                 /** Nanoticks per source frame; 0 when unknown. */
                 ticksPerFrame: 0 };
  }
  return {
    // Per-lane pitch range for the notes drawn inside clips, and the note
    // revision they were computed for. Sized here so the first frame does not
    // allocate in the middle of a draw.
    pitchLo: new Uint8Array(laneCount + 8),
    pitchHi: new Uint8Array(laneCount + 8),
    _pitchRev: -1,
    lanes, laneCount,
    clips, clipCount: 0,
    /**
     * How many pooled clip elements the renderer needs — the engine's extent
     * count, not the visible count.
     *
     * A clip's element is chosen by its EXTENT INDEX, the way the ruler's labels
     * and the gridlines are chosen by theirs (GUIDELINES 3.4). Indexed by position
     * in the visible list instead, one region scrolling off the left shifts every
     * later region down a slot, so every element takes a new x, a new width and a
     * new placement key — a full rebind of the whole screen for a pan of one
     * pixel. That is the failure the tracker's row pool was built to avoid and the
     * clip pool had it: measured, a pan across a song of regions spent ~600 bytes
     * a frame rewriting positions that had not changed.
     *
     * Bounded by the engine's kUiMaxClipExtents — 256 as of kShmVersion 27, and 64
     * before it. Written as "the engine's cap" rather than as the number, because
     * the number moved and this comment did not: it said 64 for a while after the
     * engine had quadrupled it. A stale bound in a comment is how a hardcoded one
     * gets written next to it.
     *
     * A slot with nothing in it this frame is hidden rather than removed
     * (GUIDELINES 3.7).
     */
    clipSlots: 0,
    /**
     * Bumped by the page whenever a waveform window lands or the source table is
     * replaced. The renderer's canvas is a cache of what it last painted, and
     * this is the one input to that painting which is not on screen — without it
     * the picture would stand still while its content changed (GUIDELINES 2.1).
     */
    waveRevision: 0,
    /** Memo for `ticksPerSourceFrame` — the tempo and rate it was computed at. */
    _tpfBpm: -1, _tpfRate: -1, _tpf: 0,
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
 * `audio` is the page's audio side — `{sources, clips, bpmMilli, revision}` as
 * `__uni.audioSources()` publishes it, plus the revision described on the buffer.
 * Null when the project has no audio or the table has not arrived.
 *
 * @param {{startTick:number, width:number, zoomIndex:number, tracks:number,
 *          engine:object|null, laneHeight:number, cursor:object,
 *          selectedPlacement:number, audio:object|null}} opts
 */
export function buildArrangeModel(opts, buf) {
  const {
    startTick = 0, width = 1200, zoomIndex = 3, tracks: laneCount = 8, loop = null,
    engine = null, laneHeight = 44, cursor = NO_CURSOR,
    selectedPlacement = -1, laneScroll = 0, audio = null, clipMarginPx = 0,
    // The SONG's meter — what the ruler numbers and where the bar lines go. A clip
    // in another meter draws its own accents inside those bars; that grid rides on
    // the clip and is not this.
    meter = DEFAULT_METER,
  } = opts;
  const barTicks = ticksPerBar(meter);
  const beatTicks = ticksPerBeat(meter);

  const zoom = ARRANGE_ZOOM[Math.max(0, Math.min(ARRANGE_ZOOM.length - 1, zoomIndex))];
  const tpp = zoom.ticksPerPixel;
  const endTick = startTick + width * tpp;

  buf.view.startTick = startTick;
  buf.view.ticksPerPixel = tpp;
  buf.view.width = width;
  buf.scrollX = startTick / tpp;
  buf.zoom = zoom;
  // The meter the VIEW snaps in. Exposed rather than re-derived because a drag
  // must land on the same bar lines the ruler drew — a 7/8 project snapping to
  // 4/4 bars puts clips on positions no note in it can be on, which is the exact
  // mistake `snapLoop` documents one screen up.
  buf.meter = meter;
  buf.laneHeight = laneHeight;
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
  const beatPx = beatTicks / tpp;
  // Below ~6px a beat line is noise, so drop to bars only. Deciding this from
  // the projection rather than from the zoom index means it stays right if the
  // zoom table changes.
  const step = beatPx >= 6 ? beatTicks : barTicks;
  const firstLine = Math.floor(startTick / step);
  const first = firstLine * step;
  // ABSOLUTE x: `tick / tpp`, with no `startTick` in it. See `scrollX`.
  for (let tick = first; tick < endTick && g < buf.grid.length; tick += step) {
    buf.grid[g] = tick / tpp;
    buf.gridIsBar[g] = tick % barTicks === 0 ? 1 : 0;
    g++;
  }
  buf.gridCount = g;
  buf.gridFirst = firstLine;

  // Ruler: bar numbers only, and thinned so the labels never collide. Deciding
  // the stride from the measured pixel width rather than the zoom index means it
  // stays correct if the zoom table changes.
  const barPx = barTicks / tpp;
  const every = barPx >= 48 ? 1 : barPx >= 24 ? 2 : barPx >= 12 ? 4 : 8;
  let r = 0;
  const firstBar = Math.floor(startTick / barTicks);
  // The identity of a LABEL is `bar / every`, not the bar: at every > 1 the bars
  // in between have no label at all, so numbering the pool by the bar would
  // leave `every - 1` slots of every `every` permanently unclaimed and shift the
  // rest on every pan. Set from the first label actually emitted, because
  // `firstBar` itself usually is not one.
  buf.rulerFirst = Math.ceil(firstBar / every);
  for (let bar = firstBar; r < buf.ruler.length; bar++) {
    const tick = bar * barTicks;
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

  // Clips overlapping the window. Placements are already bounded by the engine's
  // kUiMaxClipExtents (256 as of kShmVersion 27), but windowing here keeps the
  // renderer's work proportional to what is VISIBLE rather than to what exists —
  // which is the property that matters as that cap keeps rising.
  //
  // `clipMarginPx` widens THIS window and nothing else — not the grid, not the
  // ruler, not the loop. The renderer paints its waveform band wider than the
  // screen so a pan costs no repaint (ARRANGE_CLIP_MARGIN_PX), and a band it
  // cannot see the clips of would draw holes at its edges that fill in a frame
  // later. The extra blocks are placed at their absolute x like every other one
  // and the arrangement's own overflow clips them.
  const clipLo = startTick - clipMarginPx * tpp;
  const clipHi = endTick + clipMarginPx * tpp;
  let c = 0;
  if (engine) {
    for (let i = 0; i < engine.extentCount && c < buf.clips.length; i++) {
      const e = engine.extents[i];
      if (e.track >= laneCount) continue;
      if (e.endTick <= clipLo || e.startTick >= clipHi) continue;
      const cl = buf.clips[c++];
      cl.slot = i;
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
      // What this clip reads, if anything. Two lookups on plain objects keyed by
      // id — no allocation, and both misses are ordinary: the source table
      // arrives on a project load and the extents arrive at frame rate, so for a
      // frame or two after a load there are audio extents with no audio clip
      // record and that is not a fault.
      cl.srcId = 0; cl.srcStatus = 0; cl.srcFrames = 0; cl.startFrame = 0;
      // Guarded for the same reason the write below is: once this field has held
      // a fraction it is a double field, and storing 0 into one is still a heap
      // number.
      if (cl.ticksPerFrame !== 0) cl.ticksPerFrame = 0;
      if (cl.audio && audio) {
        const ac = audio.clips ? audio.clips[e.clipId] : null;
        const src = ac && audio.sources ? audio.sources[ac.sourceId] : null;
        if (src) {
          cl.srcId = src.id;
          cl.srcStatus = src.status;
          cl.srcFrames = src.frames;
          cl.startFrame = ac.startFrame;
          // Memoised on its two inputs, and guarded on the way out. This is the
          // one field here that is not an integer, and V8 has not had unboxed
          // double fields since 7.6: both the CALL, which returns a fraction, and
          // the STORE, which puts one in an object, allocate a heap number.
          // Unguarded it cost ~80 bytes a frame to recompute a number that changes
          // when the tempo or the file does, which is to say almost never.
          if (buf._tpfBpm !== audio.bpmMilli || buf._tpfRate !== src.rate) {
            buf._tpfBpm = audio.bpmMilli; buf._tpfRate = src.rate;
            buf._tpf = ticksPerSourceFrame(audio.bpmMilli, src.rate);
          }
          if (cl.ticksPerFrame !== buf._tpf) cl.ticksPerFrame = buf._tpf;
        }
      }
      // Keyed on (track, startTick), not placement_id. Backend's note: the id is
    // currently the extent's INDEX, so it shifts whenever the list changes — a
    // selection keyed on it would silently jump to a different clip after an
    // edit. Position is stable and disappears when the clip does, which is what
    // a selection should do. Switch to the id when it becomes stable.
    cl.selected = e.track === buf._selTrack && e.startTick === buf._selTick;
    }
  }
  /*
   * PITCH RANGE PER LANE, for drawing notes inside clips.
   *
   * Per LANE rather than per clip: two clips on the same track are the same
   * part, and normalising each to its own range would draw a bass figure and a
   * one-note stab at the same height and make them look alike. Per track, a low
   * part sits low.
   *
   * Guarded on the note revision because it is a scan over every published note
   * — twenty-odd thousand on the stress fixtures — and the answer only changes
   * when the notes do. Unguarded this would be the most expensive thing in the
   * frame, to produce the same two numbers sixty times a second.
   */
  if (buf.pitchLo.length < laneCount) {
    buf.pitchLo = new Uint8Array(laneCount + 8);
    buf.pitchHi = new Uint8Array(laneCount + 8);
    buf._pitchRev = -1;
  }
  const noteRev = engine ? engine.notesRevision : -1;
  if (buf._pitchRev !== noteRev) {
    buf._pitchRev = noteRev;
    buf.pitchLo.fill(255);
    buf.pitchHi.fill(0);
    if (engine) {
      for (let i = 0; i < engine.noteCount; i++) {
        const n = engine.notes[i];
        if (n.track >= laneCount) continue;
        if (n.pitch < buf.pitchLo[n.track]) buf.pitchLo[n.track] = n.pitch;
        if (n.pitch > buf.pitchHi[n.track]) buf.pitchHi[n.track] = n.pitch;
      }
    }
    // A lane with no notes, or one note, gets a MINIMUM span. Without it a
    // single-pitch part divides by zero, and a two-semitone part draws its two
    // notes at the very top and very bottom of the lane — which reads as a
    // dramatic leap rather than a semitone.
    for (let t = 0; t < laneCount; t++) {
      if (buf.pitchLo[t] > buf.pitchHi[t]) { buf.pitchLo[t] = 60; buf.pitchHi[t] = 72; }
      else if (buf.pitchHi[t] - buf.pitchLo[t] < MIN_PITCH_SPAN) {
        const mid = (buf.pitchLo[t] + buf.pitchHi[t]) >> 1;
        buf.pitchLo[t] = Math.max(0, mid - (MIN_PITCH_SPAN >> 1));
        buf.pitchHi[t] = Math.min(127, buf.pitchLo[t] + MIN_PITCH_SPAN);
      }
    }
  }

  buf.clipCount = c;
  buf.clipSlots = engine ? engine.extentCount : 0;
  buf.waveRevision = audio ? audio.revision : 0;
  // What the notes drawn inside clips were painted against. See `_notes`.
  buf.noteRevision = engine ? engine.notesRevision : 0;

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
