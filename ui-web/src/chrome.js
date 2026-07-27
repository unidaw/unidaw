// The transport bar, from the redesign's top strip.
//
// Structure and content follow Uni.dc.html; every value comes from tokens.css.
// Built once and mutated in place — the position readout updates at the engine's
// publish rate, so rebuilding it would allocate a string and a Text node ~86
// times a second, which is exactly what GUIDELINES 3.1 and 3.2 forbid.

const NANOTICKS_PER_QUARTER = 960000;
const BEATS_PER_BAR = 4;

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
  const posLabel = label('ch-pos', '1:1:000');
  const tempo = label('ch-meta', '120.00 BPM');
  const sig = label('ch-meta', '4/4');
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
  entry.append(viewLabel, octLabel, stepLabel, velLabel);

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

  return {
    /** Called from the draw loop. Must stay allocation-free when nothing moves. */
    update({ playheadTick, transport: tstate, linkText, octave, editStep,
             velocity = 100, rejectText = '', viewName = '', keyName = '' }) {
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
        const bar = Math.floor(beat / BEATS_PER_BAR) + 1;
        const inBar = (beat % BEATS_PER_BAR) + 1;
        const sub = Math.floor((playheadTick % NANOTICKS_PER_QUARTER) / (NANOTICKS_PER_QUARTER / 1000));
        posLabel.firstChild.nodeValue =
          bar + ':' + inBar + ':' + (sub < 10 ? '00' : sub < 100 ? '0' : '') + sub;
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
