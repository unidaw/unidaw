#!/usr/bin/env node
// Allocation regression test. A written rule decays; a failing test does not.
//
//   node test/alloc.mjs
//
// The draw path must allocate nothing at rest. Anything it does allocate while
// scrolling must be the strings the DOM genuinely requires for the one row whose
// identity changed — a `top` value and a `data-row` — and nothing else.
//
// Before this was enforced the draw path allocated ~280 strings per frame:
// String(rowIdx) for every pooled row, four template literals per clip rail,
// the band transform, and a HUD rebuilt from template literals every draw.
// That is ~11 KB/draw, or 660 KB/s at 60 fps.
//
// If this fails, you have added one of:
//   - a template literal or string concat in a per-frame path
//   - String(x) / x.toFixed() / padStart in a per-frame path
//   - .textContent = (destroys and recreates a Text node — use .nodeValue)
//   - an unguarded style write (compare a NUMBER you cached, not a string)
//   - .dataset.foo compared against String(x) rather than a numeric cache
//   - an object/array literal, a .map/.filter, a for...of, or a closure per frame
// See GUIDELINES.md section 3.
//
// ---------------------------------------------------------------------------
// WHY THIS MEASURES WHAT IT MEASURES
//
// The first version of this file compared `Runtime.getHeapUsage` before and
// after, with a `HeapProfiler.collectGarbage` on each side. That measures the
// heap that SURVIVED, which is very nearly the opposite of what this test is
// about: every allocation the draw path makes is dead by the time the frame
// ends, so a collectGarbage-bracketed measurement collects it all and reports
// almost nothing. Three independent audits of the renderers found hundreds of
// transient strings per frame while that metric was reading 14-40 bytes/draw
// and passing. It was not measuring a clean draw path; it was measuring the
// absence of a LEAK, which is a different and much weaker property.
//
// This version uses the sampling heap profiler with
// `includeObjectsCollectedByMinorGC`, which counts allocations whether or not
// they survive. The numbers it reports are perhaps 5-10% off in either
// direction — it samples every `SAMPLE_BYTES` rather than instrumenting every
// allocation — but they are of the right quantity, and they move when the code
// moves. The retained-heap check is kept as a second, separate assertion,
// because a leak and a churn are both worth failing on and neither implies the
// other.
// ---------------------------------------------------------------------------

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// How many bytes between samples. Small enough that a few hundred bytes a frame
// is many samples over a run, large enough that the profiler does not dominate
// the thing it is profiling.
const SAMPLE_BYTES = 256;

// Per-draw allocation budget, in bytes.
//
// Zero is not achievable and not the goal: a `top` style write on the row that
// crossed the band edge is a string the DOM requires, and the profiler's own
// bookkeeping lands in the same measurement. What IS achievable is that a frame
// costs a handful of strings rather than a few hundred, and these limits are set
// just above where the fixed paths actually land so that reintroducing one
// unguarded per-row or per-note string — which is 40x this — cannot pass.
const CHURN_MAX = 900;
// Scrolling legitimately rebinds a row per step and repaints the band transform.
// Every surface now scrolls by translating a container rather than repositioning
// its contents, so this is close to the flat case: measured 144-720.
const CHURN_SCROLL_MAX = 1200;
// Retained growth. Loose, and about leaks rather than churn: see above.
const RETAINED_MAX = 250;

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2' };
const srv = createServer((q, r) => {
  const p = join(root, normalize(decodeURI(q.url.split('?')[0])));
  let b; try { b = readFileSync(p); } catch { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': MIME[extname(p)] || 'application/octet-stream' }); r.end(b);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));

const br = await chromium.launch({ channel: 'chrome' });
const page = await br.newPage({ viewport: { width: 1500, height: 760 }, deviceScaleFactor: 2 });
await page.goto(`http://127.0.0.1:${srv.address().port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(300);

const cdp = await page.context().newCDPSession(page);
const heap = async () => {
  await cdp.send('HeapProfiler.collectGarbage');
  return (await cdp.send('Runtime.getHeapUsage')).usedSize;
};

/** Total bytes in a sampling profile, survivors and collected alike. */
function profileBytes(node) {
  let n = node.selfSize || 0;
  const kids = node.children || [];
  for (let i = 0; i < kids.length; i++) n += profileBytes(kids[i]);
  return n;
}

const N = 3000;

let fail = 0;
const check = (v, max, label) => {
  const ok = v <= max;
  if (!ok) fail++;
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label.padEnd(40)} ${v.toFixed(0).padStart(6)} B/draw   (limit ${max})`);
};

/**
 * Measure a loop body. `setup` runs once outside the measurement — nothing
 * inside the measured loop may allocate on the test's behalf. state() deep-
 * clones via JSON and inflated an earlier version by 15 bytes/draw: the probe
 * measuring itself, for the third time in this project.
 *
 * Returns {churn, retained}: bytes allocated per draw, and bytes still reachable
 * per draw. The first is the one this file exists for.
 */
async function measure(setup, body, n = N) {
  if (setup) await page.evaluate(setup);
  await page.evaluate(body, 300);              // warm: JIT, pool growth, first bind
  const before = await heap();
  await cdp.send('HeapProfiler.startSampling', {
    samplingInterval: SAMPLE_BYTES,
    includeObjectsCollectedByMinorGC: true,
    includeObjectsCollectedByMajorGC: true,
  });
  await page.evaluate(body, n);
  const { profile } = await cdp.send('HeapProfiler.stopSampling');
  const churn = profileBytes(profile.head) / n;
  const retained = (await heap() - before) / n;
  return { churn, retained };
}

/** Both assertions for one scenario, on one line each. */
async function scenario(label, setup, body, churnMax = CHURN_MAX) {
  const { churn, retained } = await measure(setup, body);
  check(churn, churnMax, label);
  check(retained, RETAINED_MAX, label + ' (retained)');
}

console.log(`\n  allocation, ${N} draws each — churn is what matters\n`);

// Every scenario states where it is — zoom, scroll position, fixture or engine.
// None of them may inherit that from the scenario before it. Two of these were
// implicitly running at bar 3,000 and zoom "1 bar" because an earlier scenario
// had scrolled there, and the numbers were four times what the same code
// measured from the top of the song. A benchmark that does not say where it
// stands is measuring something, but not the thing in its name.
// Spelled out at each site rather than shared: page.evaluate serialises the
// function and ships it to the browser, so it cannot close over anything
// declared here in Node. A helper would have looked right and thrown at runtime.

// --- the tracker against a REAL engine frame ------------------------------
// This is the case the app is actually in when performance matters, and until
// now it was the one case the file never covered: every scenario below loads
// `?engine=off`, which takes the fixture branch and never spells a pitch, a
// velocity, a harmony field or a clip name from engine data.
await scenario('tracker, engine data, playing',
  () => { window.__uni.view('tracker'); window.__uni.useBusyEngine(8, 8);
          window.__uni.setZoom(1); window.__uni.scrollTo(0); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.tickBusy(1); });

await scenario('tracker, engine data, at rest',
  () => { window.__uni.setZoom(1); window.__uni.scrollTo(0); window.__uni.goto(0, 0); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.scrollTo(0); });

await scenario('tracker, engine data, scrolling',
  () => { window.__uni.setZoom(1); window.__uni.scrollTo(0); window.__uni.goto(0, 0); }, (n) => { for (let i = 0; i < n; i++) window.__uni.step(); },
  CHURN_SCROLL_MAX);

// The coarse zooms take an entirely different branch — the engine's aggregate
// contour rather than per-note cells — and nothing measured it before.
await scenario('tracker, aggregate zoom',
  // Rebuilt, not just rewound: tickBusy advances the synthetic playhead and
  // never resets it, so by the time this ran the playhead alone was past the
  // small-integer range and the scenario was measuring the long-song regime
  // under the name of the aggregate one.
  () => { window.__uni.useBusyEngine(8, 8); window.__uni.scrollTo(0);
          window.__uni.goto(0, 0); window.__uni.setZoom(4); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.tickBusy(1); });

/**
 * Deep into a long song, at a coarse zoom. Its own limit, and here is why.
 *
 * Nanoticks run at 960,000 to the quarter, so a position passes V8's
 * small-integer range (2^30 with pointer compression) about nine minutes in at
 * 120 BPM. Past that, every tick expression in the draw path — a window bound, a
 * cursor position, the minimap's span — produces a boxed double on the heap
 * instead of an immediate. Nothing is wrong with the code; the numbers simply
 * got big. It costs roughly a kilobyte a frame, it cannot be guarded away
 * without giving up absolute tick arithmetic, and it appears only in long
 * projects, which is the worst way for a cost to arrive: invisible in every
 * short test.
 *
 * So it is measured deliberately, with a limit of its own, and the day someone
 * wants it gone the fix is to carry window-relative offsets rather than absolute
 * ticks through the hot arithmetic.
 */
await scenario('tracker, hour into the song',
  () => { window.__uni.useBusyEngine(8, 8); window.__uni.goto(0, 0);
          window.__uni.setZoom(4); window.__uni.scrollTo(3000); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.tickBusy(1); },
  1200);

// --- the fixture tracker ---------------------------------------------------
// The fixture derives every cell from the row's tick, so it is the one path that
// cannot skip the arithmetic above. It ships only behind `?engine=off`.
await scenario('tracker fixture at rest',
  () => { window.__uni.useFixture(); window.__uni.view('tracker');
          window.__uni.setZoom(1); window.__uni.scrollTo(0); window.__uni.goto(0, 0); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.scrollTo(0); });
await scenario('tracker fixture scrolling',
  () => { window.__uni.setZoom(1); window.__uni.scrollTo(0); window.__uni.goto(0, 0); }, (n) => { for (let i = 0; i < n; i++) window.__uni.step(); },
  CHURN_SCROLL_MAX);

// --- the other surfaces --------------------------------------------------
// Each was built after the rule was written and none of them was covered by it.
// A rule enforced on one surface is a rule the next surface does not have.
await scenario('arrange scrolling time',
  () => { window.__uni.useArrangeFixture(); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.arrangeTo((i % 64) * 240000); },
  CHURN_SCROLL_MAX);

/**
 * The arrangement panning with WAVEFORMS on screen.
 *
 * The one property this is measuring: a pan must not repaint the waveform canvas.
 * The layer is one canvas at the painted band's absolute origin inside the same
 * scrolled wrapper the clips are in, so a pan moves it with the one transform
 * everything else already rides (GUIDELINES 3.3) and the canvas is untouched
 * until the viewport reaches the edge of what has been painted.
 *
 * The step is the SAME 4 px a frame as the scenario above, and the range is eight
 * times longer: the painted band is wider than the screen, so a 256 px sweep
 * would never leave it, and this one leaves it about ten times a cycle. The
 * repaint path is therefore measured rather than avoided — 34 repaints in 3,300
 * draws, which is what "a pan does not repaint the canvas" looks like as a number.
 *
 * READ THE NUMBER CAREFULLY. Most of it is not the waveform layer. The same wide
 * pan run against the ARRANGE fixture, with no audio anywhere and the canvas
 * painting nothing, measures ~1,120 B/draw — whichever arrange scenario first
 * pans further than a screen inside a profiled run pays a large one-off in
 * `render`, and every arrange scenario after it is cheap. Run second, this
 * scenario measures 212. Run first, as it is here, it measures ~940. The waveform
 * layer's own contribution, in both orders, is the ~150 B/draw that `_waveWindow`,
 * `_waves` and `ticksPerSourceFrame` account for between them.
 *
 * The 300-draw warm-up walks the whole range first, so every window the loop asks
 * for is already in the cache and what is left is the painting, not the fixture
 * synthesising audio.
 *
 * If this goes red, the likely cause is a guard that no longer covers everything
 * the picture is computed from, so every frame repaints: the canvas is a cache
 * and its key is the pile of numbers compared at the top of `_waves`.
 */
await scenario('arrange panning with waveforms',
  () => { window.__uni.useWaveFixture(); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.arrangeTo((i % 512) * 240000); },
  CHURN_SCROLL_MAX);

await scenario('piano roll scrolling time',
  () => { window.__uni.usePianoFixture(); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.pianoTo((i % 64) * 120000); },
  CHURN_SCROLL_MAX);

// The mixer's meters move every frame by design, so this is the one surface
// where a redraw legitimately writes on every pass. It still must not allocate.
await scenario('mixer, meters and controls moving',
  () => { window.__uni.useMixerFixture(); },
  // redraw(), not setPan alone: the mixer's setters go through schedule(), which
  // coalesces to one draw per frame, so a loop through them measures almost no
  // draws at all and reports a reassuringly small number that means nothing.
  (n) => { for (let i = 0; i < n; i++) {
    window.__uni.setPan(i % 8, ((i * 37) % 2000) - 1000);
    window.__uni.redraw();
  } });

// The patcher was read-only until config editing landed; a draw that rebuilds a
// config line per node per frame is exactly the shape that quietly allocates.
await scenario('patcher moving the field cursor',
  () => { window.__uni.usePatcherFixture(); },
  (n) => { for (let i = 0; i < n; i++) {
    window.__uni.patchSelect(i % 5);
    window.__uni.patchField(i % 4);
  } });

// The patcher redrawing with nothing moving: the graph is laid out once and the
// per-frame cost should be a pass over guards. It shares the frame with the
// device chain, which is on screen whatever surface you are looking at.
await scenario('patcher at rest',
  null, (n) => { for (let i = 0; i < n; i++) window.__uni.redraw(); });

// The device chain redraws with everything else and builds a title string per
// card. Small surfaces are where the discipline quietly stops being true.
await scenario('device chain, moving the selection',
  () => { window.__uni.useChainFixture(); },
  (n) => { for (let i = 0; i < n; i++) { window.__uni.chainSelect(i % 4); } });

// The palette filters on every keystroke and is the newest draw path here.
await scenario('palette, moving the selection',
  () => { window.__uni.palette(true); },
  (n) => { for (let i = 0; i < n; i++) { window.__uni.paletteMove(i % 2 ? 1 : -1); } });

// The browser rail with both feeds behind it: 5 projects and 52 plugins, which
// is what this machine actually has. It has never been covered here, and it is
// the surface that most invites a per-row string — every row carries a badge, a
// name, a meta line built from a vendor and a format, and a type mark, and the
// rail redraws with the rest of the app on every frame it is open, whatever
// surface you are looking at. If any of those four is built per row per frame
// rather than when the catalogue arrived, this reads ~57x it.
await scenario('browser rail, 52 plugins, moving the selection',
  () => { window.__uni.usePluginFixture(); window.__uni.browserCategory('all'); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.browserMove(i % 2 ? 1 : -1); });

await br.close(); srv.close();

console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES — see GUIDELINES.md section 3`}`);
process.exit(fail ? 1 : 0);
