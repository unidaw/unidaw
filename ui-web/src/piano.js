// Piano roll renderer. Same pooling and guarded-write discipline as the others.

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

export class Piano {
  constructor(host, { onNote, onSelect, onMarquee, onMarqueeEnd, onDrag, onDragEnd } = {}) {
    this.host = host;
    this.host.className = 'pr';
    this.onNote = onNote; this.onSelect = onSelect;
    this.onMarquee = onMarquee; this.onMarqueeEnd = onMarqueeEnd;
    this.onDrag = onDrag; this.onDragEnd = onDragEnd;
    /** How close to a note's right edge counts as "resize" rather than "move". */
    this.EDGE = 7;

    this.keysEl = div('pr-keys', host);
    this.band = div('pr-band', host);
    this.ruler = div('pr-ruler', this.band);
    this.rowsEl = div('pr-rows', this.band);
    this.gridEl = div('pr-grid', this.band);
    this.notesEl = div('pr-notes', this.band);
    this.playhead = div('pr-playhead', this.band);
    this.marquee = div('pr-marquee', this.band);

    this.keyPool = [];
    this.rowPool = [];
    this.gridPool = [];
    this.rulerPool = [];
    this.notePool = [];
    this.vm = null;

    this.band.addEventListener('pointerdown', (e) => this._down(e));
    this.band.addEventListener('pointermove', (e) => this._move(e));
    this.band.addEventListener('pointerup', (e) => this._up(e));
    this.band.addEventListener('pointercancel', () => this._cancel());
    this._drag = null;
  }

  _local(e) {
    const r = this.rowsEl.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  _down(e) {
    const vm = this.vm;
    if (!vm) return;
    const noteEl = e.target.closest('.pr-note');
    if (noteEl) {
      const id = Number(noteEl.dataset.id);
      this.onSelect && this.onSelect(id);
      const r = noteEl.getBoundingClientRect();
      const p = this._local(e);
      // Grabbing the right edge resizes, anywhere else moves. A note narrower
      // than twice the edge zone is all edge, so it can only be resized —
      // otherwise a short note would be impossible to lengthen.
      const kind = (r.right - e.clientX) <= this.EDGE ? 'resize' : 'move';
      this._note = { id, kind, x0: p.x, y0: p.y, x1: p.x, y1: p.y };
      this.band.setPointerCapture(e.pointerId);
      return;
    }
    const p = this._local(e);
    if (p.x < 0 || p.y < 0) return;
    // Shift starts a marquee; a plain click writes a note. Writing on every drag
    // would make selecting impossible, and selecting on every click would make
    // the fastest way to enter notes a modifier chord.
    if (e.shiftKey) {
      this._drag = { x0: p.x, y0: p.y, x1: p.x, y1: p.y };
      this.band.setPointerCapture(e.pointerId);
      this.onMarquee && this.onMarquee(this._drag);
      return;
    }
    const high = vm.view.lowPitch + vm.keyCount;
    const pitch = high - 1 - Math.floor(p.y / vm.view.keyHeight);
    const tick = vm.view.startTick + p.x * vm.view.ticksPerPixel;
    this.onNote && this.onNote(pitch, tick);
  }

  _move(e) {
    const p = this._local(e);
    if (this._note) {
      this._note.x1 = p.x; this._note.y1 = p.y;
      this.onDrag && this.onDrag(this._note);
      return;
    }
    if (!this._drag) return;
    this._drag.x1 = p.x; this._drag.y1 = p.y;
    this.onMarquee && this.onMarquee(this._drag);
  }

  _up(e) {
    if (this._note) {
      const n = this._note;
      this._note = null;
      // A click is a drag of zero distance; committing one would rewrite a note
      // to exactly where it already is and burn a clip version for nothing.
      const moved = Math.abs(n.x1 - n.x0) > 2 || Math.abs(n.y1 - n.y0) > 2;
      this.onDragEnd && this.onDragEnd(moved ? n : null);
      return;
    }
    if (!this._drag) return;
    const d = this._drag;
    this._drag = null;
    this.onMarqueeEnd && this.onMarqueeEnd(d);
  }

  _cancel() {
    this._drag = null; this._note = null;
    this.onMarqueeEnd && this.onMarqueeEnd(null);
    this.onDragEnd && this.onDragEnd(null);
  }

  _pool(arr, cls, parent, n, init) {
    while (arr.length < n) {
      const el = div(cls, parent);
      if (init) init(el);
      arr.push(el);
    }
    return arr;
  }

  render(vm) {
    this.vm = vm;
    const kh = vm.view.keyHeight;

    const keys = this._pool(this.keyPool, 'pr-key', this.keysEl, vm.keyCount, (el) => {
      el.appendChild(document.createTextNode(''));
      el._y = -1; el._black = null; el._label = null;
    });
    const rows = this._pool(this.rowPool, 'pr-row', this.rowsEl, vm.keyCount, (el) => { el._y = -1; });
    for (let i = 0; i < keys.length; i++) {
      const on = i < vm.keyCount;
      const kEl = keys[i], rEl = rows[i];
      const disp = on ? '' : 'none';
      if (kEl.style.display !== disp) kEl.style.display = disp;
      if (rEl.style.display !== disp) rEl.style.display = disp;
      if (!on) continue;
      const key = vm.keys[i];
      if (kEl._y !== key.y) {
        kEl._y = key.y;
        kEl.style.transform = `translateY(${key.y}px)`;
        kEl.style.height = `${key.h}px`;
      }
      if (rEl._y !== key.y) {
        rEl._y = key.y;
        rEl.style.transform = `translateY(${key.y}px)`;
        rEl.style.height = `${key.h}px`;
      }
      if (kEl._black !== key.black) {
        kEl._black = key.black;
        kEl.classList.toggle('black', key.black);
        rEl.classList.toggle('black', key.black);
      }
      if (kEl._label !== key.label) { kEl._label = key.label; kEl.firstChild.nodeValue = key.label; }
    }

    const ruler = this._pool(this.rulerPool, 'pr-tick', this.ruler, vm.rulerCount, (el) => {
      el.appendChild(document.createTextNode('')); el._x = -1; el._bar = -1;
    });
    for (let i = 0; i < ruler.length; i++) {
      const el = ruler[i];
      const on = i < vm.rulerCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      if (el._x !== vm.ruler[i]) { el._x = vm.ruler[i]; el.style.transform = `translateX(${vm.ruler[i]}px)`; }
      if (el._bar !== vm.rulerBar[i]) { el._bar = vm.rulerBar[i]; el.firstChild.nodeValue = String(vm.rulerBar[i]); }
    }

    const grid = this._pool(this.gridPool, 'pr-gridline', this.gridEl, vm.gridCount, (el) => { el._x = -1; });
    for (let i = 0; i < grid.length; i++) {
      const el = grid[i];
      const on = i < vm.gridCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      if (el._x !== vm.grid[i]) { el._x = vm.grid[i]; el.style.transform = `translateX(${vm.grid[i]}px)`; }
      const bar = vm.gridIsBar[i] === 1;
      if (el._bar !== bar) { el._bar = bar; el.classList.toggle('bar', bar); }
    }

    const notes = this._pool(this.notePool, 'pr-note', this.notesEl, vm.noteCount, (el) => {
      el._x = -1; el._y = -1; el._w = -1;
    });
    for (let i = 0; i < notes.length; i++) {
      const el = notes[i];
      const on = i < vm.noteCount;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const n = vm.notes[i];
      if (el._x !== n.x || el._y !== n.y) {
        el._x = n.x; el._y = n.y;
        el.style.transform = `translate(${n.x}px, ${n.y}px)`;
      }
      if (el._w !== n.w) { el._w = n.w; el.style.width = `${n.w}px`; }
      if (el._h !== n.h) { el._h = n.h; el.style.height = `${n.h}px`; }
      // Velocity as opacity: it is the one note property with no other home here,
      // and a piano roll where every note looks equally loud hides the dynamics.
      const op = (0.35 + 0.65 * (n.velocity / 127)).toFixed(2);
      if (el._op !== op) { el._op = op; el.style.opacity = op; }
      if (el._muted !== n.muted) { el._muted = n.muted; el.classList.toggle('muted', n.muted); }
      if (el._add !== n.isAdd) { el._add = n.isAdd; el.classList.toggle('add', n.isAdd); }
      if (el._sel !== n.selected) { el._sel = n.selected; el.classList.toggle('sel', n.selected); }
      if (el._id !== n.id) { el._id = n.id; el.dataset.id = String(n.id); }
    }

    // A ghost of the note being dragged, so you can see where it will land
    // before you let go. Drawn as a class on the note itself rather than a
    // second element: the note IS the preview.
    for (let i = 0; i < vm.noteCount; i++) {
      const el = this.notePool[i];
      if (!el) continue;
      const dragging = vm.dragId !== undefined && vm.notes[i].id === vm.dragId;
      if (el._dragging !== dragging) { el._dragging = dragging; el.classList.toggle('dragging', dragging); }
    }

    const mq = vm.marquee;
    const mqKey = mq ? `${mq.x0},${mq.y0},${mq.x1},${mq.y1}` : '';
    if (this._mq !== mqKey) {
      this._mq = mqKey;
      if (!mq) this.marquee.style.display = 'none';
      else {
        if (this.marquee.style.display === 'none') this.marquee.style.display = '';
        const x = Math.min(mq.x0, mq.x1), y = Math.min(mq.y0, mq.y1);
        this.marquee.style.transform = `translate(${x}px, ${y}px)`;
        this.marquee.style.width = Math.abs(mq.x1 - mq.x0) + 'px';
        this.marquee.style.height = Math.abs(mq.y1 - mq.y0) + 'px';
      }
    }

    const px = vm.playheadX;
    if (this._px !== px) {
      this._px = px;
      if (px < 0) this.playhead.style.display = 'none';
      else {
        if (this.playhead.style.display === 'none') this.playhead.style.display = '';
        this.playhead.style.transform = `translateX(${px}px)`;
      }
    }
  }

  probe() {
    const vm = this.vm;
    if (!vm) return null;
    let lo = 128, hi = -1;
    for (let i = 0; i < vm.noteCount; i++) {
      const p = vm.notes[i].pitch;
      if (p < lo) lo = p; if (p > hi) hi = p;
    }
    return {
      notes: vm.noteCount, keys: vm.keyCount, zoom: vm.zoom.label, track: vm.trackName,
      lowPitch: vm.view.lowPitch, startTick: vm.view.startTick,
      pitchRange: hi < 0 ? null : [lo, hi],
      gridLines: vm.gridCount, playheadX: Math.round(vm.playheadX),
      selected: vm.selectedCount,
      domNodes: this.notePool.length + this.keyPool.length * 2 + this.gridPool.length,
    };
  }
}
