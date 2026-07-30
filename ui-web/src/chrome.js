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

/**
 * @param {HTMLElement} host the top bar
 * @param {HTMLElement} [secondaryHost] the row below it, for the entry cluster
 *
 * WHY THE ENTRY CLUSTER GOES SOMEWHERE ELSE. Measured: the top bar's cells came to 1583px in
 * a 1680px window once the design's loop chip, telemetry and saved chip were in it, and the
 * entry cluster is 348 of those — so something had to leave, and the design's own answer is
 * that entry state is not top-bar material at all (it has none there, in either view).
 *
 * The row below is where it goes rather than the design's bottom status bar, because that bar
 * does not exist yet and building it is its own piece. This keeps the state VISIBLE — a
 * tracker where you cannot see the octave you are typing into writes the wrong notes silently
 * — while taking it out of a bar it no longer fits in. Same elements, same guards, mounted one
 * row down; nothing about how they update changes.
 *
 * Without a secondary host it stays in the top bar, which is what the fixtures do — and they
 * are narrower than 1680, so this must not depend on the room being there.
 */
export function createChrome(host, { onPlay, onStop, onScales, onView,
                                     onAddTrack, onRemoveTrack,
                                     secondaryHost = null } = {}) {
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
  /*
   * THE THIRD BUTTON, and it is DISABLED on purpose.
   *
   * The design has three and the engine has no record command at all — nothing on the
   * command enumeration arms a take. Drawing it live would be the worst version: a button
   * that looks like every other one and does nothing, which is the failure this file has
   * shipped three times (splitter.js unbuilt, delDevice unreachable, onNav never passed).
   *
   * Drawn and marked unavailable instead, with the reason in its title. That is a different
   * claim from an ordinary button — it says "this belongs here and is not built" — and it is
   * the honest one while the capability is a backend request.
   */
  const rec = button('ch-rec', 'ph ph-record', 'Record — the engine has no record command yet');
  rec.disabled = true;
  rec.classList.add('unavailable');
  transport.append(play, stop, rec);

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
  /*
   * THE GROOVE, which is the cursor track's lane SWING and not a separate setting.
   *
   * On the second line beside the meter, as the design has it. Swing is the one quantize
   * field whose effect is a feel rather than a position, and it was previously visible only
   * on the lane badge in the tracker — so in the arrangement or the mixer there was no way
   * to see that the track you were listening to was swung.
   *
   * Absent, not zero, when the lane is straight: "groove 0%" is a setting and no groove is
   * the absence of one, and printing the first for the second fills the bar with noise.
   */
  const groove = label('ch-meta ch-groove', '');
  let lastGroove = -2;
  // Two lines, in one cell. `ch-stack` puts the tempo above the meter, which is the design's
  // arrangement and buys the horizontal room the telemetry needs.
  const posMeta = document.createElement('div');
  posMeta.className = 'ch-stack';
  const posMeta2 = document.createElement('div');
  posMeta2.className = 'ch-stack-row';
  posMeta2.append(sig, groove);
  posMeta.append(tempo, posMeta2);
  pos.append(posLabel, posMeta);

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
  /**
   * "The patcher is making notes."
   *
   * The single most expensive missing sentence in this application. A patcher
   * graph that emits events plays them through whatever instrument the track
   * has, alongside whatever is written in the clip — so you drop an instrument
   * on a track, hear notes you did not write, and nothing on screen accounts for
   * them. That has been reported as "phantom notes" three times, cost hours each
   * time, and produced three confident wrong diagnoses from me on the first
   * round: a missing note-off, MIDI leaking between tracks, a version race.
   * Every one of them was a story invented to explain a sound with no visible
   * source, and the fix each time was to notice a euclidean node.
   *
   * Deliberately in the CHROME, next to EDIT and follow, rather than on a device
   * card. The badge that already existed hangs off a device, and a track-level
   * generator with an empty device chain therefore showed nothing at all —
   * which is exactly the case that keeps biting. This is the one place a person
   * is already looking to find out what mode the app is in, and "notes are
   * coming from somewhere other than the notes" is a mode.
   *
   * It says the graph, not the track: the engine publishes ONE patcher region
   * ("one global graph today"), so which track is generating is not knowable
   * here yet. Asked for as a per-track flag bit; until then, saying the true
   * thing loudly beats saying the precise thing not at all.
   */
  const genWarn = label('ch-mode ch-gen', '');
  let lastGen = null;
  let lastEdit = null, lastFollow = null;
  /*
   * NO VIEW LABEL HERE. The tabs say which surface is showing, in the accent, and the
   * breadcrumb says it again in words — a third copy in the entry cluster was one more thing
   * competing for a row that is now shared with the breadcrumb. It stays constructed because
   * `update` writes to it and a null would be a fourth branch in a hot path; it is simply not
   * mounted, which is cheaper than making every write conditional.
   */
  entry.append(octLabel, stepLabel, velLabel, digitMode, editMode, followMode, genWarn);

  /*
   * THE LOOP, as a readout of the bars it spans.
   *
   * A READOUT and not a toggle, and the reason is the engine's: a loop is expressed by its
   * range and `SetLoopRange` refuses `end <= start`, so there is no command that means
   * "stop looping". A chip that looked like a toggle and could only ever be switched ON
   * would be worse than one that only reports. Asked backend for a way to clear a loop.
   *
   * Bars, not ticks, because that is what the design says and what a person means by a loop.
   */
  const loopChip = label('ch-chip ch-loop', '');
  /*
   * `null`, NOT `''` — a sentinel no real value can equal.
   *
   * Seeded to the empty string, the first frame's key is also '' so the guard skips, and the
   * `display: none` that HIDES an absent chip is never written. The chip then holds its space
   * and its border for a loop that does not exist, forever, until one is set. Every other
   * guard in this file has the same shape and the same trap: a cache seeded to a value the
   * first frame can produce never applies its initial state.
   */
  let lastLoopKey = null;
  /*
   * THE METRONOME, which the engine does not have.
   *
   * Not a single command anywhere arms a click, so this cannot be wired at all — and the
   * design puts it here, which makes its absence worth SHOWING rather than leaving the bar
   * looking complete. Marked unavailable, with the reason in the title, exactly like the
   * record button.
   */
  const clickChip = label('ch-chip ch-click unavailable', 'CLICK');
  clickChip.title = 'Metronome — the engine has no click yet';

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

  /*
   * THE MACHINE, in the two numbers that are actually knowable.
   *
   * `lat` is `blockSize / sampleRate`, which IS the output latency and has no second answer
   * — and both facts now cross the wire so it is read rather than assumed. `shm` is the
   * document version, which is what tells you an edit landed.
   *
   * THE DESIGN ALSO ASKS FOR `dsp N%` AND `pdc N smp` AND THOSE ARE NOT DRAWN, because the
   * engine publishes neither. A DSP meter reading 0% would say the machine is idle while it
   * struggles, and a PDC of 0 samples would say the compensation is off when Movement 4
   * landed it — both are worse than a gap, and both are backend requests. The bar is short
   * here on purpose and the reason is written down.
   */
  const telemetry = document.createElement('div');
  telemetry.className = 'ch-group ch-telemetry';
  const latLabel = label('ch-meta', '');
  const shmLabel = label('ch-meta', '');
  // Two labels in a flex row with a gap, not two strings in one node: they change at entirely
  // different rates — the latency once per device open, the version on every edit — and one
  // node would rebuild both strings at the rate of the faster one.
  telemetry.append(latLabel, shmLabel);
  let lastLat = -1, lastShm = -1;
  /*
   * WHEN THE PROJECT WAS LAST SAVED, in words.
   *
   * "40s ago" rather than a clock time, because the question a person is asking is how much
   * work is at risk, and that is an interval. It says nothing at all until a save has
   * succeeded — an unsaved session reading "saved never" is noise, and reading "saved 0s
   * ago" would be a lie.
   */
  const savedChip = label('ch-chip ch-saved', '');
  // `null` for the reason `lastLoopKey` gives: seeded to '' the hidden state is never applied.
  let lastSavedText = null;

  const right = document.createElement('div');
  right.className = 'ch-right';
  const link = label('ch-link', 'connecting');
  right.append(reject, link);

  // Track structure. In the chrome rather than in the tracker and the arrange
  // view separately: tracks are a property of the song, not of the surface you
  // happen to be looking at, and one implementation cannot drift from the other.
  // Titled with what they do to the SONG, since "+" alone next to a transport
  // reads as "add a bar" about as easily as "add a track".
  const tracks = document.createElement('div');
  tracks.className = 'ch-group';
  // ph-rows-plus-bottom exists in the bundled set; there is no ph-rows-minus, and
  // naming one would have shipped a button that renders and cannot be seen — the
  // Open-button bug again. ph-minus is the pair that actually exists.
  const addTrk = button('', 'ph ph-rows-plus-bottom', 'Add a track (append)');
  const delTrk = button('', 'ph ph-minus', 'Remove the cursor’s track');
  tracks.append(addTrk, delTrk);

  /*
   * THE ORDER IS THE DESIGN'S.
   *
   * brand · transport · position · loop/click · the harmony chip · entry state · [gap] ·
   * telemetry · saved · tabs. The harmony chip moves from the far right to just after the
   * transport cluster, which is where the design puts it — and that is not decoration: what
   * key the song is in belongs next to what bar you are on, not beside the connection state.
   *
   * The ENTRY cluster stays. The design has no entry state in the top bar at all — it lives
   * in a bottom status bar this build does not have yet — and moving it out before that bar
   * exists would delete the only place the octave you are typing into is visible.
   */
  /*
   * ...AND THE TABS ARE LAST, flush to the bar's right padding, as the design has them. The
   * connection state and the reject line go BEFORE the telemetry rather than after the tabs:
   * `.ch-telemetry` is what takes the free space, so anything after it is pinned to the right
   * edge, and there is only room for one thing to be pinned there.
   */
  host.append(brand, transport, pos, loopChip, clickChip, scales, tracks,
              right, telemetry, savedChip, tabs);
  // The entry cluster, one row down when there is one. `.ch-entry-row` drops the group's left
  // rule, which is a separator between top-bar cells and reads as a stray line on its own.
  if (secondaryHost) {
    entry.classList.add('ch-entry-row');
    secondaryHost.appendChild(entry);
  } else {
    host.insertBefore(entry, tracks);
  }

  if (onPlay) play.addEventListener('click', onPlay);
  if (onStop) stop.addEventListener('click', onStop);
  if (onScales) scales.addEventListener('click', onScales);
  if (onAddTrack) addTrk.addEventListener('click', onAddTrack);
  if (onRemoveTrack) delTrk.addEventListener('click', onRemoveTrack);

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
             digitMode: digitModeText = '', generating = '',
             editMode: editOn = true, followPlayhead: followOn = false,
             // The design's telemetry and its two chips. Every one of them is a fact from
             // the engine and every one is ABSENT rather than defaulted when the engine has
             // not said — see each guard below for why that distinction is load-bearing.
             loopStartBar = 0, loopEndBar = 0, grooveMilli = 0,
             blockSize = 0, sampleRateHz = 0, docVersion = -1, savedAgoSeconds = -1 }) {
      /*
       * THE LOOP, in bars. Absent when there is none — a chip reading "LOOP 1-1" for a loop
       * that does not exist is a claim about the song, not an empty field.
       */
      const loopKey = loopEndBar > loopStartBar ? `${loopStartBar}\u2013${loopEndBar}` : '';
      if (loopKey !== lastLoopKey) {
        lastLoopKey = loopKey;
        loopChip.firstChild.nodeValue = loopKey ? `LOOP ${loopKey}` : '';
        // The chip is hidden rather than emptied, so it takes no width when there is no loop
        // and nothing beside it shifts when one appears... except it does shift, which is why
        // this is a display toggle and not an opacity one: an invisible chip that still holds
        // its space is a permanent gap in a bar that is short of room.
        loopChip.style.display = loopKey ? '' : 'none';
      }
      /*
       * THE GROOVE. Thousandths in, percent out, and ABSENT at zero: a straight lane has no
       * groove, and "groove 0%" is a setting where nothing is the absence of one.
       */
      if (grooveMilli !== lastGroove) {
        lastGroove = grooveMilli;
        // NON-BREAKING spaces round the separator. A leading ordinary space in a text node
        // collapses to nothing against the sibling beside it, so this read "4/4· groove 20%".
        groove.firstChild.nodeValue = grooveMilli
          ? `\u00a0\u00b7\u00a0groove ${Math.round(grooveMilli / 10)}%` : '';
      }
      /*
       * THE LATENCY, derived from the device's own block and rate.
       *
       * Nothing at all until both have arrived. Zero block or zero rate means the engine has
       * not opened a device, and "0.0ms" would report a perfect machine for a machine that
       * has not started — the exact shape of lie this bar exists to avoid.
       */
      const latTenths = (blockSize > 0 && sampleRateHz > 0)
        ? Math.round((blockSize / sampleRateHz) * 10000) : -1;
      if (latTenths !== lastLat) {
        lastLat = latTenths;
        latLabel.firstChild.nodeValue = latTenths < 0 ? ''
          : `${blockSize} smp \u00b7 lat ${(latTenths / 10).toFixed(1)}ms`;
      }
      if (docVersion !== lastShm) {
        lastShm = docVersion;
        shmLabel.firstChild.nodeValue = docVersion < 0 ? '' : `doc v${docVersion}`;
      }
      /*
       * WHEN IT WAS LAST SAVED. Silent until a save has succeeded: a session that has never
       * been saved reading "saved never" is noise, and "saved 0s ago" would be a lie about
       * work that is entirely at risk.
       */
      const savedText = savedAgoSeconds < 0 ? ''
        : savedAgoSeconds < 60 ? `saved ${savedAgoSeconds}s ago`
        : savedAgoSeconds < 3600 ? `saved ${Math.floor(savedAgoSeconds / 60)}m ago`
        : `saved ${Math.floor(savedAgoSeconds / 3600)}h ago`;
      if (savedText !== lastSavedText) {
        lastSavedText = savedText;
        savedChip.firstChild.nodeValue = savedText;
        savedChip.style.display = savedText ? '' : 'none';
      }
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
      // Guarded on the phrase, which changes when a graph loads and never again.
      if (generating !== lastGen) {
        lastGen = generating;
        genWarn.firstChild.nodeValue = generating;
        genWarn.title = generating
          ? 'A patcher graph is emitting notes of its own, on top of what is '
            + 'written in the clips. Press F3 to see it, or `delnode <id>` to remove it.'
          : '';
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
