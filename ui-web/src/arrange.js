// Arrange renderer: lanes of clip blocks on a horizontal time axis.
//
// Same rules as the tracker renderer, for the same reasons (GUIDELINES 3):
// pooled elements that are hidden rather than removed, every style write guarded
// by a cached number, and no string building unless the string actually changed.
// A DAW's arrange page is where a user drags things around, so it is the one
// surface where a dropped frame is felt directly.

const GRID_POOL = 256;

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

export class Arrange {
  constructor(host, metrics) {
    this.host = host;
    this.metrics = metrics;
    this.host.className = 'ar';

    this.gutter = div('ar-gutter', host);
    this.band = div('ar-band', host);
    this.ruler = div('ar-ruler', this.band);
    this.loop = div('ar-loop', this.ruler);
    this.lanesEl = div('ar-lanes', this.band);
    this.gridEl = div('ar-grid', this.band);
    this.clipsEl = div('ar-clips', this.band);
    this.playhead = div('ar-playhead', this.band);

    this.lanePool = [];
    this.headPool = [];
    this.clipPool = [];
    this.gridPool = [];
    this.rulerPool = [];
    this.laneCount = 0;
    this.vm = null;
  }

  /** Grow pools to the shape being drawn. Called only when the shape changes. */
  resize(laneCount, laneHeight) {
    if (this.laneCount === laneCount && this._laneHeight === laneHeight) return;
    this.laneCount = laneCount;
    this._laneHeight = laneHeight;

    while (this.lanePool.length < laneCount) {
      const i = this.lanePool.length;
      const lane = div('ar-lane', this.lanesEl);
      lane.dataset.track = String(i);
      this.lanePool.push(lane);
      const head = div('ar-head', this.gutter);
      head.dataset.track = String(i);
      const nm = div('ar-head-name', head);
      nm.appendChild(document.createTextNode(''));
      const lpb = div('ar-head-lpb', head);
      lpb.appendChild(document.createTextNode(''));
      head._nm = nm.firstChild; head._lpb = lpb.firstChild; head._lpbVal = -1;
      this.headPool.push(head);
    }
    for (let i = 0; i < this.lanePool.length; i++) {
      const on = i < laneCount;
      const lane = this.lanePool[i], head = this.headPool[i];
      const disp = on ? '' : 'none';
      if (lane.style.display !== disp) lane.style.display = disp;
      if (head.style.display !== disp) head.style.display = disp;
      if (!on) continue;
      const y = i * laneHeight;
      if (lane._y !== y) { lane._y = y; lane.style.transform = `translateY(${y}px)`; }
      if (lane._h !== laneHeight) { lane._h = laneHeight; lane.style.height = `${laneHeight}px`; }
      if (head._h !== laneHeight) { head._h = laneHeight; head.style.height = `${laneHeight}px`; }
    }
  }

  _grid(n) {
    while (this.gridPool.length < n && this.gridPool.length < GRID_POOL) {
      this.gridPool.push(div('ar-gridline', this.gridEl));
    }
    return this.gridPool;
  }

  _ruler(n) {
    while (this.rulerPool.length < n) {
      const el = div('ar-tick', this.ruler);
      el.appendChild(document.createTextNode(''));
      el._x = -1; el._bar = -1;
      this.rulerPool.push(el);
    }
    return this.rulerPool;
  }

  _clip(n) {
    while (this.clipPool.length < n) {
      const el = div('ar-clip', this.clipsEl);
      const label = div('ar-clip-name', el);
      label.appendChild(document.createTextNode(''));
      el._label = label.firstChild;
      el._x = -1; el._w = -1; el._y = -1; el._name = null;
      this.clipPool.push(el);
    }
    return this.clipPool;
  }

  render(vm) {
    this.vm = vm;
    const lh = vm.lanes.length ? vm.lanes[0].height : 44;
    this.resize(vm.laneCount, lh);

    for (let i = 0; i < vm.laneCount; i++) {
      const lane = vm.lanes[i], head = this.headPool[i];
      if (head._nm.nodeValue !== lane.name) head._nm.nodeValue = lane.name;
      if (head._lpbVal !== lane.lpb) {
        head._lpbVal = lane.lpb;
        head._lpb.nodeValue = lane.lpb ? lane.lpb + '/b' : '';
      }
    }

    const ruler = this._ruler(vm.rulerCount);
    for (let i = 0; i < ruler.length; i++) {
      const el = ruler[i];
      if (i >= vm.rulerCount) {
        if (el.style.display !== 'none') el.style.display = 'none';
        continue;
      }
      if (el.style.display === 'none') el.style.display = '';
      const x = vm.ruler[i];
      if (el._x !== x) { el._x = x; el.style.transform = `translateX(${x}px)`; }
      const bar = vm.rulerBar[i];
      if (el._bar !== bar) { el._bar = bar; el.firstChild.nodeValue = String(bar); }
    }

    const grid = this._grid(vm.gridCount);
    for (let i = 0; i < grid.length; i++) {
      const el = grid[i];
      if (i >= vm.gridCount) {
        if (el.style.display !== 'none') el.style.display = 'none';
        continue;
      }
      if (el.style.display === 'none') el.style.display = '';
      const x = vm.grid[i];
      if (el._x !== x) { el._x = x; el.style.transform = `translateX(${x}px)`; }
      const bar = vm.gridIsBar[i] === 1;
      if (el._bar !== bar) { el._bar = bar; el.classList.toggle('bar', bar); }
    }

    const clips = this._clip(vm.clipCount);
    for (let i = 0; i < clips.length; i++) {
      const el = clips[i];
      if (i >= vm.clipCount) {
        if (el.style.display !== 'none') el.style.display = 'none';
        continue;
      }
      if (el.style.display === 'none') el.style.display = '';
      const c = vm.clips[i];
      const y = c.track * lh;
      if (el._x !== c.x || el._y !== y) {
        el._x = c.x; el._y = y;
        el.style.transform = `translate(${c.x}px, ${y}px)`;
      }
      if (el._w !== c.w) { el._w = c.w; el.style.width = `${c.w}px`; }
      if (el._h !== lh) { el._h = lh; el.style.height = `${lh}px`; }
      if (el._name !== c.name) { el._name = c.name; el._label.nodeValue = c.name; }
      if (el._audio !== c.audio) { el._audio = c.audio; el.classList.toggle('audio', c.audio); }
      if (el._sel !== c.selected) { el._sel = c.selected; el.classList.toggle('sel', c.selected); }
      const pk = c.track + ':' + c.startTick;
      if (el._pid !== pk) { el._pid = pk; el.dataset.placement = pk; }
    }

    const lp = vm.loop;
    const lk = lp.on ? lp.x + ':' + lp.w : '';
    if (this._loop !== lk) {
      this._loop = lk;
      if (!lp.on) this.loop.style.display = 'none';
      else {
        if (this.loop.style.display === 'none') this.loop.style.display = '';
        this.loop.style.transform = `translateX(${lp.x}px)`;
        this.loop.style.width = `${lp.w}px`;
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

  /** Which lane and tick a point falls on. Null outside the lanes. */
  hitTest(clientX, clientY, vm) {
    const r = this.band.getBoundingClientRect();
    const x = clientX - r.left, y = clientY - r.top;
    if (x < 0 || y < 0) return null;
    const lh = vm.lanes.length ? vm.lanes[0].height : 44;
    const track = Math.floor(y / lh);
    if (track < 0 || track >= vm.laneCount) return null;
    const tick = vm.view.startTick + x * vm.view.ticksPerPixel;
    // A hit on a placement is more specific than a hit on empty lane, and the
    // caller almost always wants the placement, so resolve it here.
    for (let i = 0; i < vm.clipCount; i++) {
      const c = vm.clips[i];
      if (c.track === track && x >= c.x && x < c.x + c.w) {
        return { track, tick, placement: c.track + ':' + c.startTick, id: c.id, clip: c };
      }
    }
    return { track, tick, placement: null, clip: null };
  }

  /** Structure for tests and agents — the same contract the tracker's probe has. */
  probe() {
    const vm = this.vm;
    if (!vm) return null;
    return {
      lanes: vm.laneCount,
      clips: vm.clipCount,
      audioClips: (() => { let n = 0; for (let i = 0; i < vm.clipCount; i++) if (vm.clips[i].audio) n++; return n; })(),
      zoom: vm.zoom.label,
      startTick: vm.view.startTick,
      ticksPerPixel: vm.view.ticksPerPixel, width: vm.view.width,
      gridLines: vm.gridCount,
      loop: vm.loop.on ? { x: Math.round(vm.loop.x), w: Math.round(vm.loop.w) } : null,
      rulerTicks: vm.rulerCount,
      firstBar: vm.rulerCount ? vm.rulerBar[0] : -1,
      playheadX: Math.round(vm.playheadX),
      domNodes: this.clipPool.length + this.lanePool.length + this.gridPool.length,
    };
  }
}
