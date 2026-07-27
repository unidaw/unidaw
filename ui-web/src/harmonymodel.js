// The harmony card's view-model: the key in force at the playhead, and an
// honest account of the tuning that is not published.
//
// WHAT CROSSES THE WIRE, exactly: a harmony event is `{tick, root, scaleId}`,
// sixteen bytes, decoded in wire.js — and that is all of it. The engine holds
// more than that and encodes none of it: `apps/harmony_timeline.h` carries a
// `flags` word the frame drops, and `apps/scale_library.h` models a scale as
// per-degree `Interval{cents, ratioNum, ratioDen}` plus its own octave, which is
// exactly the data a tuning display needs and exactly the data no frame has ever
// contained. There is no TET field, no scala reference, and no per-degree cents.
//
// So this file names the key, which is real, and refuses to name a tuning, which
// is not. The design's TET selector, its "31 steps · scala" subtitle and its
// seven-row cents ladder are all controls over values that do not exist; drawing
// plausible numbers into them is the silent-plausible-wrongness GUIDELINES 2.1
// is a table about, and the mixer's "these faders are a local guess" line is the
// precedent for saying so on screen instead. When the wire grows a tuning, the
// notice goes away in the same commit that reads it.
//
// One further honesty: the engine's four scales (scale_library.cpp) are defined
// in cents and every one of them is a multiple of 100 with a 1200-cent octave.
// 12-TET is therefore true of what exists today rather than published by it,
// which is why `tuningKnown` is false and the chip is drawn as a readout.

/** Nanoticks. Same constant the tracker, arrange and piano projections use. */
const TICKS_PER_BAR = 3840000;
const TICKS_PER_BEAT = TICKS_PER_BAR / 4;

export const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/** From apps/scale_library.cpp. Index 0 is unused; the engine's ids start at 1. */
export const SCALE_NAMES = ['', 'major', 'minor', 'dorian', 'mixolydian'];

/**
 * How a harmony event is spelled. index.html has a `nameHarmony` of the same
 * shape feeding the chrome and the tracker's harmony column, and this card takes
 * `opts.nameHarmony` so all three can be the SAME function — two spellers for one
 * field is how one of them gets a fix the other does not.
 */
export function harmonyLabel(root, scaleId) {
  const pc = NOTE_NAMES[((root % 12) + 12) % 12] || '?';
  return pc + ' ' + (SCALE_NAMES[scaleId] || ('scale ' + scaleId));
}

/**
 * The one line this card owes the reader. Specific about which fields are
 * missing, because "tuning coming soon" would not tell anyone whether the cents
 * they are looking at came from the engine.
 */
export const TUNING_NOTICE =
  'the wire carries tick, root and scaleId only — no TET, no scala, no '
  + 'per-degree cents; the engine’s own scales are all 12-TET';

/** What the tuning chip reads. Inferred from the engine's scales, not published. */
export const TUNING_INFERRED = '12-TET';

export function createHarmonyBuffer(rowCap = 8) {
  const rows = new Array(rowCap);
  for (let i = 0; i < rowCap; i++) {
    rows[i] = { index: 0, tick: 0, root: 0, scaleId: 0,
                num: '', name: '', at: '', current: false,
                _r: -1, _s: -1, _b: -1, _i: -1 };
  }
  return {
    /** False when nothing is publishing a timeline at all — distinct from an
     *  engine that published an empty one, which is a project with no key. */
    known: false,
    open: true,
    key: '', root: -1, scaleId: -1,
    /** Index into the timeline of the event in force, -1 when there is none. */
    index: -1, count: 0,
    since: '', at: '', pos: '', fields: '',
    version: -1, versionText: '',
    notice: '',
    tuning: TUNING_INFERRED, tuningKnown: false, tuningNotice: TUNING_NOTICE,
    rows, rowCount: 0, rowFirst: 0, more: '',
    _cap: rowCap,
    // Cache keys, one set per derived string. Every one of them names the whole
    // of what its string is computed from, so a draw that changes nothing writes
    // nothing — GUIDELINES 3.0, and 2.1 for why a partial key is worse than none.
    _kKeyR: -1, _kKeyS: -1, _kSince: -1, _kBar: -1, _kBeat: -1,
    _kPosI: -2, _kPosN: -1, _kVer: -2,
    _kMoreF: -1, _kMoreN: -1, _kMoreC: -1,
  };
}

/**
 * @param {{harmony:Array|null, playheadTick:number, version:number,
 *          open:boolean, nameHarmony:function|null}} opts
 *
 * `harmony` is `store.harmony` — the decoded timeline, ascending by tick. The
 * event in force is the LAST one at or before the playhead, which is what makes
 * the key change as the playhead crosses one; the chrome had this hardcoded to
 * "C major" and was wrong three bars in four.
 */
export function buildHarmonyModel(opts, buf) {
  const { harmony = null, playheadTick = 0, version = -1, open = true,
          nameHarmony = null } = opts;

  buf.known = !!harmony;
  buf.open = open !== false;

  if (buf._kVer !== version) {
    buf._kVer = version;
    buf.version = version;
    // Nothing at all rather than "v-1": a version on screen that nobody could
    // act on is worse than an empty slot. Same call the chain makes.
    buf.versionText = version >= 0 ? 'harmony v' + version : '';
  }

  const count = harmony ? harmony.length : 0;

  // The playhead's own position, so the mapping line shows what is being asked
  // as well as what was answered. Keyed on the BEAT, not the tick, or this
  // rebuilds a string on every frame of playback.
  const bar = Math.floor(playheadTick / TICKS_PER_BAR) + 1;
  const beat = Math.floor((playheadTick % TICKS_PER_BAR) / TICKS_PER_BEAT) + 1;
  if (buf._kBar !== bar || buf._kBeat !== beat) {
    buf._kBar = bar; buf._kBeat = beat;
    buf.at = 'bar ' + bar + '.' + beat;
  }

  let index = -1;
  for (let i = 0; i < count; i++) {
    if (harmony[i].tick <= playheadTick) index = i; else break;
  }

  if (buf._kPosI !== index || buf._kPosN !== count) {
    buf._kPosI = index; buf._kPosN = count;
    buf.index = index; buf.count = count;
    buf.pos = index < 0 ? 'no event here' : 'event ' + (index + 1) + ' of ' + count;
  }

  buf.notice = !harmony
    ? 'no engine attached — the harmony timeline is the engine’s'
    : (count === 0
        ? 'this project has no harmony events — nothing defines a key'
        : (index < 0
            ? 'the playhead is before the first harmony event'
            : ''));

  const cur = index >= 0 ? harmony[index] : null;
  const root = cur ? cur.root : -1;
  const scaleId = cur ? cur.scaleId : -1;
  if (buf._kKeyR !== root || buf._kKeyS !== scaleId) {
    buf._kKeyR = root; buf._kKeyS = scaleId;
    buf.root = root; buf.scaleId = scaleId;
    buf.key = cur
      ? (nameHarmony ? nameHarmony(cur).label : harmonyLabel(root, scaleId))
      : '—';
    // The raw fields beside the name they produce. This is the only mapping the
    // card can show that is entirely made of published numbers, so it is the one
    // that replaces the design's "17-4 -> G4 +38c -> 391.27 Hz".
    buf.fields = cur ? 'root ' + root + ' · scale ' + scaleId : '';
  }

  const sinceBar = cur ? Math.floor(cur.tick / TICKS_PER_BAR) + 1 : -1;
  if (buf._kSince !== sinceBar) {
    buf._kSince = sinceBar;
    buf.since = sinceBar > 0 ? 'since bar ' + sinceBar : '';
  }

  // A window over the timeline, not the whole of it: a project may key-change
  // every bar for ten minutes and the card is 331px wide. Anchored two events
  // behind the current one so a change that is about to happen is on screen.
  const cap = buf.rows.length;
  const first = count <= cap ? 0
    : Math.max(0, Math.min(count - cap, (index < 0 ? 0 : index) - 2));
  const n = Math.min(cap, count);
  buf.rowFirst = first;
  buf.rowCount = n;
  for (let i = 0; i < n; i++) {
    const e = harmony[first + i];
    const r = buf.rows[i];
    r.index = first + i;
    r.tick = e.tick;
    r.root = e.root;
    r.scaleId = e.scaleId;
    r.current = first + i === index;
    if (r._i !== r.index) { r._i = r.index; r.num = String(r.index + 1).padStart(2, '0'); }
    if (r._r !== e.root || r._s !== e.scaleId) {
      r._r = e.root; r._s = e.scaleId;
      r.name = nameHarmony ? nameHarmony(e).label : harmonyLabel(e.root, e.scaleId);
    }
    const b = Math.floor(e.tick / TICKS_PER_BAR) + 1;
    if (r._b !== b) { r._b = b; r.at = 'bar ' + b; }
  }

  if (buf._kMoreF !== first || buf._kMoreN !== n || buf._kMoreC !== count) {
    buf._kMoreF = first; buf._kMoreN = n; buf._kMoreC = count;
    // A windowed list that does not say it is windowed is a list that looks
    // complete. Only shown when it actually is one.
    buf.more = count > n ? 'events ' + (first + 1) + '–' + (first + n) + ' of ' + count : '';
  }

  return buf;
}
