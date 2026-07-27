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
// See GUIDELINES.md section 3.

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Thresholds are deliberately loose. Repeated runs of the fixed renderer land
// anywhere in 1-17 bytes/draw: at 3,000 draws that is under 50 KB total, which is
// inside the resolution of collectGarbage + getHeapUsage. Chasing that spread
// would produce a flaky test that everyone learns to ignore.
//
// What this catches is the regression CLASS, which is orders of magnitude away:
// the unfixed renderer allocated ~11,000 bytes/draw. 250 is ~15x above observed
// noise and ~44x below a single reintroduced per-frame template literal.
const STATIC_MAX = 250;
const SCROLL_MAX = 250;

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
await page.goto(`http://127.0.0.1:${srv.address().port}/index.html`);
await page.waitForFunction(() => !!window.__uni);
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(300);

const cdp = await page.context().newCDPSession(page);
const heap = async () => {
  await cdp.send('HeapProfiler.collectGarbage');
  return (await cdp.send('Runtime.getHeapUsage')).usedSize;
};

const N = 3000;

let fail = 0;
const check = (v, max, label) => {
  const ok = v <= max;
  if (!ok) fail++;
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label.padEnd(38)} ${v.toFixed(1)} bytes/draw   (limit ${max})`);
};

/**
 * Measure a loop body. `setup` runs once outside the measurement — nothing
 * inside the measured loop may allocate on the test's behalf. state() deep-
 * clones via JSON and inflated an earlier version by 15 bytes/draw: the probe
 * measuring itself, for the third time in this project.
 */
async function measure(setup, body, n = N) {
  if (setup) await page.evaluate(setup);
  await page.evaluate(body, 300);              // warm
  const a = await heap();
  await page.evaluate(body, n);
  const b = await heap();
  return (b - a) / n;
}

console.log(`\n  allocation, ${N} draws each\n`);

// --- tracker -------------------------------------------------------------
check(await measure(
  () => { window.__uni.view('tracker'); },
  (n) => { const at = window.__uni.state().start;
           for (let i = 0; i < n; i++) window.__uni.scrollTo(at); }),
  STATIC_MAX, 'tracker at rest');
check(await measure(null, (n) => { for (let i = 0; i < n; i++) window.__uni.step(); }),
  SCROLL_MAX, 'tracker scrolling');

// --- the other surfaces --------------------------------------------------
// Each was built after the rule was written and none of them was covered by it.
// A rule enforced on one surface is a rule the next surface does not have.
check(await measure(
  () => { window.__uni.useArrangeFixture(); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.arrangeTo((i % 64) * 240000); }),
  SCROLL_MAX, 'arrange scrolling time');

check(await measure(
  () => { window.__uni.usePianoFixture(); },
  (n) => { for (let i = 0; i < n; i++) window.__uni.pianoTo((i % 64) * 120000); }),
  SCROLL_MAX, 'piano roll scrolling time');

// The mixer's meters move every frame by design, so this is the one surface
// where a redraw legitimately writes on every pass. It still must not allocate.
check(await measure(
  () => { window.__uni.useMixerFixture(); },
  // redraw(), not setPan alone: the mixer's setters go through schedule(), which
  // coalesces to one draw per frame, so a loop through them measures almost no
  // draws at all and reports a reassuringly small number that means nothing.
  (n) => { for (let i = 0; i < n; i++) {
    window.__uni.setPan(i % 8, ((i * 37) % 2000) - 1000);
    window.__uni.redraw();
  } }),
  SCROLL_MAX, 'mixer, meters and controls moving');

// The patcher was read-only until config editing landed; a draw that rebuilds a
// config line per node per frame is exactly the shape that quietly allocates.
check(await measure(
  () => { window.__uni.usePatcherFixture(); },
  (n) => { for (let i = 0; i < n; i++) {
    window.__uni.patchSelect(i % 5);
    window.__uni.patchField(i % 4);
  } }),
  SCROLL_MAX, 'patcher moving the field cursor');
await br.close(); srv.close();

console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES — see GUIDELINES.md section 3`}`);
process.exit(fail ? 1 : 0);
