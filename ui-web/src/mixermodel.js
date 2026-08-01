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
 * `uiTrackFlags` bit 3 — a DIFFERENT byte from the mixer flags above.
 *
 * This one says the published slot is the MASTER: a real device chain and mixer whose
 * output is the master bus, with no arrangement rail, no clips and no tracker lane. The
 * two flag bytes are easy to confuse and the mistake is quiet, so the name says which.
 */
export const FLAG_TRACK_MASTER = 8;
/**
 * Bit 2: does this track quantize its notes to the harmony timeline?
 *
 * READ FROM HERE, WRITTEN BY ITS OWN OPCODE. It rides the mixer flags because that is the
 * per-track byte the engine already publishes, and it is NOT part of a mixer command — the
 * write path is SetTrackHarmonyQuantize (10). Toggling this bit and calling `sendMixer` would
 * be a control that appears to work and changes nothing, which is the exact shape this app
 * keeps finding; `dockApi.harmonyQuantize` is the way to set it.
 */
export const FLAG_HARMONY_QUANTIZE = 4;
/**
 * Bit 4: does note entry LEAVE the sounding note alone, or truncate it?
 *
 * Off — truncate — is today's behaviour and the default. It is the only setting in the tracker
 * that decides whether an edit loses data: `addNoteToClip` shortens the sounding note in the
 * same column IN THE DOCUMENT, so the duration you typed is gone at entry and no later view can
 * get it back. Read from here, written by SetTrackAllowNoteOverlap, same as the bit above.
 */
export const FLAG_ALLOW_OVERLAP = 8;

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
  /*
   * THE MASTER GETS ITS OWN ENTRY RATHER THAN A SLOT IN `strips`.
   *
   * It occupies a published slot like any track, but that slot's INDEX is one past the
   * last real track — so on a project at the track cap it would be `strips[64]` in a
   * 64-long array, and the optimistic value for the one fader everything passes through
   * would silently vanish. Keyed by what it is instead of by where it happens to sit.
   */
  return { strips, master: { gain: GAIN_UNITY, pan: 0, flags: 0, pendingUntil: -1 },
           authoritative: false };
}

/** Drop local values the engine has now answered. */
export function reconcileMixer(mixer, engine) {
  if (!engine || engine.mixerVersion < 0) { mixer.authoritative = false; return; }
  mixer.authoritative = true;
  for (let i = 0; i < mixer.strips.length; i++) {
    const s = mixer.strips[i];
    if (s.pendingUntil >= 0 && engine.mixerVersion > s.pendingUntil) s.pendingUntil = -1;
  }
  const m = mixer.master;
  if (m && m.pendingUntil >= 0 && engine.mixerVersion > m.pendingUntil) m.pendingUntil = -1;
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
  /*
   * THE MASTER STRIP. Shaped like the others so the view can draw it with the same code,
   * minus the two controls that mean nothing on it: solo (there is nothing to solo it
   * against) and a routing destination (it IS the destination).
   *
   * `slot` is where the engine published it and `-1` means it has not. The engine has
   * published the master as a real track with its own device chain, mixer and meters
   * since the master-track work landed, and honours its gain and mute on the summed
   * output — and the mixer drew nothing, because it iterates the LANE count, which stops
   * before the master deliberately so the tracker never draws it as a row.
   */
  const master = {
    track: -1, slot: -1, name: 'MAIN', gain: 0, gainDb: '0.0', pan: 0, panLabel: 'C',
    mute: false, pending: false, peak: 0, peakPct: 0, faderPct: 0, _nameSrc: -1,
  };
  return { strips, master, hasMaster: false, stripCount: 0, authoritative: false,
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
    s.harmonyQuantize = (flags & FLAG_HARMONY_QUANTIZE) !== 0;
    s.allowOverlap = (flags & FLAG_ALLOW_OVERLAP) !== 0;
    s.pending = mixer.strips[t].pendingUntil >= 0;
    s.dimmed = anySolo && !s.solo;
    s.faderPct = faderPosition(gain);
    // Peak RMS is 0..1 linear; show it on a dB scale or everything below -20
    // is invisible, which is exactly the range a meter needs to be useful in.
    const p = engine && t < engine.peakCount ? engine.peaks[t] : 0;
    s.peak = p;
    s.peakPct = p <= 0 ? 0 : Math.max(0, Math.min(1, 1 + Math.log10(Math.max(p, 1e-5)) / 5));
  }
  /*
   * THE MASTER, FOUND BY ITS FLAG AND NEVER BY ITS POSITION.
   *
   * The engine appends it today; `shared_memory.h` says in as many words that this is
   * not a promise ("counts back from the end rather than assuming a position"), and a
   * strip that assumed the last slot would one day attenuate a track instead of the mix.
   * Scanned over the ENGINE's extent, not `n`, because `n` is the lane count and the
   * whole point is that the master is not a lane.
   */
  buf.hasMaster = false;
  if (engine && engine.trackFlags) {
    const extent = Math.min(engine.trackCount | 0, engine.trackFlags.length);
    for (let t = 0; t < extent; t++) {
      if ((engine.trackFlags[t] & FLAG_TRACK_MASTER) === 0) continue;
      const m = buf.master;
      const local = mixer.master;
      // Same optimistic rule as a track strip: the local value while an edit is in
      // flight, the engine's once it has answered — whether it applied it or not, so a
      // REFUSED master fader move cannot sit on screen looking like state.
      const live = !engine || !mixer.authoritative || local.pendingUntil >= 0;
      const gain = live ? local.gain : (t < engine.mixCount ? engine.mixGain[t] : local.gain);
      const pan = live ? local.pan : (t < engine.mixCount ? engine.mixPan[t] : local.pan);
      const flags = live ? local.flags : (t < engine.mixCount ? engine.mixFlags[t] : local.flags);
      m.slot = t;
      if (m._nameSrc !== gain) { m._nameSrc = gain; m.gainDb = gainLabel(gain); }
      m.gain = gain;
      m.pan = pan;
      m.panLabel = panLabel(pan);
      m.mute = (flags & FLAG_MUTE) !== 0;
      m.pending = local.pendingUntil >= 0;
      m.faderPct = faderPosition(gain);
      const p = t < engine.peakCount ? engine.peaks[t] : 0;
      m.peak = p;
      m.peakPct = p <= 0 ? 0 : Math.max(0, Math.min(1, 1 + Math.log10(Math.max(p, 1e-5)) / 5));
      buf.hasMaster = true;
      break;
    }
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
