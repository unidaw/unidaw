// The transport bar, from the redesign's top strip.
//
// Structure and content follow Uni.dc.html; every value comes from tokens.css.
// Built once and mutated in place — the position readout updates at the engine's
// publish rate, so rebuilding it would allocate a string and a Text node ~86
// times a second, which is exactly what GUIDELINES 3.1 and 3.2 forbid.

import { DEFAULT_METER, meterText } from './meter.js';

const NANOTICKS_PER_QUARTER = 960000;
const BEATS_PER_BAR = 4;
const NANOTICKS_PER_SUB = NANOTICKS_PER_QUARTER / 1000;

/**
 * Every sub-beat field the readout can show, interned at load.
 *
 * The third field of "12:3:487" is the one value on this bar that genuinely
 * changes every frame: it is thousandths of a quarter note, so at 120 BPM it
 * advances ~2,000 times a second and no guard can stop it moving. What a guard
 * CAN stop is it costing a string — the domain is 0..999 and bounded by the
 * modulo that produces it, so a thousand three-character strings built once is
 * the whole cost, instead of one string per frame for the life of the session.
 */
const SUB = new Array(1000);
for (let i = 0; i < 1000; i++) SUB[i] = (i < 10 ? '00' : i < 100 ? '0' : '') + i;

/** Two digits after the point, for the tempo. Same reasoning as SUB: a closed
 *  domain, built once, rather than a `toFixed(2)` per change. */
const HUNDREDTHS = new Array(100);
for (let i = 0; i < 100; i++) HUNDREDTHS[i] = (i < 10 ? '0' : '') + i;

/** One label whose text is written through an owned Text node. */
function label(cls, initial = '') {
  const el = document.createElement('span');
  el.className = cls;
  el.appendChild(document.createTextNode(initial));
  return el;
}

function button(cls, iconClass, title) {
  const b = document.createElement('button');
  b.className = 'ch-btn ' + cls;
  b.title = title;
  const i = document.createElement('i');
  i.className = iconClass;
  b.appendChild(i);
  return b;
}

/**
 * The surfaces, in the order the design's mode buttons list them, with the
 * function key each answers to.
 *
 * Tab cycling was the ONLY way to change surface and nothing on screen said so —
 * a user pressed every function key, found nothing, and discovered Tab by
 * accident. A mode row is not decoration; it is the answer to "what else is
 * there", which an application has to answer without being asked.
 */
export const VIEW_TABS = [
  { view: 'tracker', label: 'TRACKER', key: 'F1' },
  { view: 'arrange', label: 'ARRANGE', key: 'F2' },
  { view: 'patcher', label: 'PATCHER', key: 'F3' },
  { view: 'piano', label: 'SCALE ROLL', key: 'F4' },
  { view: 'mixer', label: 'MIXER', key: 'F8' },
];

export function createChrome(host, { onPlay, onStop, onScales, onView } = {}) {
  host.className = 'chrome';

  const brand = document.createElement('div');
  brand.className = 'ch-brand';
  const name = label('ch-name', 'UNI');
  const ver = label('ch-ver', '0.4·DEV');
  brand.append(name, ver);

  const transport = document.createElement('div');
  transport.className = 'ch-group';
  const play = button('ch-play', 'ph ph-play', 'Play / pause');
  const stop = button('', 'ph ph-stop', 'Stop');
  transport.append(play, stop);

  const pos = document.createElement('div');
  pos.className = 'ch-group';
  // TWO Text nodes, not one. "bar:beat:" moves a couple of times a second and
  // the sub-beat moves every frame, so one node means building the whole string
  // at the rate of its fastest field — the per-frame string factory this bar was
  // before. Split, the frame's write is `posSub.nodeValue = SUB[n]`: a table
  // lookup and an assignment, allocating nothing. `.ch-pos` still reads
  // "1:1:000" to anything asking for its text, since textContent joins them.
  const posLabel = label('ch-pos', '1:1:');
  const posSub = document.createTextNode('000');
  posLabel.appendChild(posSub);
  const tempo = label('ch-meta', '120.00 BPM');
  // Seeded to what the store seeds tempoMilliBpm to, so the first update is a
  // no-op rather than a rewrite of the same string.
  let lastTempo = 120000, lastTempoVaries = false;
  // The song's time signature. It said '4/4' unconditionally, which was true of
  // every project only because no project could say otherwise — the model has no
  // song meter yet. Now it prints what it is given, so the day one arrives this is
  // already honest. See meter.js.
  const sig = label('ch-meta', meterText(DEFAULT_METER));
  let lastMeterKey = -1;
  pos.append(posLabel, tempo, sig);

  // Entry state. A tracker where you cannot see the octave you are typing into
  // is a tracker that writes the wrong notes silently.
  // Which surface is showing — now a row of buttons, one per surface.
  const viewLabel = label('ch-view', 'TRACKER');
  const tabs = document.createElement('div');
  tabs.className = 'ch-tabs';
  const tabEls = [];
  for (const t of VIEW_TABS) {
    const b = document.createElement('button');
    b.className = 'ch-tab';
    b.type = 'button';
    b.dataset.view = t.view;
    b.append(document.createTextNode(t.label));
    const fk = document.createElement('span');
    fk.className = 'ch-tab-key';
    fk.appendChild(document.createTextNode(t.key));
    b.appendChild(fk);
    // A control that does nothing is worse than no control, so it only binds
    // when the host gave it somewhere to go.
    if (onView) b.addEventListener('pointerdown', () => onView(t.view));
    b._on = null;
    tabs.appendChild(b);
    tabEls.push(b);
  }

  const entry = document.createElement('div');
  entry.className = 'ch-group';
  const octLabel = label('ch-meta', 'oct 4');
  const stepLabel = label('ch-meta', 'step 1');
  const velLabel = label('ch-meta', 'vel 100');
  // What a bare digit writes in the column the cursor is in. The three columns
  // read digits differently — degree, velocity, effect — and nothing said which,
  // so one keystroke did three things and you had to remember which. It sits with
  // the other entry state because that is what it is: a fact about what typing
  // will do, next to the octave and the step it will do it at.
  const digitMode = label('ch-mode', '');
  let lastDigitMode = null;
  /**
   * The two MODES a keystroke passes through, shown because a mode you cannot
   * see is a mode you cannot trust.
   *
   * `f` has toggled playhead follow since it was written and nothing on screen
   * ever said which way it was set, so pressing it was a coin flip you resolved
   * by watching whether the view moved. Edit mode has the same problem and worse
   * consequences: the difference between a keyboard that plays and one that
   * writes is not something to discover by typing.
   *
   * Written as the mode that is ON, not as a label plus a value — "EDIT" and
   * "FOLLOW" read at a glance where "edit: off" has to be parsed.
   */
  const editMode = label('ch-mode', '');
  const followMode = label('ch-mode', '');
  let lastEdit = null, lastFollow = null;
  entry.append(viewLabel, octLabel, stepLabel, velLabel, digitMode, editMode, followMode);

  const scales = document.createElement('button');
  scales.className = 'ch-btn ch-scales';
  scales.title = 'Scale browser';
  const scaleIcon = document.createElement('i');
  scaleIcon.className = 'ph ph-circles-three';
  const scaleLabel = label('ch-scale', '—');
  const scaleKey = label('ch-key', '⌘⇧S');
  scales.append(scaleIcon, scaleLabel, scaleKey);

  // Why the last edit was refused. An edit that is silently dropped is
  // indistinguishable from one that was accepted, which is the whole reason this
  // exists rather than a console warning.
  const reject = label('ch-reject', '');

  const right = document.createElement('div');
  right.className = 'ch-right';
  const link = label('ch-link', 'connecting');
  right.append(reject, link, scales);

  host.append(brand, transport, pos, entry, tabs, right);

  if (onPlay) play.addEventListener('click', onPlay);
  if (onStop) stop.addEventListener('click', onStop);
  if (onScales) scales.addEventListener('click', onScales);

  // Cached scalars, so a write only happens when the value actually changes.
  let lastTick = -1, lastTransport = -1, lastLink = '', lastOct = -1, lastStep = -1, lastVel = -1, lastReject = '', lastView = '', lastKey = '';
  // The readout's two halves, cached separately because they move at different
  // rates. `lastBeat` is the absolute beat index, which is the ONLY thing the
  // bar and the beat-in-bar are computed from — a tick that moves within a beat
  // cannot change what that half says, so it is a sufficient key for it.
  let lastBeat = -1, lastSub = -1;

  return {
    /** Called from the draw loop. Must stay allocation-free when nothing moves. */
    update({ playheadTick, transport: tstate, linkText, octave, editStep,
             velocity = 100, rejectText = '', viewName = '', keyName = '',
             tempoMilliBpm = 120000, tempoPointCount = 0, meter = DEFAULT_METER,
             digitMode: digitModeText = '',
             editMode: editOn = true, followPlayhead: followOn = false }) {
      if (digitModeText !== lastDigitMode) {
        lastDigitMode = digitModeText;
        digitMode.firstChild.nodeValue = digitModeText;
      }
      // Both guarded on the boolean, and both write the mode that is ON rather
      // than a label and a value: "EDIT" reads at a glance where "edit: off" has
      // to be parsed, and these sit next to five other fields.
      if (editOn !== lastEdit) {
        lastEdit = editOn;
        editMode.firstChild.nodeValue = editOn ? 'EDIT' : 'jam';
        editMode.classList.toggle('off', !editOn);
      }
      if (followOn !== lastFollow) {
        lastFollow = followOn;
        followMode.firstChild.nodeValue = followOn ? 'follow' : '';
      }
      // Guarded on the two numbers, not on the record: the engine will republish a
      // meter on every frame, and a guard keyed on object identity would rebuild
      // this string sixty times a second to print the same thing.
      const meterKey = meter.numerator * 64 + meter.denominator;
      if (meterKey !== lastMeterKey) {
        lastMeterKey = meterKey;
        sig.firstChild.nodeValue = meterText(meter);
      }
      /**
       * The project's tempo, at the playhead.
       *
       * This label read a hardcoded "120.00 BPM" from the day it was written,
       * through every project that was not 120 and every tempo change inside
       * one. The engine publishes it now, as an integer in thousandths, and
       * that integer is what the guard compares — a float would jitter in its
       * last digit and rebuild the string every frame to print the same number.
       *
       * The dot after the number means the song's tempo VARIES and this is
       * merely the tempo here. Without it, scrolling the playhead across a
       * change looks like the readout glitching rather than like the music
       * doing what it was written to do.
       */
      if (tempoMilliBpm !== lastTempo || (tempoPointCount > 1) !== lastTempoVaries) {
        lastTempo = tempoMilliBpm;
        lastTempoVaries = tempoPointCount > 1;
        const whole = (tempoMilliBpm / 1000) | 0;
        const frac = tempoMilliBpm % 1000;
        tempo.firstChild.nodeValue =
          whole + '.' + HUNDREDTHS[(frac / 10) | 0] + ' BPM' + (lastTempoVaries ? ' ·' : '');
      }
      if (velocity !== lastVel) { lastVel = velocity; velLabel.firstChild.nodeValue = 'vel ' + velocity; }
      // The key CHANGES: it is resolved from the harmony timeline at the
      // playhead, so it moves during playback rather than being a caption. A
      // dash means the engine has published no timeline, which is a different
      // thing from a project genuinely having no harmony.
      if (keyName !== lastKey) { lastKey = keyName; scaleLabel.firstChild.nodeValue = keyName || '—'; }
      if (viewName !== lastView) {
        lastView = viewName;
        viewLabel.firstChild.nodeValue = viewName.toUpperCase();
        for (const b of tabEls) {
          const on = b.dataset.view === viewName;
          if (b._on !== on) { b._on = on; b.classList.toggle('on', on); }
        }
      }
      if (rejectText !== lastReject) {
        lastReject = rejectText;
        reject.firstChild.nodeValue = rejectText;
        reject.classList.toggle('on', !!rejectText);
      }
      if (octave !== lastOct) { lastOct = octave; octLabel.firstChild.nodeValue = 'oct ' + octave; }
      if (editStep !== lastStep) { lastStep = editStep; stepLabel.firstChild.nodeValue = 'step ' + editStep; }
      if (playheadTick !== lastTick) {
        lastTick = playheadTick;
        const beat = Math.floor(playheadTick / NANOTICKS_PER_QUARTER);
        // A beat is 500 ms at 120 BPM, so this concatenation runs about twice a
        // second rather than sixty times — the rate the value it describes
        // actually changes at, which is the point of splitting the node.
        if (beat !== lastBeat) {
          lastBeat = beat;
          posLabel.firstChild.nodeValue =
            (Math.floor(beat / BEATS_PER_BAR) + 1) + ':' + ((beat % BEATS_PER_BAR) + 1) + ':';
        }
        // Bounded 0..999 by the modulo above, so the table always has an entry.
        const sub = Math.floor((playheadTick % NANOTICKS_PER_QUARTER) / NANOTICKS_PER_SUB);
        if (sub !== lastSub) { lastSub = sub; posSub.nodeValue = SUB[sub]; }
      }
      if (tstate !== lastTransport) {
        lastTransport = tstate;
        play.firstChild.className = tstate ? 'ph ph-pause' : 'ph ph-play';
        play.classList.toggle('on', !!tstate);
      }
      if (linkText !== lastLink) {
        lastLink = linkText;
        link.firstChild.nodeValue = linkText;
        link.classList.toggle('live', linkText === 'live');
      }
    },
  };
}
