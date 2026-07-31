// A scrolling level history per track, on a canvas.
//
// This exists to answer hard requirement 3 — "must accommodate DSP scopes later"
// — with something running rather than an assurance. The engine publishes
// ui_track_peak_rms per track at its block rate (~86 Hz), which is a real signal
// arriving at a real rate, and this draws a rolling window of it.
//
// It is a canvas, not DOM, because that is what a scope has to be: an
// oscilloscope or a spectrogram redraws every pixel every frame, and no amount of
// pooled divs makes that sensible. The rest of the app stays DOM; this is the
// proof that a canvas surface fits the same view-model boundary and the same
// draw loop when the data justifies one.
//
// The ring is a Float32Array written at head and read backwards. Nothing is
// allocated per frame — same rule as everywhere else, and more load-bearing here
// because this is the one surface that redraws unconditionally.

/** Samples kept per track. At ~86 Hz this is about six seconds. */
const HISTORY = 512;

export class Scope {
  constructor(canvas, trackCount = 16) {
    // What the last paint was for. -1 head so the first render always runs.
    this._pHead = -1; this._pN = -1; this._pW = -1; this._pH = -1;
    /** How many times the canvas has actually been repainted. The guard above is
     *  only worth having if something can show it skipping, and only SAFE if
     *  something can show it not skipping when the ring moves. */
    this.paints = 0;
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.tracks = trackCount;
    this.history = new Float32Array(trackCount * HISTORY);
    this.head = 0;
    this.dpr = 1;
    this.w = 0; this.h = 0;
    this.colors = { bg: '#141621', grid: '#232532', line: '#9184d9', clip: '#d98484' };
  }

  /** Read the theme's own colours rather than repeating them here. */
  readTheme(el) {
    const s = getComputedStyle(el);
    const pick = (name, fallback) => (s.getPropertyValue(name) || '').trim() || fallback;
    this.colors.bg = pick('--uni-surface-tracker-bg', this.colors.bg);
    this.colors.grid = pick('--uni-rule-column', this.colors.grid);
    this.colors.line = pick('--base-accent', this.colors.line);
    this.colors.clip = pick('--uni-text-fx', this.colors.clip);
  }

  /** One sample per track per engine frame. Cheap enough to call unconditionally. */
  push(peaks, count) {
    const n = Math.min(count, this.tracks);
    const h = this.head;
    for (let t = 0; t < this.tracks; t++) {
      this.history[t * HISTORY + h] = t < n ? peaks[t] : 0;
    }
    this.head = (h + 1) % HISTORY;
  }

  resize(cssW, cssH, dpr) {
    if (this.w === cssW && this.h === cssH && this.dpr === dpr) return;
    // Invalidate the paint guard: assigning canvas.width below clears the backing
    // store, so the next render must run even if the ring has not advanced.
    this._pHead = -1;
    this.w = cssW; this.h = cssH; this.dpr = dpr;
    this.canvas.width = Math.max(1, Math.round(cssW * dpr));
    this.canvas.height = Math.max(1, Math.round(cssH * dpr));
    this.canvas.style.width = cssW + 'px';
    this.canvas.style.height = cssH + 'px';
  }

  /**
   * Draw `count` lanes stacked vertically. Level is mapped logarithmically for
   * the same reason the mixer's meters are: a linear scope leaves everything
   * below -20 dB flat against the floor, which is where most of a mix lives.
   */
  render(count) {
    const { ctx } = this;
    const n = Math.max(1, Math.min(count, this.tracks));
    const W = this.canvas.width, H = this.canvas.height;

    /**
     * Nothing to redraw unless something moved.
     *
     * The picture is a pure function of (head, lane count, canvas size): the ring
     * is written by the ENGINE's frames, not by draws, so between two paints with
     * no new sample the output is identical to the pixel. Painting it anyway is
     * 8 lanes x 512 points of path per frame, and Chrome allocates for every
     * segment — measured at 48.6 KB/draw against a budget of 900 B for the whole
     * mixer, which is 54x the limit for a picture that did not change.
     *
     * This is GUIDELINES 3.6 — guard every write — applied to a canvas. Every DOM
     * write on this surface is compared before it is made; a canvas is the one
     * place where "just repaint it" felt free, and it is the most expensive
     * surface in the program.
     *
     * The size is in the key because resizing CLEARS the backing store, so a
     * canvas that changed shape has to be repainted even though the data did not.
     */
    if (this._pHead === this.head && this._pN === n
        && this._pW === W && this._pH === H) return;
    this._pHead = this.head; this._pN = n; this._pW = W; this._pH = H;
    this.paints++;

    const laneH = H / n;

    ctx.fillStyle = this.colors.bg;
    ctx.fillRect(0, 0, W, H);

    ctx.strokeStyle = this.colors.grid;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let t = 1; t < n; t++) {
      const y = Math.round(t * laneH) + 0.5;
      ctx.moveTo(0, y); ctx.lineTo(W, y);
    }
    ctx.stroke();

    const step = W / HISTORY;
    // Every lane strokes in the same colour, and assigning strokeStyle makes the
    // canvas re-parse the CSS colour string; it was being set once per lane per
    // frame to the value it already held.
    ctx.strokeStyle = this.colors.line;
    for (let t = 0; t < n; t++) {
      const base = t * HISTORY;
      const top = t * laneH;
      ctx.beginPath();
      for (let i = 0; i < HISTORY; i++) {
        // Oldest sample at the left: read forward from just past the head.
        const v = this.history[base + ((this.head + i) % HISTORY)];
        const db = v <= 0 ? 0 : Math.max(0, Math.min(1, 1 + Math.log10(Math.max(v, 1e-5)) / 5));
        const y = top + laneH - db * (laneH - 2) - 1;
        const x = i * step;
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
  }

  probe() {
    // The newest sample per track, so a test can assert signal reached the ring
    // without reading pixels.
    const last = [];
    const prev = (this.head + HISTORY - 1) % HISTORY;
    for (let t = 0; t < this.tracks; t++) last.push(this.history[t * HISTORY + prev]);
    return { history: HISTORY, tracks: this.tracks, head: this.head,
             width: this.canvas.width, height: this.canvas.height, last,
             paints: this.paints };
  }
}
