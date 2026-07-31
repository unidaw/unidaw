// The device chain view-model: one card per device on the cursor's track.
//
// The engine's chain is published as ChainSnapshot diffs on its outbound ring —
// one entry per device, all sharing a chainVersion — which the sidecar drains
// and accumulates (it is a single-consumer ring, so one drainer and a shared
// buffer). By the time it reaches here it is already per-track and already
// deduplicated by version.
//
// CONTENTS arrive separately, and later. The chain snapshot carries a device's
// IDENTITY only (id, kind, position, patcher node, host slot, capabilities,
// bypass). Its NAME and PARAMETERS live in the plugin host, so the engine has to
// go and ask — one device per query (SHM v17, RequestDeviceParams) — and the
// answer lands in a region the sidecar forwards. So a card is drawn twice: once
// as identity, then again with contents when the answer comes back. Until it
// does the card says what it knows and no more; the alternative, a slider with a
// plausible number on it, is the failure mode GUIDELINES 2.1 is about.
//
// EVERY STRING HERE IS MEMOISED on the inputs it is built from. A rack of eight
// Zebra2s is 2,048 parameters, and this function runs on every draw: building
// one title, one capability phrase and one value string per row per frame is
// ~11 KB/draw of garbage, which is precisely the number GUIDELINES 3.0 exists to
// keep at zero. The rule for anything added below: if it concatenates, it caches
// the values it concatenated from, and it is rebuilt only when one of them moves.

import { generatorsFrom } from './patchermodel.js';
import { pitchName } from './wire.js';

/**
 * One shared empty edge list, and one reused subgraph mask.
 *
 * The mask is a scratch buffer `generatorsFrom` fills and reads within one call —
 * it never carries meaning between calls — so one of them for the whole module is
 * correct and keeps a per-card allocation off a path that runs at frame rate.
 */
const EMPTY_EDGES = Object.freeze([]);
let _genMask = null;

/**
 * DeviceKind, from apps/device_chain.h. A test parses that enum and holds this list equal to it,
 * because a hand-maintained mirror of an engine enum drifts — this one already had: the sampler
 * landed as kind 5 and every sampler on screen read "kind 5 #9" with a generic badge, which is
 * what an unnamed kind looks like and not what a missing entry looks like.
 */
export const DEVICE_KINDS = [
  'patcher event', 'patcher instrument', 'patcher audio', 'VST instrument', 'VST effect',
  'sampler',
];

/** The short badge each kind gets, matching the design's PATCHER / VST3 / UNI. */
const KIND_BADGE = ['PATCHER', 'PATCHER', 'PATCHER', 'VST3', 'VST3',
                    // UNI, not VST3: the sampler is rendered IN the engine rather than in a
                    // host process, and the badge is the one thing on the card that says where
                    // a device actually runs.
                    'UNI'];

/** DeviceCapability bits, same header. */
const CAPS = [
  [1, 'midi in'],
  [2, 'midi out'],
  [4, 'audio'],
];

/**
 * kUiMaxDeviceParams, from apps/shared_memory.h.
 *
 * The engine's region holds 256 and Zebra2 fills it, so this is the number the
 * rack has to be able to SHOW rather than a limit it chose. It is still a cap —
 * a plugin with more parameters than the region carries is truncated by the
 * engine before it ever reaches here — which is why `more` below counts what was
 * dropped instead of quietly listing a prefix.
 */
export const MAX_PARAMS = 256;

/**
 * Sentinels from device_chain.h. `kDeviceIdAuto` doubles as the engine's
 * "this chain is empty" marker in a snapshot, and `kHostSlotIndexDirect` means
 * the device is not hosted out-of-process at all.
 */
export const DEVICE_ID_AUTO = 0xffffffff;
const HOST_SLOT_DIRECT = 0xfffffffe;

/**
 * The stand-in for a chain entry that carries no `devices` at all.
 *
 * `entry.devices || []` looked free and was one array per draw for as long as
 * the sidecar had told us about a track without listing anything on it. Shared
 * because nothing ever writes to it — every reader below only measures it.
 */
const NO_DEVICES = [];

/**
 * Which capabilities a mask names, as a short phrase.
 *
 * Memoised by mask: there are three bits, so there are eight possible answers
 * for the lifetime of the process, and building one of them per card per frame
 * was an array plus a join per card per draw.
 */
/*
 * WHAT IS FLOWING BETWEEN TWO DEVICES.
 *
 * Ableton draws this and it is the thing that makes a chain readable at a
 * glance: you can see where MIDI becomes audio, and you can see when a device is
 * receiving nothing.
 *
 * CAPABILITY, not activity. Two reasons, and the second is the important one.
 * The engine publishes what each device consumes and emits (capability_mask on
 * the chain snapshot); it does not publish whether a note went past a moment
 * ago, and asking for that would mean a meter's worth of traffic at frame rate
 * for a picture that flickers. But even with activity available, capability is
 * what you want here: a chain is wrong when it CANNOT carry what you meant, and
 * that is true whether or not you happen to be playing. An activity light that
 * is dark during a rest looks identical to one that is dark because the routing
 * is broken.
 *
 * THE FAILURE THIS MUST NOT HAVE is showing MIDI flowing into a device that
 * ignores it, or audio out of a device that emits none. A confident lie about
 * the signal path is worse than no indicator, because the whole point is to be
 * believed when something looks wrong. So this carries the signal FORWARD
 * through each device's own declaration rather than assuming what a chain
 * "usually" looks like:
 *
 *   an instrument   consumes MIDI, emits audio  -> MIDI stops here, audio starts
 *   an event patcher consumes and emits MIDI    -> MIDI continues, no audio
 *   an audio effect  processes audio            -> audio continues, MIDI passes
 *
 * A track's own clip is the source, so MIDI is present before the first device.
 */
export const CAP_CONSUMES_MIDI = 1;
export const CAP_PRODUCES_MIDI = 2;
export const CAP_AUDIO = 4;

/** DeviceKind. Only the instrument is named here; the rest read from `caps`. */
const KIND_VST_INSTRUMENT = 3;
/**
 * WHAT MODULATES A SLOT, and whether any of it can actually move.
 *
 * `modMask` has a bit per (target, kind): `target * 2 + kind`, targets 0..4 being volume,
 * panning, pitch, cutoff and resonance, kinds 0 envelope and 1 LFO. A bit means the modulator
 * WOULD move something — an envelope with no points and an LFO with zero swing are both stored,
 * both round-trip, and both do nothing, and neither sets its bit.
 *
 * THE FILTER IS THE CATCH, and backend flagged it before I could fall in: a cutoff or resonance
 * modulator on a filter that is OFF is silent. The mask does not know that — it is a property of
 * the mod set, not of the modulator — so a row that drew a cutoff envelope without checking
 * `filterType` would show a live control over a dead one.
 *
 * That is the same lie this rack already refuses twice: the modulation badge's inert third
 * state, and the MAP badge hidden on a parameter the plugin ignores. So it is not hidden — a
 * configured modulator that cannot move anything is worth SEEING, because the reason it does
 * nothing is fixable and invisible.
 */
const MOD_TARGETS = ['volume', 'panning', 'pitch', 'cutoff', 'resonance'];
/** Targets whose modulators are silent while the mod set's filter is off. */
const FILTER_TARGETS = 3;   // cutoff and resonance, i.e. indices 3 and 4

/**
 * The filter types, by their wire value — `UiSamplerFilterPayload.filterType`: 0 off, 1 LP12,
 * 2 LP24, 3 HP, 4 BP.
 *
 * Named rather than numbered wherever a person types or reads one: "2" is not a thing anyone
 * remembers about a filter. Lives here because this is the file that already has to decide
 * whether a cutoff modulator is live or inert, and that decision IS `filterType !== 0` — so the
 * list and the rule that reads it stay in one place.
 */
export const FILTER_TYPES = ['off', 'lp12', 'lp24', 'hp', 'bp'];

/**
 * What an envelope can move, by its wire value — `kSamplerEnvTarget*`: 0 volume, 1 panning,
 * 2 pitch, 3 cutoff, 4 resonance.
 *
 * Beside FILTER_TYPES because the two are read together: a cutoff or resonance envelope over a
 * filter that is OFF is a live control on a dead one, which is the whole reason `modSummary`
 * takes both.
 */
export const ENV_TARGETS = ['volume', 'pan', 'pitch', 'cutoff', 'resonance'];

/**
 * The slot fields `SamplerSetSlot` can set, INDEXED BY THEIR WIRE VALUE — `UiSamplerSlotField`
 * in event_payloads.h, VoiceGroup 0 through EndFrame 26.
 *
 * Named because nobody remembers that a gate is field 2, and dense from 0 because the wire is:
 * the index IS the field id, so this list cannot be reordered or have a name dropped from the
 * middle without changing what every caller means. A ratchet holds it against the enum.
 *
 * `gate` is the one that matters most today: 0 is a one-shot that IGNORES note-off — right for a
 * drum, wrong for anything a person expects to be able to cut — and 1 releases the voice when
 * the note ends. The engine has had both since the sampler shipped and no UI could reach either.
 */
/**
 * The DEVICE-level sampler fields `SamplerSetDevice` (88) can set, indexed by their wire value —
 * `SamplerDeviceField`, DefaultGate 1 through DefaultView 3. Index 0 is unused on the wire, so
 * the list carries a hole rather than an off-by-one waiting to happen.
 *
 * `defaultGate` SEEDS a slot at mint and then stops mattering: `load-sample` and `slice` stamp
 * it onto slots they create, and the slot's own gate is the authority from that moment. It is
 * not a live override — a device flag the voice consulted on every note would be two facts about
 * one thing, which is the shape that produced the kit read-back disagreeing with the model.
 *
 * `defaultView` is a REMEMBERED VIEW rather than a mode: 0 = kit (the pad grid, one slot per
 * pad), 1 = sample (one sample filling the view with its waveform and slice markers). Seeded
 * from how many files were dropped and user-owned after that.
 */
export const DEVICE_FIELDS = ['', 'default-gate', 'voice-cap', 'default-view'];

/** What `defaultView`'s two states are called on a card. The engine has no opinion past these. */
export const DEVICE_VIEWS = ['kit', 'sample'];

export const SLOT_FIELDS = ['voicegroup', 'nna', 'gate', 'reverse', 'gain', 'pan', 'tune', 'pitchtrack', 'root', 'keylow', 'keyhigh', 'vellow', 'velhigh', 'selectmode', 'polyphony', 'chokefade', 'modset', 'stem', 'quality', 'layergroup', 'loopmode', 'sustainloop', 'loopstart', 'loopend', 'loopxfade', 'startframe', 'endframe',
  // 27/28, the repoint pair. `source` moves a slot onto a different loaded sample and `slice`
  // moves it onto a different slice of one — REFUSED, not clamped, when the id does not exist,
  // because a slot pointing at a source that is not there is silent and silence is not a
  // near-miss. `slice 0` is legal and means the whole sample.
  'source', 'slice'];

export function modSummary(modMask, filterType) {
  if (!modMask) return { mark: '', title: '' };
  let live = 0, inert = 0;
  const names = [];
  for (let t = 0; t < MOD_TARGETS.length; t++) {
    for (let kind = 0; kind < 2; kind++) {
      if (!(modMask & (1 << (t * 2 + kind)))) continue;
      const dead = t >= FILTER_TARGETS && !filterType;
      if (dead) inert++; else live++;
      names.push(`${MOD_TARGETS[t]} ${kind ? 'LFO' : 'envelope'}${dead ? ' (filter is off)' : ''}`);
    }
  }
  return {
    // `~` is movement; `!` is movement that cannot happen. One character each, so a row says
    // which it has without spending width on which targets — the title carries that.
    mark: (live ? '~' : '') + (inert ? '!' : ''),
    title: names.join(', '),
  };
}

/** The built-in sampler. Named because its card draws its KIT where a plugin's params go. */
const KIND_SAMPLER = 5;

/**
 * A slot's length, in the unit a person reads it in.
 *
 * Frames are what the engine publishes and seconds are what anyone thinks in, but the sample
 * rate is not on this side of the wire — so this says frames, briefly, rather than inventing a
 * rate to divide by. A wrong duration is worse than an honest count.
 */
const FRAME_TEXT = new Map();
function frameText(n) {
  let s = FRAME_TEXT.get(n);
  if (s === undefined) {
    s = n >= 1000000 ? `${Math.round(n / 100000) / 10}Mf`
      : n >= 1000 ? `${Math.round(n / 100) / 10}kf` : `${n}f`;
    FRAME_TEXT.set(n, s);
  }
  return s;
}
/** kUiMeterSilent: silent or below the floor, and a real reading, not a hole. */
export const METER_SILENT = -32768;
/**
 * The bottom of the drawn scale, in dBFS millibels.
 *
 * -60 dB, which is the range a channel meter is useful over. Not the -327 dB the
 * i16 could hold: a scale that reserves four fifths of its length for levels
 * nobody can hear puts every real signal in the top inch, where the difference
 * between "hot" and "clipping" is a pixel — and telling those two apart is the
 * entire reason to look at a meter.
 */
const METER_FLOOR_MB = -6000;

/**
 * A millibel reading as a fraction of the drawn scale, 0..1.
 *
 * SILENT is 0 rather than being clamped like any other low value, because it is
 * not a low value: it is "there is nothing here". They land in the same place on
 * screen and mean different things, and the distinction is what stops an
 * instrument's absent input from drawing as a very quiet signal.
 */
/**
 * A millibel reading as the dB string a card prints.
 *
 * Named and exported so it can be TESTED. It was an inline `(mb / 100).toFixed(1)`,
 * and the e2e that "checked" it compared the model's own string against the same
 * string in the DOM — true by construction, and it stayed green when the divisor was
 * mutated from 100 to 10. A check that compares a value to itself defends the copy,
 * not the arithmetic.
 *
 * SILENT gets the symbol rather than a number, because there is no dB value for
 * nothing and -327.7 is what the sentinel would print.
 */
export function meterDb(mb) {
  return mb === METER_SILENT ? "−∞" : (mb / 100).toFixed(1);
}

export function meterScale(mb) {
  if (mb === METER_SILENT || mb <= METER_FLOOR_MB) return 0;
  if (mb >= 0) return 1;
  return (mb - METER_FLOOR_MB) / -METER_FLOOR_MB;
}

/**
 * The n+1 gaps in a chain of n devices: before the first, between each pair,
 * after the last. Each says what is present at that point.
 *
 * `dead` marks a gap carrying nothing at all — which is not a drawing detail but
 * the most useful thing this can tell you: a device downstream of it can never
 * do anything, and until now the only way to find that out was to wonder why you
 * could not hear it.
 */
export function resolveFlow(devices, out) {
  const gaps = out || [];
  while (gaps.length < devices.length + 1) gaps.push({ midi: false, audio: false, dead: false });
  // The clip feeds the head of the chain.
  let midi = true, audio = false;
  for (let i = 0; i <= devices.length; i++) {
    const g = gaps[i];
    g.midi = midi; g.audio = audio; g.dead = !midi && !audio;
    if (i === devices.length) break;
    const d = devices[i];
    const caps = d.caps | 0;
    // A BYPASSED device is a wire. Not "a device that does nothing" — its
    // conversions do not happen either, so an instrument that is bypassed does
    // not turn MIDI into audio, and the gap after it must say so.
    if (d.bypass) continue;
    const consumes = !!(caps & CAP_CONSUMES_MIDI);
    const produces = !!(caps & CAP_PRODUCES_MIDI);
    midi = produces ? true : (consumes ? false : midi);
    if (caps & CAP_AUDIO) audio = true;
  }
  gaps.length = devices.length + 1;
  return gaps;
}

const CAP_TEXT = [];
function describeCaps(mask) {
  const m = mask & 7;
  let s = CAP_TEXT[m];
  if (s !== undefined) return s;
  const on = [];
  for (const [bit, name] of CAPS) if (m & bit) on.push(name);
  s = on.join(' · ');
  CAP_TEXT[m] = s;
  return s;
}

/** The empty link list, shared. A track with no modulation must not allocate one. */
const NO_MODS = Object.freeze([]);

/** One pooled parameter row's worth of view-model. Mutated, never replaced. */
function createParamSlot() {
  return {
    index: 0,       // the engine's own ordering index; NOT a durable identity
    uid: '',        // hashStableId16 hex — the durable one, see shared_memory.h
    name: '',
    display: '',
    frac: 0,        // 0..1, already normalised by the bridge
    milli: 0,       // the same value in the engine's own unit, for comparison
    _dv: NaN,       // what `display` was derived from, when we had to derive it
    /**
     * THE MODULATION LINK THAT MOVES THIS PARAMETER, by link id, or 0 for none.
     *
     * The id and not a boolean, because unmapping needs it: `RemoveModLink` addresses a
     * link, and a row that knew only "something modulates me" would have to search for
     * which one at click time — from a renderer, against a list the model already walked.
     *
     * Matched on (targetDevice, uid16) and NEVER on the target id alone. `targetId` is the
     * plugin's own parameter index and the rows are in the plugin's order, so they look
     * interchangeable — until a plugin publishes a sparse or reordered set, and then the
     * badge lights on the wrong row. The uid16 is the identity; the index is the ordering.
     */
    mod: 0,
    /** How far the source sweeps it, 0..1 of the parameter's range. */
    modDepth: 0,
    /*
     * WHAT THE PARAMETER IS (v30), not just where it is.
     *
     * `range` is the endpoints AS THE PLUGIN RENDERS THEM — "-60.0 dB .. 0.0 dB" — and it is the
     * load-bearing one: for a VST3 through JUCE the normalisable range is 0..1, so the numeric
     * min and max say nothing and the real range exists only as that text.
     *
     * `steps` non-zero means a SWITCH with that many positions, and drawing one as a continuous
     * bar is not merely ugly: it says the value between two positions is reachable, and it is not.
     *
     * `automatable: false` means the plugin will IGNORE an automation lane pointed here, so the
     * rack must not offer one — a control that accepts a curve nothing reads is the same class of
     * lie as a modulation badge over a link that moves nothing.
     */
    unit: '', range: '', steps: 0, automatable: true, defaultValue: 0, isSlot: false,
    /**
     * The link exists and CANNOT WORK: it has no uid16, and the engine addresses a VST
     * parameter by uid16 alone. Drawn differently from a working link, because a badge
     * that lit the same way for both would be the exact lie this whole feature risks —
     * an interface reporting a modulation that moves nothing.
     */
    modInert: false,
  };
}

/**
 * The bus summary, interned on its own content.
 *
 * The domain is tiny — a device has at most 32 buses — and the string is a pure
 * function of two small counts, so the key IS the content and there is nothing to
 * invalidate. Same reasoning as the lane labels in viewmodel.js: a table keyed on a
 * POSITION needs a guard and a clear; a table keyed on what it spells does not.
 */
const BUS_TEXT = new Map();
function busSummary(ins, outs) {
  const key = ins * 64 + outs;
  let t = BUS_TEXT.get(key);
  if (t === undefined) {
    t = outs && ins ? outs + ' out \u00b7 ' + ins + ' in'
      : outs ? outs + ' out'
      : ins ? ins + ' in'
      : '';
    BUS_TEXT.set(key, t);
  }
  return t;
}

/** "3/8" while a device's buses are still arriving. Same interning argument. */
const BUS_PARTIAL = new Map();
function busPartialText(have, want) {
  const key = have * 64 + want;
  let t = BUS_PARTIAL.get(key);
  if (t === undefined) { t = 'buses ' + have + '/' + want; BUS_PARTIAL.set(key, t); }
  return t;
}

/**
 * The key a device's parameters are stored and looked up under.
 *
 * (track, device), never device alone. DEVICE IDS ARE PER-TRACK: in
 * presets/projects/maximal.uniproj.json all six tracks have a device with
 * `device_id: 0`, so a map keyed on the id alone has ONE slot for every track's
 * first device. The last answer to arrive wins and every track's rack shows that
 * plugin's name and parameters — six tracks all reporting "Analog Heat", and a
 * plugin you just added never appearing because the slot is already full.
 *
 * The REQUEST side already keyed on the pair; only the store and the lookup did
 * not, so the right question was asked and the answer was filed under the wrong
 * name. Exported so there is one definition rather than two that agree until
 * somebody edits one.
 */
export function paramKey(track, device) { return track * 65536 + device; }

export function createChainBuffer(cap = 16) {
  const cards = new Array(cap);
  for (let i = 0; i < cap; i++) {
    cards[i] = { id: 0, kind: 0, badge: '', title: '', pos: 0,
                 sub: '', caps: '', bypass: false, selected: false, patcherNode: -1,
                 // What is flowing INTO this card, and — on the last card only —
                 // out of it. Two small strings rather than a gap object per
                 // card, because the renderer's whole job with them is to toggle
                 // a class and a class is a string.
                 flowIn: '', flowOut: '',
                 named: false, params: [], paramCount: 0, more: '',
                 // The device's audio buses (kShmVersion 20): what it can actually
                 // route, which is the difference between "a plugin" and "a plugin
                 // with eight stems and a sidechain input". Empty until the engine
                 // publishes a chain that carries them.
                 busText: '', busTruncated: false, busPartial: false,
                 /*
                  * v24 PER-INSERT METERS, as fractions of the drawn scale plus
                  * the dB the card prints. `hasIn` says whether an INPUT meter
                  * belongs on this device at all — see meterScale.
                  *
                  * Numbers on the card object rather than a meter object per
                  * card: these change every frame by definition, and a fresh
                  * object per card per frame is the one allocation pattern this
                  * file exists to avoid.
                  */
                 hasMeter: false, hasIn: false,
                 inRms: 0, inPeak: 0, outRms: 0, outPeak: 0, meterText: '',
                 _bKey: -1, _mOut: 1,
                 // Memo keys for the three strings this card builds. Named for
                 // what they cache so a fourth input added to one of them is
                 // obviously missing from its key.
                 _tName: null, _tKind: -1, _tId: -1,
                 _sSlot: -1, _sNode: -2, _sCount: -1, _sMore: -1,
                 _mCount: -1,
                 // And the key for the parameter slots themselves: which answer
                 // filled them, for which device, and how much of it was taken.
                 _pSrc: undefined, _pId: -1, _pCount: -1 };
  }
  return { cards, cardCount: 0, track: 0, trackName: '', version: -1,
           known: false, notice: '', _cap: cap,
           _nNamed: -1, _nTotal: -1 };
}

/**
 * @param {{engine:object|null, track:number, chains:object|null,
 *          selected:number, trackName:string}} opts
 *
 * `chains` is the sidecar's accumulated per-track state, keyed by track id.
 * A track absent from it is a track the engine has never published a chain for
 * — which today is every track until something changes or somebody asks. That
 * is a different thing from a track with no devices, and the two must not look
 * the same.
 */
export function buildChainModel(opts, buf) {
  const { track = 0, chains = null, selected = -1, trackName = '', params = null,
          /*
           * The sampler kits, keyed like `params` — see `paramKey`.
           *
           * A sampler has no plugin parameters at all, so its card was a title and an empty
           * body: the engine had a whole instrument in it and the rack said nothing. Its SLOTS
           * are what a kit grid is, and they fit the parameter rows exactly — a name, a value
           * and no bar — so they ride the same virtualized ring rather than needing a surface
           * of their own.
           */
          kits = null,
          // Which device the published patcher graph belongs to, and what is in
          // it. A generator node emits notes of its own, and until now nothing
          // anywhere said so — see `generators` below.
          patcherDevice = -1,
          // The published patcher POOL, so each card can name the generators in
          // its OWN subgraph. `generators` — one phrase for the whole song — is
          // what this replaces; see the card's `sub` below.
          patcherNodes = null, patcherEdges = null, patcherVersion = -1,
          // v24 per-insert meters, as the wire decoded them. Read by TRACK ID and
          // DEVICE ID below, never by slot or by position — both of those diverge
          // from the ids the moment anything is removed.
          meters = null, meterCount = 0, meterTrack = -1 } = opts;
  const entry = chains ? chains[track] : null;

  buf.track = track;
  buf.trackName = trackName;
  buf.version = entry ? entry.version : -1;
  /*
   * WHAT MODULATES WHAT on this track, as the engine published it.
   *
   * The array by REFERENCE, not a copy: the sidecar replaces the whole chain entry per
   * track when anything changes, so the same array is the same links, and copying it here
   * would be an allocation per frame for a list nobody mutates.
   *
   * `modVersion` is the engine's own counter for this track's registry, and it is what the
   * parameter memo below keys on — without it a new link would not repaint the rows,
   * because the parameter VALUES had not moved.
   */
  buf.modLinks = (entry && entry.modLinks) || NO_MODS;
  buf.modVersion = (entry && entry.modVersion) || 0;
  buf.known = !!entry;
  buf.cardCount = 0;

  if (!entry) {
    buf.notice = 'the engine publishes a chain only when it changes — '
               + 'press r to ask for this track’s';
    // And forget what the "asking the hosts" line was last built from. Without
    // this, moving the cursor to a track the engine has never published a chain
    // for and back again left the press-r line on screen: the counts were still
    // 1-of-3, so the memo below decided the notice already said so, while the
    // notice had been overwritten in between. Content moved, key stood still —
    // GUIDELINES 2.1 exactly.
    buf._nNamed = -1; buf._nTotal = -1;
    return buf;
  }

  const devices = entry.devices || NO_DEVICES;
  // How much of the rack has actually answered. A blanket "identity only" was
  // right when nothing could answer and is wrong now that some cards have.
  //
  // An index loop, not `for...of`: this runs on every draw, and a for-of over
  // the device list is an iterator object per frame for a count of at most 16.
  let named = 0;
  for (let i = 0; i < devices.length; i++) {
    const dp = params ? params[paramKey(track, devices[i].id)] : null;
    if (dp && dp.name) named++;
  }
  if (!devices.length) {
    buf.notice = 'no devices on this track';
    buf._nNamed = -1; buf._nTotal = -1;
  } else if (named === devices.length) {
    buf.notice = '';
    buf._nNamed = -1; buf._nTotal = -1;
  } else if (buf._nNamed !== named || buf._nTotal !== devices.length) {
    buf._nNamed = named; buf._nTotal = devices.length;
    buf.notice = 'asking the hosts for names and parameters — '
               + named + ' of ' + devices.length;
  }

  const n = Math.min(devices.length, buf.cards.length);
  for (let i = 0; i < n; i++) {
    const d = devices[i];
    const c = buf.cards[i];
    c.id = d.id;
    c.kind = d.kind;
    c.badge = KIND_BADGE[d.kind] || 'DEV';


    /**
     * The device's buses, and — the part that matters — whether we have them ALL.
     *
     * `busCount` is what the engine said was coming; `buses.length` is what has
     * arrived. While those differ the set is INCOMPLETE, and the card says so
     * instead of summarising what it happens to hold. A rack that renders "2 out"
     * from the first two of eight and silently becomes "8 out" a frame later is the
     * draw-then-rearrange this field was added to prevent — and it is worse than
     * saying nothing, because the wrong number is one somebody might act on.
     *
     * Guarded on a key built from the three inputs, so a device whose buses have
     * not moved costs one integer compare per draw rather than a walk of its list.
     */
    const bl = d.buses;
    const bn = bl ? bl.length : 0;
    const want = d.busCount || 0;
    const bKey = (want << 12) | (bn << 4) | (d.busTruncated ? 1 : 0);
    if (c._bKey !== bKey) {
      c._bKey = bKey;
      c.busTruncated = !!d.busTruncated;
      c.busPartial = bn < want;
      if (!want && !bn) {
        c.busText = '';
      } else if (bn < want) {
        c.busText = busPartialText(bn, want);
      } else {
        let ins = 0, outs = 0;
        for (let k = 0; k < bn; k++) { if (bl[k].input) ins++; else outs++; }
        c.busText = busSummary(ins, outs);
      }
    }

    // The plugin's own name once the host has answered, and what it IS until
    // then. Both are true statements; only one of them is the device's name.
    const dp = params ? params[paramKey(track, d.id)] : null;
    const nm = (dp && dp.name) ? dp.name : null;
    c.named = !!nm;
    if (c._tName !== nm || c._tKind !== d.kind || c._tId !== d.id) {
      c._tName = nm; c._tKind = d.kind; c._tId = d.id;
      // No concatenation at all in the named case: the host's string is used as
      // it arrived, and only the fallback builds anything.
      c.title = nm || ((DEVICE_KINDS[d.kind] || ('kind ' + d.kind)) + ' #' + d.id);
    }

    /*
     * A SAMPLER'S ROWS ARE ITS SLOTS.
     *
     * `src` is normally the host's parameter list. For a sampler there is no host and no
     * parameters, so the kit's slots stand in — same shape, same rows, same ring. The identity
     * guard below keys on this array, and a kit answer is replaced wholesale when a new one
     * arrives, so the same array is the same slots and nothing is rebuilt per frame.
     */
    const kit = (kits && d.kind === KIND_SAMPLER) ? kits[paramKey(track, d.id)] : null;
    /*
     * THE SAMPLER'S FILTER, at card level, so the rack can offer a control for it.
     *
     * Read from the first slot: the filter belongs to a MOD SET, and `samplerFilter` with modSet
     * 0 sets every one of them — the gesture a kit wants, one filter for the whole instrument.
     * A kit whose sets genuinely differ is still addressable from the console by naming a set.
     *
     * -1, not 0, when there is no kit: 0 is the OFF filter, a real state the control has to be
     * able to show, so it cannot double as "there is no control here".
     *
     * SET HERE AND NOT INSIDE THE SLOT COPY BELOW. That copy is guarded to run only when the
     * kit ANSWER changes, so a value written inside it is correct on one frame and stale on
     * every other — and paired with a reset at the top of this loop it was -1 on every frame but
     * one. The button appeared for a single frame and then hid itself, which reads as a control
     * that does not work rather than as a value being cleared underneath it.
     */
    c.filterType = kit && kit.slots && kit.slots.length ? (kit.slots[0].filterType | 0) : -1;
    /*
     * THE BANK'S OWN GATE DEFAULT, from the kit answer rather than from a slot.
     *
     * It cannot be inferred from the slots: it SEEDS a slot at mint and then stops mattering, so
     * a bank whose default is "gated" can legitimately hold one-shot slots minted before it was
     * set. Reading slot 0 would report the past rather than the setting.
     *
     * -1 when there is no kit, for the same reason filterType uses -1: 0 is the real "one-shot"
     * state a card has to be able to show, so it cannot double as "there is nothing here".
     */
    c.defaultGate = kit ? (kit.defaultGate | 0) : -1;
    const src = kit && kit.found && kit.slots ? kit.slots
              : ((dp && dp.params) ? dp.params : null);
    const isKit = !!(kit && kit.found && kit.slots && src === kit.slots);
    const total = src ? src.length : 0;
    const shown = Math.min(total, MAX_PARAMS);
    // Grow the pool to what this card needs and never shrink it: the count moves
    // when you click along a rack, and rebuilding the slots on that transition
    // is an allocation per parameter per selection.
    const slots = c.params;
    while (slots.length < shown) slots.push(createParamSlot());
    // Copy the host's answer into the slots ONLY when it is a different answer.
    //
    // Keyed on (the answer array itself, the device it describes, how many of it
    // we take). A host's answer is never edited in place — it arrives from
    // JSON.parse and `deviceParams[id]` is replaced wholesale — so the same
    // array is the same values, and the two extra terms cover the case the array
    // identity cannot: card slot `i` is reused when the rack changes shape, so a
    // key that named only the array would let device B's values stand under
    // device A's name. The key is stored beside the slots it filled, which is
    // what makes that safe — after B has written them the stored key names B.
    //
    // Eight Zebra2s is 2,048 of these, sixty times a second, to write back the
    // numbers already there.
    if (c._pSrc !== src || c._pId !== d.id || c._pCount !== shown
        || c._pMod !== buf.modVersion) {
      c._pSrc = src; c._pId = d.id; c._pCount = shown; c._pMod = buf.modVersion;
    if (isKit) c._pSrc = src;   // keyed on the kit array, same as a parameter list
      for (let k = 0; k < shown; k++) {
        const q = src[k];
        const p = slots[k];
        if (isKit) {
          /*
           * ONE SLOT, AS A ROW. Built once per kit answer, not per frame — the guard above is
           * what makes that true, and it is the same guard the parameter path uses.
           *
           * The NAME is the slot number and the key it answers to, because that is how a person
           * finds a pad. `lengthFrames === 0` means the source did not resolve, and the region
           * publishes a source-missing flag precisely so a UI can say WHICH kind of silent it
           * is — so it says.
           */
          const missing = (q.flags & 4) !== 0;
          p.index = q.slot;
          p.uid = '';
          p.name = q.keyLow === q.keyHigh
            ? `${q.slot}  ${pitchName(q.root)}`
            : `${q.slot}  ${pitchName(q.keyLow)}-${pitchName(q.keyHigh)}`;
          p.unit = '';
          p.steps = 0;
          // A slot is not a parameter: nothing modulates it, so the row must not offer a badge
          // that would sit over an inert link.
          p.automatable = false;
          /*
           * `isSlot` so the row is not annotated as a PARAMETER. Marking it un-automatable is
           * what hides the modulation badge — correct, nothing modulates a slot — but the title
           * builder turns that same flag into the words "not automatable", which reads as a
           * limitation of this slot rather than as a category error. It is the second consumer
           * of one flag wanting different things, so it gets its own.
           */
          p.isSlot = true;
          p.defaultValue = 0;
          const mod = modSummary(q.modMask | 0, q.filterType | 0);
          /*
           * `lengthFrames` IS THE SOURCE'S LENGTH, NOT THE SLOT'S.
           *
           * `e.lengthFrames = audio->frames` at the publish site, so every slot of a chop
           * reports the whole file — eight slices of one break all say 352800 frames. The slice
           * EXTENT is derived at note-on from the marker and is not published at all.
           *
           * So a sliced slot says which slice it is and how long its SOURCE is, labelled as the
           * source. Presenting that number as the slice's length would be a plain lie, and the
           * lie would be invisible: every slot of a chop would agree with every other, which
           * looks exactly like a correct answer.
           */
          p.range = missing ? 'the source file did not resolve — this slot is silent'
                  : [q.slice ? `slice ${q.slice} of a ${q.frames}-frame source`
                             : `${q.frames} frames`,
                     mod.title].filter(Boolean).join(' · ');
          p.display = missing ? 'MISSING'
                    : (q.slice ? `sl${q.slice}` : frameText(q.frames)) + mod.mark;
          p.value = 0;
          p.mod = -1;
          p.modDepth = 0;
          continue;
        }
        // The engine's index, kept because the wire orders by it — but the
        // command that eventually writes a value should carry `uid` too; the
        // region's own comment says the index is for ordering and the uid is
        // the identity.
        p.index = q.index === undefined ? k : q.index;
        p.uid = q.uid || '';
        p.name = q.name;
        p.unit = q.unit || '';
        p.steps = q.steps || 0;
        // Absent means TRUE. A plugin that publishes nothing about automatability is not saying
        // "no" — and defaulting to no would hide the lane on every plugin that predates the field.
        p.automatable = q.automatable !== false;
        p.isSlot = false;
        p.defaultValue = typeof q.default === 'number' ? q.default : 0;
        /*
         * The range, built ONCE per parameter rather than per frame, and only when both ends have
         * text. A half-range ("-60.0 dB .. ") is worse than none: it reads as a plugin that
         * failed to answer rather than as one that was never asked.
         */
        p.range = (q.minText && q.maxText) ? `${q.minText} .. ${q.maxText}` : '';
        // ALREADY NORMALISED. The engine publishes milli-units and the bridge
        // divides by 1000 before it reaches here, so dividing again put every
        // bar at a thousandth of its length — all of them empty, next to
        // values that were right. A bar that is always zero beside a correct
        // number is worse than no bar: it reads as "this parameter is off".
        const frac = q.value < 0 ? 0 : (q.value > 1 ? 1 : q.value);
        p.frac = frac;
        p.milli = Math.round(frac * 1000);
        // The host's own display string when it has one — it knows whether a
        // value is dB, Hz or a note name, and this side does not. Only the
        // fallback formats, and only when the number it formats has moved.
        if (q.display) {
          p.display = q.display;
          p._dv = NaN;
        } else if (p._dv !== q.value) {
          p._dv = q.value;
          p.display = q.value.toFixed(2);
        }
        /*
         * ...and whether anything MOVES it.
         *
         * A linear scan, inside a block that only runs when the values, the device or the
         * mod version changed — so it is not per frame. A Map would be the reflex and
         * would be wrong here: building one allocates, and a track carries a handful of
         * links, not hundreds.
         *
         * Only VST-param targets match. A link whose target is a patcher parameter has a
         * uid16 of nothing, and `p.uid` for a plugin without a stable id is also empty —
         * so an empty-equals-empty comparison would light every unnamed row for every
         * patcher link. Both sides must be non-empty to match.
         */
        p.mod = 0; p.modDepth = 0; p.modInert = false;
        for (let m = 0; m < buf.modLinks.length; m++) {
          const L = buf.modLinks[m];
          if (L.targetDevice !== d.id) continue;
          if (p.uid && L.uid16 && L.uid16 === p.uid) {
            p.mod = L.id; p.modDepth = L.depth; break;
          }
          /*
           * A LINK WITH NO uid16 IS INERT, and it is shown as such rather than as working.
           *
           * The engine's block-rate applier builds its ParamPayload from `uid16` and never
           * reads `targetId`, so a VstParam link without one is accepted, published,
           * drawable — and moves nothing. Projects can hold these, and so can a session
           * whose naming command was lost.
           *
           * Matched on `targetId` only for THIS case, because it is the only thing such a
           * link carries that says which row it meant. Not used as the general match: the
           * uid is the identity and the index is the ordering, and a plugin that publishes
           * a sparse set would light the wrong row.
           */
          if (!L.uid16 && L.targetId === p.index) {
            p.mod = L.id; p.modDepth = L.depth; p.modInert = true; break;
          }
        }
      }
    }
    c.paramCount = shown;
    // Honest about what was dropped. With the list scrollable nothing is hidden
    // by the CARD any more, so this can only fire when the ENGINE truncated —
    // its region holds 256 — and saying "+N more" then is the difference between
    // a rack that is complete and one that looks it.
    const moreCount = total - shown;
    if (c._mCount !== moreCount) {
      c._mCount = moreCount;
      c.more = moreCount > 0 ? '+' + moreCount + ' more' : '';
    }

    /*
     * THE DEVICE'S METERS, FOUND BY ID.
     *
     * A linear scan of a list that is a handful long, once per card, rather than
     * a Map rebuilt every frame: building the map would allocate more than the
     * scan costs, and this runs at 120 Hz. Matching on `device` and not on
     * position is the contract — the engine's compacted insert order skips
     * patcher devices, so the Nth meter is not the Nth card, and indexing by
     * position paints one device's level on another's face.
     */
    c.hasMeter = false;
    if (meters && meterTrack >= 0) {
      for (let m = 0; m < meterCount; m++) {
        const e = meters[m];
        if (e.track !== meterTrack || e.device !== d.id) continue;
        c.hasMeter = true;
        c.outRms = meterScale(e.outRms);
        c.outPeak = meterScale(e.outPeak);
        c.inRms = meterScale(e.inRms);
        c.inPeak = meterScale(e.inPeak);
        /*
         * WHETHER AN INPUT METER BELONGS HERE AT ALL, from what the device IS
         * rather than from what it currently reads.
         *
         * An instrument has no audio input and honestly reports its input as
         * silent forever, so keying on "is the input silent right now" would
         * draw an input meter on an effect and then blank it during a rest —
         * a meter that disappears when the music stops. A device that processes
         * audio gets both, which is the whole gain-staging comparison; one that
         * only makes it gets an output.
         */
        c.hasIn = (d.caps & CAP_AUDIO) !== 0 && d.kind !== KIND_VST_INSTRUMENT;
        if (c._mOut !== e.outPeak) {
          c._mOut = e.outPeak;
          c.meterText = meterDb(e.outPeak);
        }
        break;
      }
    }

    c.pos = d.pos;
    c.patcherNode = d.node === DEVICE_ID_AUTO ? -1 : d.node;
    c.caps = describeCaps(d.caps);
    c.bypass = !!d.bypass;
    c.selected = i === selected;
    // The POOL VERSION is part of the key, for the reason the generator phrase
    // used to be: it can change while the slot, node and parameter count all stay
    // put — somebody edits a graph — and that is GUIDELINES 2.1 exactly, content
    // moving under a key that did not. The device's own root node is in the key
    // already, so those two together are everything the phrase is built from.
    if (c._sSlot !== d.slot || c._sNode !== c.patcherNode
        || c._sCount !== shown || c._sMore !== moreCount
        || c._sGen !== patcherVersion) {
      c._sSlot = d.slot; c._sNode = c.patcherNode;
      c._sCount = shown; c._sMore = moreCount; c._sGen = patcherVersion;
      c.sub = d.slot === HOST_SLOT_DIRECT
        ? 'in-process'
        : (d.slot === DEVICE_ID_AUTO ? 'slot unassigned' : 'slot ' + d.slot);
      if (c.patcherNode >= 0) c.sub += ' · node ' + c.patcherNode;
      /**
       * SAY WHEN A DEVICE IS MAKING ITS OWN NOTES.
       *
       * Jaakko spent an evening hearing notes that were not in his clip, and I
       * spent it blaming note-off and then MIDI routing. They were a euclidean
       * generator and a random_degree node in this device's patcher graph,
       * playing alongside the sequencer. Everything was working; nothing said it
       * was happening.
       *
       * The graph was reachable the whole time — F3, if you knew — and the fix
       * is not more places to look, it is one line on the card that owns it.
       */
      /*
       * THE DEVICE'S OWN BIT, not a guess at which device owns the graph.
       *
       * This used to read `d.id === patcherDevice`, where `patcherDevice` came
       * from the published patcher region — a field the engine never writes, so
       * it was always 0. A generator on slot 3 was therefore reported on slot 0,
       * and on a track whose slot 0 is an instrument it was reported on the
       * instrument. In `maximal` that put "generates: euclidean + random" on
       * Zebra2 and left the patcher device beside it saying nothing.
       *
       * The engine now sets a per-device bit on the chain snapshot
       * (UI_CHAIN_DIFF_GENERATES). One device, one truth, no inference.
       */
      /*
       * WHAT this device generates, from ITS OWN subgraph.
       *
       * The per-device bit (UI_CHAIN_DIFF_GENERATES) says whether; it does not say
       * what. The phrase used to come from the whole published POOL, so a device
       * whose graph is a plain passthrough read "generates: euclidean + random"
       * because some other track had a euclidean in the pool. One device, one
       * truth — the same correction the bit itself was for, one layer up.
       *
       * Memoised on the device's root node and the pool's version: the answer
       * changes when a graph changes and never otherwise, and this runs per card
       * per frame.
       */
      if (d.generates && patcherNodes) {
        // No memo of its own: the block this sits in only runs when the key above
        // changes, and that key already carries both inputs. Two caches where one
        // does the work is how the next person debugs the inert one.
        const what = generatorsFrom(patcherNodes, patcherEdges || EMPTY_EDGES,
                                    c.patcherNode, _genMask);
        if (what) c.sub += ' · generates: ' + what;
      }
      // The count belongs on screen, not just in the scrollbar: six rows of 256
      // with a thin scrollbar reads as "this plugin has six parameters" unless
      // something says otherwise. This is the something.
      if (shown) c.sub += ' · ' + shown + ' params';
      if (moreCount > 0) c.sub += ' · ' + c.more;
    }
    buf.cardCount++;
  }

  /*
   * WHAT IS FLOWING BETWEEN THE CARDS.
   *
   * After the loop, because it is a property of the CHAIN and not of any one
   * device: what reaches a card depends on everything upstream of it.
   *
   * Each card carries the gap BEFORE it, and the last card carries the gap after
   * it as well. That is what lets the renderer draw n+1 gaps with no elements of
   * its own — two pseudo-elements and a class, rather than a pooled gutter to
   * keep in step with a pooled card.
   */
  const gaps = resolveFlow(devices, buf._flow || (buf._flow = []));
  for (let i = 0; i < buf.cardCount; i++) {
    buf.cards[i].flowIn = flowClass(gaps[i]);
    buf.cards[i].flowOut = i === buf.cardCount - 1 ? flowClass(gaps[i + 1]) : '';
  }
  return buf;
}

/** A gap as the one word the renderer toggles. Interned; there are four. */
function flowClass(g) {
  if (!g || g.dead) return 'dead';
  if (g.midi && g.audio) return 'both';
  return g.audio ? 'audio' : 'midi';
}

// ---------------------------------------------------------------------------
// Parameter edits in flight.
//
// A parameter belongs to the PLUGIN, not to this side: the only value that is
// true is the one its host reports. So an edit is held here, apart from the
// view-model, for exactly as long as it takes the engine to agree — and if the
// engine does not agree, it is dropped and the bar goes back to where the plugin
// says it is. That is the whole reason this is not simply "write the new value
// into the model": a model that has adopted a value nobody accepted is a
// confident control over a guess (GUIDELINES 4.5).
//
// A flat array rather than a Map: it is read on every frame while a drag is
// live, and `for (const x of map)` allocates an iterator every time. There are
// never more of these than fingers.
// ---------------------------------------------------------------------------

/** How long an edit waits for the engine before it is treated as refused. */
export const EDIT_HOLD_MS = 1200;

export function createParamEdits(cap = 8) {
  const slots = new Array(cap);
  for (let i = 0; i < cap; i++) {
    slots[i] = { device: -1, index: -1, milli: 0, at: 0 };
  }
  return { slots, count: 0, cap };
}

/** The pending edit for one parameter, or null. */
export function findParamEdit(edits, device, index) {
  for (let i = 0; i < edits.count; i++) {
    const s = edits.slots[i];
    if (s.device === device && s.index === index) return s;
  }
  return null;
}

/**
 * Record (or move) an optimistic value. Returns the slot, or null when there is
 * no room — which is a refusal, not a silent drop: eight parameters cannot be
 * mid-flight at once from one pointer, so a ninth means something is leaking.
 */
export function setParamEdit(edits, device, index, milli, now) {
  const found = findParamEdit(edits, device, index);
  if (found) { found.milli = milli; found.at = now; return found; }
  if (edits.count >= edits.cap) return null;
  const s = edits.slots[edits.count++];
  s.device = device; s.index = index; s.milli = milli; s.at = now;
  return s;
}

export function dropParamEdit(edits, slot) {
  for (let i = 0; i < edits.count; i++) {
    if (edits.slots[i] !== slot) continue;
    // Swap with the last live slot rather than splice: the array is the pool.
    const last = edits.slots[edits.count - 1];
    edits.slots[i] = last;
    edits.slots[edits.count - 1] = slot;
    edits.count--;
    return true;
  }
  return false;
}

/**
 * Settle what the engine has answered and expire what it has not.
 *
 * `engineMilli(device, index)` returns the published value, or -1 when the
 * parameter is not on screen at all. `held` is the one the pointer is still on,
 * which never expires — a person resting mid-drag has not been refused.
 *
 * Returns the number of edits that expired, so the caller can say so. An edit
 * that SETTLED is dropped silently: that is just the value arriving.
 */
export function reapParamEdits(edits, engineMilli, now, held, holdMs = EDIT_HOLD_MS) {
  let expired = 0;
  for (let i = edits.count - 1; i >= 0; i--) {
    const s = edits.slots[i];
    const v = engineMilli(s.device, s.index);
    // Within a milli-unit is the same value: the engine rounds to the same grid
    // this does, but a plugin is free to quantise a parameter to its own steps.
    if (v >= 0 && Math.abs(v - s.milli) <= 1) { dropParamEdit(edits, s); continue; }
    if (s === held) continue;
    if (now - s.at > holdMs) { dropParamEdit(edits, s); expired++; }
  }
  return expired;
}
