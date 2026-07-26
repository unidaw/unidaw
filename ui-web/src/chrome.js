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

export function createChrome(host, { onPlay, onStop, onScales } = {}) {
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

  const scales = document.createElement('button');
  scales.className = 'ch-btn ch-scales';
  scales.title = 'Scale browser';
  const scaleIcon = document.createElement('i');
  scaleIcon.className = 'ph ph-circles-three';
  const scaleLabel = label('ch-scale', 'C major');
  const scaleKey = label('ch-key', '⌘⇧S');
  scales.append(scaleIcon, scaleLabel, scaleKey);

  const right = document.createElement('div');
  right.className = 'ch-right';
  const link = label('ch-link', 'connecting');
  right.append(link, scales);

  host.append(brand, transport, pos, right);

  if (onPlay) play.addEventListener('click', onPlay);
  if (onStop) stop.addEventListener('click', onStop);
  if (onScales) scales.addEventListener('click', onScales);

  // Cached scalars, so a write only happens when the value actually changes.
  let lastTick = -1, lastTransport = -1, lastLink = '';

  return {
    /** Called from the draw loop. Must stay allocation-free when nothing moves. */
    update({ playheadTick, transport: tstate, linkText }) {
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
