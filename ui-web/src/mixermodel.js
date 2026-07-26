// The mixer view-model: one strip per track.
//
// A note on truth. The engine accepts SetTrackMixer but publishes no gain, pan,
// mute or solo, so there is nothing to read back. Everything except the meters is
// therefore LOCAL state — what this client last sent — and it is wrong the moment
// anything else moves a fader: a project load, an undo, the agent, another
// surface. That is a real limitation and the UI says so rather than drawing a
// confident fader over a guess. Asked backend for the read-back; when it lands,
// `authoritative` flips and the local copy becomes a pending overlay like note
// edits already are.
//
// Meters ARE real: ui_track_peak_rms is published per track and arrives as
// `peaks` on the wire.

/** Gain range, in millibels, matching what the sidecar clamps to. */
export const GAIN_MIN = -9600;   // -96 dB, i.e. silence
export const GAIN_MAX = 1200;    // +12 dB
export const GAIN_UNITY = 0;

export const FLAG_MUTE = 1;
export const FLAG_SOLO = 2;

/** Local mixer state, one entry per track. */
export function createMixerState(trackCount = 16) {
  const strips = new Array(trackCount);
  for (let i = 0; i < trackCount; i++) {
    strips[i] = { track: i, gain: GAIN_UNITY, pan: 0, flags: 0, sentVersion: -1 };
  }
  return {
    strips,
    /** False until the engine publishes mixer state; see the note above. */
    authoritative: false,
  };
}

export function createMixerBuffer(trackCount = 16) {
  const strips = new Array(trackCount);
  for (let i = 0; i < trackCount; i++) {
    strips[i] = {
      track: i, name: '', gain: 0, gainDb: '0.0', pan: 0, panLabel: 'C',
      mute: false, solo: false, dimmed: false,
      peak: 0, peakPct: 0, faderPct: 0,
    };
  }
  return { strips, stripCount: 0, authoritative: false, contentRevision: 0,
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

const SIG = { peakRev: -1, trackCount: -1, mixRev: -1, authoritative: null };
let contentRevision = 0;

/**
 * @param {{tracks:number, engine:object|null, mixer:object, mixRevision:number}} opts
 */
export function buildMixerModel(opts, buf) {
  const { tracks: trackCount = 8, engine = null, mixer, mixRevision = 0 } = opts;
  const n = Math.min(trackCount, buf.strips.length);

  // Solo is exclusive-ish: if anything is soloed, everything unsoloed is dimmed.
  // Computed here rather than per-strip so one pass decides it for all of them.
  let anySolo = false;
  for (let t = 0; t < n; t++) if (mixer.strips[t].flags & FLAG_SOLO) { anySolo = true; break; }

  for (let t = 0; t < n; t++) {
    const src = mixer.strips[t];
    const s = buf.strips[t];
    s.track = t;
    s.name = 'T' + String(t + 1).padStart(2, '0');
    s.gain = src.gain;
    s.gainDb = gainLabel(src.gain);
    s.pan = src.pan;
    s.panLabel = panLabel(src.pan);
    s.mute = (src.flags & FLAG_MUTE) !== 0;
    s.solo = (src.flags & FLAG_SOLO) !== 0;
    s.dimmed = anySolo && !s.solo;
    s.faderPct = faderPosition(src.gain);
    // Peak RMS is 0..1 linear; show it on a dB scale or everything below -20
    // is invisible, which is exactly the range a meter needs to be useful in.
    const p = engine && t < engine.peakCount ? engine.peaks[t] : 0;
    s.peak = p;
    s.peakPct = p <= 0 ? 0 : Math.max(0, Math.min(1, 1 + Math.log10(Math.max(p, 1e-5)) / 5));
  }
  buf.stripCount = n;
  buf.authoritative = mixer.authoritative;

  // Meters move every frame, so they are deliberately NOT in the revision: the
  // renderer updates meter heights unconditionally and everything else only on
  // a real change. Putting peaks in here would rebind every strip at 86 Hz.
  if (SIG.trackCount !== n || SIG.mixRev !== mixRevision
      || SIG.authoritative !== mixer.authoritative) {
    SIG.trackCount = n; SIG.mixRev = mixRevision;
    SIG.authoritative = mixer.authoritative;
    contentRevision++;
  }
  buf.contentRevision = contentRevision;
  return buf;
}
