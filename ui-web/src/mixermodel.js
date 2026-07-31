// The mixer view-model: one strip per track.
//
// A note on truth, now resolved. The engine publishes per-track gain, pan and
// mute/solo (SHM v12), so this surface reads the engine rather than remembering
// what it sent. Local values survive only until the engine answers — the same
// optimistic pattern the note cells use, and for the same reason: a fader that
// keeps showing a local guess is wrong the moment a project load, an undo or
// another surface moves it.
//
// The read-back is keyed on ui_mixer_version, which moves only on change.

import { trackName } from './arrangemodel.js';

/** Gain range, in millibels, matching what the sidecar clamps to. */
export const GAIN_MIN = -9600;   // -96 dB, i.e. silence
export const GAIN_MAX = 1200;    // +12 dB
export const GAIN_UNITY = 0;

export const FLAG_MUTE = 1;
export const FLAG_SOLO = 2;

/**
 * Optimistic local edits, one entry per track. `pendingUntil` is the mixer
 * version this edit was composed against: once the engine publishes anything
 * newer it has seen the edit, and the published value wins whether it applied
 * it or not — exactly the base_version reconciliation the note path uses. Without
 * the "or not" a REJECTED fader move would sit on screen forever looking like
 * state.
 */
export function createMixerState(trackCount = 16) {
  const strips = new Array(trackCount);
  for (let i = 0; i < trackCount; i++) {
    strips[i] = { track: i, gain: GAIN_UNITY, pan: 0, flags: 0, pendingUntil: -1 };
  }
  return { strips, authoritative: false };
}

/** Drop local values the engine has now answered. */
export function reconcileMixer(mixer, engine) {
  if (!engine || engine.mixerVersion < 0) { mixer.authoritative = false; return; }
  mixer.authoritative = true;
  for (let i = 0; i < mixer.strips.length; i++) {
    const s = mixer.strips[i];
    if (s.pendingUntil >= 0 && engine.mixerVersion > s.pendingUntil) s.pendingUntil = -1;
  }
}

/** The value to draw: the local one while an edit is in flight, else the engine's. */
function resolve(mixer, engine, t, field, engineField) {
  const s = mixer.strips[t];
  if (!engine || !mixer.authoritative || s.pendingUntil >= 0) return s[field];
  return t < engine.mixCount ? engine[engineField][t] : s[field];
}

export function createMixerBuffer(trackCount = 16) {
  const strips = new Array(trackCount);
  for (let i = 0; i < trackCount; i++) {
    strips[i] = {
      track: i, name: '', gain: 0, gainDb: '0.0', pan: 0, panLabel: 'C',
      // Where this track's audio GOES. `outTo` is the destination track id, or
      // -1 for the master — "Main" on screen. See ROUTE_MASTER.
      outTo: -1,
      mute: false, solo: false, dimmed: false, pending: false,
      peak: 0, peakPct: 0, faderPct: 0,
      // The name the engine published that `name` was derived from. A number,
      // so it can never equal a string or the undefined a nameless engine
      // yields, and the first build therefore always computes a name. `gainDb`
      // and `panLabel` need no such sentinel: they are seeded with exactly what
      // gainLabel(0) and panLabel(0) return, which is what gain 0 / pan 0 mean.
      _nameSrc: -1,
    };
  }
  return { strips, stripCount: 0, authoritative: false,
           /* Changes when the DESTINATION LIST would — the strip count and the
              names. The view rebuilds its <option>s on this and not on the
              routing, which moves far more often and does not change the list. */
           routeKey: '',
           _shape: String(trackCount) };
}

/** Millibels to a short dB string. Only called when the value changed. */
export function gainLabel(mb) {
  if (mb <= GAIN_MIN) return '-inf';
  const db = mb / 100;
  // Round BEFORE choosing the sign, or -1 millibel prints as "-0.0" — which
  // reads as a real cut when it is a rounding artefact of the fader taper.
  const r = Math.round(db * 10) / 10;
  if (r === 0) return '0.0';
  return (r > 0 ? '+' : '') + r.toFixed(1);
}

/** Pan thousandths to L/C/R notation, as every mixer writes it. */
export function panLabel(p) {
  if (p === 0) return 'C';
  const n = Math.round(Math.abs(p) / 10);
  return (p < 0 ? 'L' : 'R') + n;
}

/**
 * Fader position 0..1 from millibels. Deliberately not linear in dB: a linear
 * dB fader spends most of its travel in territory nobody mixes in. This is the
 * usual cubic taper — unity sits around 70% of the way up.
 */
export function faderPosition(mb) {
  const t = (mb - GAIN_MIN) / (GAIN_MAX - GAIN_MIN);
  return Math.pow(Math.max(0, Math.min(1, t)), 3);
}

/** ...and its inverse, for dragging. */
export function gainAtPosition(pos) {
  const t = Math.pow(Math.max(0, Math.min(1, pos)), 1 / 3);
  return Math.round(GAIN_MIN + t * (GAIN_MAX - GAIN_MIN));
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
 * @param {{tracks:number, engine:object|null, mixer:object, mixRevision:number}} opts
 */
/** daw::TrackRouteKind. 1 is the master, 2 is another track. */
export const ROUTE_KIND_MASTER = 1;
export const ROUTE_KIND_TRACK = 2;

export function buildMixerModel(opts, buf) {
  const { tracks: trackCount = 8, engine = null, mixer, mixRevision = 0,
          // Per-track chains, which is where the engine publishes routing. The
          // mixer needs them for one field; passing the whole map rather than a
          // second derived structure keeps one source of truth.
          chains = null } = opts;
  const n = Math.min(trackCount, buf.strips.length);

  // Solo is exclusive-ish: if anything is soloed, everything unsoloed is dimmed.
  // Computed here rather than per-strip so one pass decides it for all of them.
  let anySolo = false;
  for (let t = 0; t < n; t++) {
    if (resolve(mixer, engine, t, 'flags', 'mixFlags') & FLAG_SOLO) { anySolo = true; break; }
  }

  for (let t = 0; t < n; t++) {
    const s = buf.strips[t];
    const gain = resolve(mixer, engine, t, 'gain', 'mixGain');
    const pan = resolve(mixer, engine, t, 'pan', 'mixPan');
    const flags = resolve(mixer, engine, t, 'flags', 'mixFlags');
    s.track = t;
    // trackName's fallback is String() + padStart() + a concat — three strings
    // per strip per frame to spell a name that only a rename can change. Keyed
    // on the published name ITSELF rather than on a version: a rename moves the
    // name and nothing else, which is precisely how the two track-name bugs in
    // GUIDELINES 2.1 got onto that list. t is fixed for this strip, so (name, t)
    // — the whole of trackName's input — is covered.
    const nm = engine && engine.names ? engine.names[t] : undefined;
    if (s._nameSrc !== nm) { s._nameSrc = nm; s.name = trackName(engine, t); }
    /*
     * WHERE THE TRACK'S AUDIO GOES.
     *
     * `null` routing is not "the master" — it is "the engine has not said yet",
     * and defaulting it to Main would show every track routed to Main before the
     * first snapshot arrives and would show a track sent to a group as sent to
     * Main until it happened to republish. So an unknown routing keeps whatever
     * was last known rather than inventing a plausible one.
     */
    const r = chains && chains[t] ? chains[t].routing : null;
    if (r) s.outTo = r.audioOutKind === ROUTE_KIND_TRACK ? r.audioOutTrack : -1;
    // Both labels are pure functions of the value beside them, and both were
    // rebuilt for every strip on every frame to produce the characters that were
    // already there. The value we store is the key. Moving a fader now allocates
    // a string on the frames it actually changes on, and nothing on the rest.
    if (s.gain !== gain) { s.gain = gain; s.gainDb = gainLabel(gain); }
    if (s.pan !== pan) { s.pan = pan; s.panLabel = panLabel(pan); }
    s.mute = (flags & FLAG_MUTE) !== 0;
    s.solo = (flags & FLAG_SOLO) !== 0;
    s.pending = mixer.strips[t].pendingUntil >= 0;
    s.dimmed = anySolo && !s.solo;
    s.faderPct = faderPosition(gain);
    // Peak RMS is 0..1 linear; show it on a dB scale or everything below -20
    // is invisible, which is exactly the range a meter needs to be useful in.
    const p = engine && t < engine.peakCount ? engine.peaks[t] : 0;
    s.peak = p;
    s.peakPct = p <= 0 ? 0 : Math.max(0, Math.min(1, 1 + Math.log10(Math.max(p, 1e-5)) / 5));
  }
  buf.stripCount = n;
  // Built here, once, rather than compared field by field in the view: it is one
  // small string per frame against N option elements per strip per frame.
  let key = n + ':';
  for (let i = 0; i < n; i++) key += buf.strips[i].name + ',';
  buf.routeKey = key;
  buf.authoritative = mixer.authoritative;
  return buf;
}
