// The tracker minimap's view-model: the whole song folded into one narrow column.
//
// Every other model here describes only the visible window, because the timeline
// is unbounded and a "whole document" representation is exactly what GUIDELINES 2
// rules out. The minimap is the deliberate exception, and it is affordable only
// because its RESOLUTION is fixed rather than its length: the song is folded into
// at most `maxMarks` buckets however long it is, so summarising a 512-bar project
// costs the same per draw as a 16-bar one.
//
// That bound is not a nicety. presets/projects/stress-512.uniproj.json is 512
// bars — 2,048 beats, 80,896 notes — and the design's one-mark-per-beat rule
// would put 2,048 marks in a 714px column: eight marks fighting over every three
// pixels, and 2,048 of whatever the renderer makes a mark out of. Up to
// `maxMarks` beats a mark IS a beat, exactly as the design draws it; past that a
// mark is a whole power-of-two number of beats.
//
// Powers of two, and not `ceil(beats / maxMarks)`, so the mark grid is stable
// while a song grows: recording one more bar re-buckets everything if the
// divisor moves every bar, and a minimap whose marks slide under you as you
// record is worse than a coarser one that holds still.
//
// No DOM here, by the same boundary rule as viewmodel.js — this is plain data,
// the renderer consumes it and nothing else, and a test reads the same shape.

const TICKS_PER_BEAT = 960000;
const TICKS_PER_BAR = TICKS_PER_BEAT * 4;

/** Buckets the model will ever produce. See the header for why it is bounded. */
export const MAX_MARKS = 256;

/**
 * The shortest song the map will draw. A two-bar project mapped to its own
 * length gives a viewport rectangle taller than the column and a playhead that
 * crosses it in seconds — technically true and useless.
 */
const MIN_SONG_TICKS = TICKS_PER_BAR * 16;

/**
 * The design's density scale: `min(0.6, events / 34)` per beat, measured off
 * design/redesign/Uni.dc.html (the marks there run 0.267–0.443 alpha over its
 * eight-bar fixture).
 *
 * Note what that formula actually saturates at. The divisor is 34 but the cap is
 * 0.6, so every beat carrying 0.6 x 34 = 20.4 events or more draws the SAME
 * mark. presets/projects/stress-512.uniproj.json averages 39 notes a beat across
 * its tracks — comfortably past that — so on a real dense project the design's
 * scale flattens the top half of the range into one value and the column becomes
 * a wash. That is the "uniform noise" failure GUIDELINES 7 already records for
 * the aggregate cells, arriving in a second place.
 *
 * So the reference is a floor on the SCALE rather than a fixed divisor:
 * `ref = max(34, peak / 0.6)`. Below the saturation point that is arithmetically
 * identical to the design — ref stays 34 and nothing reaches the cap — and above
 * it the busiest beat in the song lands on 0.6 with everything quieter spread
 * beneath it. `peak` and `saturated` are in the model so a probe can say which
 * of the two regimes a picture is in rather than leaving it to the eye.
 *
 * The one thing this cannot fix is genuinely flat music: uniform density draws a
 * uniform column under either rule, and correctly so.
 */
const REF_EVENTS_PER_BEAT = 34;
const MAX_ALPHA = 0.6;

/**
 * The faintest a bucket that HAS events may draw. A linear scale necessarily
 * crushes its bottom end, and note density spans two orders of magnitude: in a
 * song peaking at 40 events a beat, a one-event beat lands on 0.015 alpha — a
 * step of nine in the blue channel over the column's ground, which is to say
 * invisible. A stretch of music that reads as empty is worse than a coarse one,
 * because empty is a thing the minimap is specifically supposed to tell you.
 *
 * 0.06 is not a guess: it is the alpha the design fills its own viewport
 * rectangle with over this same ground, and that rectangle is legible. So it is
 * the measured floor of visibility here. The cost is that the very quietest
 * buckets stop being distinguishable from each other — they were not
 * distinguishable before either, they were both nothing.
 */
const MIN_ALPHA = 0.06;

/**
 * Reusable buffer. Same discipline as every other model here: the draw path
 * allocates nothing, so both arrays are sized once at the cap and only the first
 * `markCount` entries are ever meaningful.
 */
export function createMinimapBuffer(maxMarks = MAX_MARKS) {
  return {
    /** Per-bucket event count. What was measured. */
    counts: new Uint32Array(maxMarks),
    /** Per-bucket opacity, 0..MAX_ALPHA. What is drawn. */
    marks: new Float32Array(maxMarks),
    markCount: 0,
    /** A mark spans this many beats; 1 until the song outgrows the cap. */
    beatsPerMark: 1,
    ticksPerMark: TICKS_PER_BEAT,

    /** The span the column represents, top to bottom. */
    songTicks: 0,
    songBeats: 0,
    /** Where the notes and clips actually stop, before the viewport and the
     *  playhead get a say in how far the map has to reach. */
    contentTicks: 0,

    /** The viewport rectangle and the playhead, as fractions of the column. */
    viewTop: 0,
    viewHeight: 0,
    playhead: 0,
    playheadTick: 0,
    /** The tracker is scrolled past the end of the map, so the rectangle is
     *  pinned rather than positioned. Without this a pinned rectangle and a
     *  rectangle that happens to be at the bottom are the same picture. */
    beyond: false,

    /** Totals, for a probe and for the density scale. */
    events: 0,
    /** Notes the map had nowhere to put — see the scan. Non-zero means the note
     *  feed and the clip extents disagree about where the song ends. */
    dropped: 0,
    peak: 0,
    saturated: false,
    /** False when there is no engine store to read — which is a different thing
     *  from a song with no notes in it, and the two must not look the same. The
     *  column is 29px wide and cannot carry a sentence saying so, so it says it
     *  here and the probe reports it. */
    known: false,

    /**
     * Bumped only when `counts`/`marks` actually change, so the renderer can
     * skip a redraw it does not need. It names the marks and nothing else — the
     * viewport and playhead move without it and the renderer guards those on
     * their own values.
     */
    revision: 0,

    /**
     * The cache key for the density scan, which is the only O(noteCount) work
     * here. It names every input the counts are computed from: which notes
     * (notesRevision, noteCount), how long the song is reckoned to be
     * (extentsRevision, and songTicks, which moves when you scroll past the end),
     * and how many beats a bucket holds. GUIDELINES 2.1 is a list of caches that
     * omitted one of those.
     */
    _notesRev: -1,
    _noteCount: -1,
    _extRev: -1,
    _songTicks: -1,
    _beatsPerMark: -1,
    _cap: maxMarks,

    /** The cached content scan and its own key — a strict subset of the one
     *  above, because where the material ends does not depend on how the column
     *  is bucketed. -2 so the first call rescans even with no engine attached. */
    _content: 0,
    _cNotesRev: -2,
    _cNoteCount: -2,
    _cExtRev: -2,
  };
}

/** Smallest power of two >= n, for n >= 1. */
function pow2AtLeast(n) {
  let p = 1;
  while (p < n) p *= 2;
  return p;
}

/**
 * Where the song stops. Clip extents are the authority — a clip is the container
 * for notes and the space between clips is genuinely empty (GUIDELINES 2), so
 * the last placement's end is the end of the material. Notes are the fallback
 * for a feed that has not published extents, not a second opinion.
 */
function contentEndOf(engine) {
  let end = 0;
  const ec = engine.extentCount || 0;
  for (let i = 0; i < ec; i++) {
    const e = engine.extents[i];
    if (e && e.endTick > end) end = e.endTick;
  }
  if (end > 0) return end;
  const nc = engine.noteCount || 0;
  for (let i = 0; i < nc; i++) {
    const n = engine.notes[i];
    const t = n.tOff > n.tOn ? n.tOff : n.tOn;
    if (t > end) end = t;
  }
  return end > 0 ? end + TICKS_PER_BAR : 0;
}

/**
 * @param {{engine:object|null, startTick:number, visibleTicks:number,
 *          playheadTick:number}} opts
 *
 * `engine` is the store from wire.js; only `notes`/`noteCount`,
 * `extents`/`extentCount`, the two revisions and `playheadTick` are read.
 *
 * The viewport arrives in TICKS, not rows. The tracker holds its position as a
 * row index at a zoom, so the caller converts — `startRow * zoom.rowNanoticks`
 * and `rowCount * zoom.rowNanoticks` — for the reason GUIDELINES 2 gives: a row
 * index is a function of the current zoom, and the minimap's own axis is not.
 * Passing rows here would make the map's scale depend on the tracker's zoom.
 */
export function buildMinimapModel(opts, buf) {
  const {
    engine = null,
    startTick = 0,
    visibleTicks = 0,
    playheadTick = engine ? engine.playheadTick : 0,
  } = opts;

  if (!buf) buf = createMinimapBuffer();

  buf.known = !!engine;
  buf.playheadTick = playheadTick;

  // -1 for "there is no engine", which wire.js's counters can never be: they
  // start at 0 and only climb. So no-engine and empty-engine are different keys,
  // and a page booted with ?engine=off does not rescan nothing every frame.
  const notesRev = engine ? engine.notesRevision : -1;
  const noteCount = engine ? engine.noteCount : -1;
  const extRev = engine ? engine.extentsRevision : -1;

  // Where the song stops, cached on the same key. This is not a string or an
  // object, so it is not the allocation this pass is about — it is a SCAN, over
  // every extent and, on a feed that has published none, over every note. On
  // stress-512 that fallback is 80,896 iterations, and it ran on every frame of
  // playback to produce a number that only moves when an edit lands. The key is
  // the whole of what contentEndOf reads: which notes (notesRevision, noteCount)
  // and which extents (extentsRevision, which wire.js bumps on a count change as
  // well as a field change).
  if (buf._cNotesRev !== notesRev || buf._cNoteCount !== noteCount || buf._cExtRev !== extRev) {
    buf._cNotesRev = notesRev; buf._cNoteCount = noteCount; buf._cExtRev = extRev;
    buf._content = engine ? contentEndOf(engine) : 0;
  }
  const content = buf._content;
  buf.contentTicks = content;

  // The column spans the MATERIAL, and never the viewport. Letting it stretch to
  // cover wherever the tracker is looking is the obvious first version and it is
  // wrong twice over. The tracker scrolls to row 100,000 whatever the song is
  // (index.html's TOTAL_ROWS), so the span is unbounded and parking the cursor
  // out there collapses 512 bars into the top two pixels. And it moves the
  // BUCKETING: measured, scrolling into the last screenful of a 512-bar song
  // pushed the span past 2,048 beats, which pushed beats-per-mark from 8 to 16
  // and halved the mark count — the picture changing resolution because you
  // scrolled, with nothing about the music having changed.
  //
  // So the span is fixed by the content, the viewport rectangle clamps to the
  // bottom when you go past the end, and `beyond` says it is pinned rather than
  // positioned. Rounded up to a bar to keep the last mark whole.
  const floor = content > MIN_SONG_TICKS ? content : MIN_SONG_TICKS;
  const songTicks = Math.ceil(floor / TICKS_PER_BAR) * TICKS_PER_BAR;
  buf.beyond = startTick >= songTicks;
  const songBeats = Math.round(songTicks / TICKS_PER_BEAT);
  buf.songTicks = songTicks;
  buf.songBeats = songBeats;

  const beatsPerMark = pow2AtLeast(Math.ceil(songBeats / buf._cap));
  const markCount = Math.min(buf._cap, Math.ceil(songBeats / beatsPerMark));
  const prevMarkCount = buf.markCount;
  buf.beatsPerMark = beatsPerMark;
  buf.ticksPerMark = beatsPerMark * TICKS_PER_BEAT;
  buf.markCount = markCount;

  const stale = buf._notesRev !== notesRev
    || buf._noteCount !== noteCount
    || buf._extRev !== extRev
    || buf._songTicks !== songTicks
    || buf._beatsPerMark !== beatsPerMark;

  if (stale) {
    const counts = buf.counts;
    const marks = buf.marks;
    // What the renderer draws is `marks`, so that is what decides whether this
    // was a change — a rescan landing on the same alphas must not cost a redraw.
    // The mark COUNT is the exception: the same alphas over a different number
    // of buckets is a different picture.
    let changed = prevMarkCount !== markCount;
    for (let i = 0; i < markCount; i++) counts[i] = 0;
    let events = 0;
    let dropped = 0;
    if (engine) {
      // Every track, not the cursor's: the minimap is where the SONG is dense,
      // and one track's silence says nothing about whether that stretch is
      // empty. Counted by note start, like the design counts events — this is an
      // event histogram, not a coverage measure, so a long note is one mark's
      // worth of ink at the beat it begins.
      const tpm = buf.ticksPerMark;
      const nc = noteCount;
      const notes = engine.notes;
      for (let i = 0; i < nc; i++) {
        const t = notes[i].tOn;
        // Off the map: a note before zero, or one past the last placement's end,
        // which means the extents and the notes disagree about where the song
        // stops. Counted rather than dropped in silence — a minimap missing the
        // only marks that would have shown you the disagreement is the shape of
        // bug this project keeps having.
        const b = t < 0 ? -1 : (t / tpm) | 0;
        if (b < 0 || b >= markCount) { dropped++; continue; }
        counts[b]++;
        events++;
      }
    }
    let peak = 0;
    for (let i = 0; i < markCount; i++) {
      const perBeat = counts[i] / beatsPerMark;
      if (perBeat > peak) peak = perBeat;
    }
    const stretch = peak / MAX_ALPHA;
    const ref = stretch > REF_EVENTS_PER_BEAT ? stretch : REF_EVENTS_PER_BEAT;
    for (let i = 0; i < markCount; i++) {
      const c = counts[i];
      const scaled = Math.min(MAX_ALPHA, c / beatsPerMark / ref);
      const a = c === 0 ? 0 : (scaled < MIN_ALPHA ? MIN_ALPHA : scaled);
      if (marks[i] !== a) { marks[i] = a; changed = true; }
    }
    buf.events = events;
    buf.dropped = dropped;
    buf.peak = peak;
    // True when the design's fixed divisor would have clamped the busiest beat,
    // i.e. when this is drawing on the rescaled axis rather than the design's.
    buf.saturated = stretch > REF_EVENTS_PER_BEAT;
    if (changed) buf.revision++;

    buf._notesRev = notesRev;
    buf._noteCount = noteCount;
    buf._extRev = extRev;
    buf._songTicks = songTicks;
    buf._beatsPerMark = beatsPerMark;
  }

  // Fractions of the column, never pixels: the model has no idea how tall it is
  // drawn, and the renderer is the only place that knows a rectangle can be too
  // short to see.
  const top = startTick / songTicks;
  buf.viewTop = top < 0 ? 0 : (top > 1 ? 1 : top);
  const h = visibleTicks / songTicks;
  buf.viewHeight = h < 0 ? 0 : (h > 1 - buf.viewTop ? 1 - buf.viewTop : h);
  const p = playheadTick / songTicks;
  buf.playhead = p < 0 ? 0 : (p > 1 ? 1 : p);

  return buf;
}

/** The tick a fraction down the column refers to. The inverse of the projection
 *  above, so a click on the map can become a seek without the renderer knowing
 *  what a tick is. */
export function tickAtFraction(buf, f) {
  const c = f < 0 ? 0 : (f > 1 ? 1 : f);
  return Math.round(c * buf.songTicks);
}
