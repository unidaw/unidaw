// Mixer renderer: one strip per track.
//
// Meters are the one thing here that moves every frame, so they are updated
// unconditionally while everything else is guarded on a cached value. That split
// is deliberate: putting the peak level into the change key would rebind every
// strip at the engine's publish rate and turn a cheap surface into the most
// expensive one in the app.

import { createField, begin as fieldBegin, cancel as fieldCancel,
         feed as fieldFeed, display } from './textfield.js';

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

function textDiv(cls, parent) {
  const el = div(cls, parent);
  el.appendChild(document.createTextNode(''));
  return el;
}

export class Mixer {
  constructor(host, { onGain, onPan, onToggle, onRename } = {}) {
    this.host = host;
    this.host.className = 'mx';
    this.onGain = onGain; this.onPan = onPan; this.onToggle = onToggle;
    this.onRename = onRename;
    /** Which strip is being renamed; the text lives in the shared field. */
    this.renaming = -1;
    this.field = createField({ max: 23 });
    this.stripsEl = div('mx-strips', host);
    // A canvas, deliberately. This is the surface where a real scope goes, and a
    // scope redraws every pixel every frame — no arrangement of pooled divs
    // makes that sensible. See src/scope.js.
    this.scopeCanvas = document.createElement('canvas');
    this.scopeCanvas.className = 'mx-scope';
    host.appendChild(this.scopeCanvas);
    this.notice = textDiv('mx-notice', host);
    this.pool = [];
    this.vm = null;
    this._notice = null;

    // One listener for the whole surface rather than per control: strips are
    // pooled and re-bound, so per-element listeners would have to be rebound too.
    this.host.addEventListener('pointerdown', (e) => this._down(e));
    this.host.addEventListener('pointermove', (e) => this._move(e));
    this.host.addEventListener('pointerup', () => this._up());
    this.host.addEventListener('pointercancel', () => this._up());
    this._drag = null;
  }

  _strip(i) {
    while (this.pool.length <= i) {
      const k = this.pool.length;
      const el = div('mx-strip', this.stripsEl);
      el.dataset.track = String(k);
      const name = textDiv('mx-name', el);
      name.dataset.act = 'name';
      const meterWrap = div('mx-meterwrap', el);
      const fader = div('mx-fader', meterWrap);
      const faderFill = div('mx-fader-fill', fader);
      const meter = div('mx-meter', meterWrap);
      const meterFill = div('mx-meter-fill', meter);
      const gain = textDiv('mx-gain', el);
      const pan = textDiv('mx-pan', el);
      const btns = div('mx-btns', el);
      const mute = textDiv('mx-btn mx-mute', btns);
      mute.firstChild.nodeValue = 'M';
      mute.dataset.act = 'mute';
      const solo = textDiv('mx-btn mx-solo', btns);
      solo.firstChild.nodeValue = 'S';
      solo.dataset.act = 'solo';
      fader.dataset.act = 'fader';
      pan.dataset.act = 'pan';
      el._name = name.firstChild; el._gain = gain.firstChild; el._pan = pan.firstChild;
      el._fader = fader; el._faderFill = faderFill; el._meterFill = meterFill;
      el._mute = mute; el._solo = solo;
      el._nameV = null; el._gainV = null; el._panV = null;
      el._faderV = -1; el._meterV = -1; el._muteV = null; el._soloV = null; el._dimV = null;
      this.pool.push(el);
    }
    return this.pool[i];
  }

  _down(e) {
    const target = e.target.closest('[data-act]');
    if (!target) return;
    const stripEl = e.target.closest('.mx-strip');
    if (!stripEl) return;
    const track = Number(stripEl.dataset.track);
    const act = target.dataset.act;
    if (act === 'mute' || act === 'solo') { this.onToggle && this.onToggle(track, act); return; }
    if (act === 'fader') {
      const r = target.getBoundingClientRect();
      // Bottom of the fader is 0, top is 1 — a fader that grew downward would
      // be a novel and unwelcome invention.
      this._drag = { track, rect: r };
      this.onGain && this.onGain(track, 1 - (e.clientY - r.top) / r.height);
      this.host.setPointerCapture(e.pointerId);
    }
    if (act === 'pan') { this.onPan && this.onPan(track, e.shiftKey ? -1 : 1); }
    if (act === 'name') { this.beginRename(track); }
  }

  /**
   * Continue a fader drag. The rect is captured at pointerdown and reused: the
   * fader's fill changes height as you drag, so re-measuring the element would
   * move the reference frame under the gesture and make the fader accelerate
   * away from the pointer.
   */
  _move(e) {
    if (!this._drag) return;
    const r = this._drag.rect;
    this.onGain && this.onGain(this._drag.track, 1 - (e.clientY - r.top) / r.height);
  }

  _up() { this._drag = null; }

  beginRename(track) {
    this.renaming = track;
    const cur = this.vm && this.vm.strips[track] ? this.vm.strips[track].name : '';
    fieldBegin(this.field, cur);
  }
  cancelRename() { this.renaming = -1; fieldCancel(this.field); }

  /** One shared implementation; see src/textfield.js. */
  feedRename(key) {
    if (this.renaming < 0) return 'ignore';
    const act = fieldFeed(this.field, key);
    if (act === 'commit') {
      const name = this.field.text.trim();
      const track = this.renaming;
      this.renaming = -1;
      fieldCancel(this.field);
      if (name) this.onRename && this.onRename(track, name);
    } else if (act === 'cancel') {
      this.renaming = -1;
    }
    return act;
  }

  render(vm) {
    this.vm = vm;
    for (let i = 0; i < vm.stripCount; i++) {
      const s = vm.strips[i];
      const el = this._strip(i);
      if (el.style.display === 'none') el.style.display = '';
      // While renaming, the strip shows what you are typing rather than what the
      // engine still thinks it is called.
      const shown = i === this.renaming ? display(this.field) : s.name;
      if (el._nameV !== shown) { el._nameV = shown; el._name.nodeValue = shown; }
      const ren = i === this.renaming;
      if (el._renV !== ren) { el._renV = ren; el.classList.toggle('renaming', ren); }
      if (el._gainV !== s.gainDb) { el._gainV = s.gainDb; el._gain.nodeValue = s.gainDb; }
      if (el._panV !== s.panLabel) { el._panV = s.panLabel; el._pan.nodeValue = s.panLabel; }
      if (el._faderV !== s.faderPct) {
        el._faderV = s.faderPct;
        el._faderFill.style.height = (s.faderPct * 100).toFixed(1) + '%';
      }
      if (el._muteV !== s.mute) { el._muteV = s.mute; el._mute.classList.toggle('on', s.mute); }
      if (el._soloV !== s.solo) { el._soloV = s.solo; el._solo.classList.toggle('on', s.solo); }
      if (el._dimV !== s.dimmed) { el._dimV = s.dimmed; el.classList.toggle('dim', s.dimmed); }
      if (el._pendV !== s.pending) { el._pendV = s.pending; el.classList.toggle('pending', s.pending); }
      // Unguarded on purpose — see the header note.
      const pct = (s.peakPct * 100).toFixed(0);
      if (el._meterV !== pct) { el._meterV = pct; el._meterFill.style.height = pct + '%'; }
    }
    for (let i = vm.stripCount; i < this.pool.length; i++) {
      const el = this.pool[i];
      if (el.style.display !== 'none') el.style.display = 'none';
    }

    // The one honest thing this surface can say about itself.
    const note = vm.authoritative
      ? ''
      : 'no engine — faders show local values only';
    if (this._notice !== note) {
      this._notice = note;
      this.notice.firstChild.nodeValue = note;
      this.notice.classList.toggle('on', !!note);
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    const s = [];
    for (let i = 0; i < vm.stripCount; i++) {
      const x = vm.strips[i];
      s.push({ track: x.track, gain: x.gain, db: x.gainDb, pan: x.panLabel,
               mute: x.mute, solo: x.solo, dim: x.dimmed, pending: x.pending,
               fader: +x.faderPct.toFixed(3), meter: +x.peakPct.toFixed(3) });
    }
    return { strips: vm.stripCount, authoritative: vm.authoritative,
             renaming: this.renaming, renameText: this.field.text,
             domNodes: this.pool.length * 12, detail: s };
  }
}
