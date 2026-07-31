#!/usr/bin/env node
// Heap soak across every surface.
//
//   node test/soak.mjs [minutes]
//
// test/alloc.mjs measures bytes per draw over a few thousand draws, which catches
// a reintroduced per-frame allocation. It cannot catch a LEAK — something that
// grows slowly and never comes back, like a pool that appends without bound, a
// listener rebound per frame, or a Map keyed on something unbounded.
//
// GUIDELINES 3.1 claims "no heap trend over a 5-minute soak", and that was true
// when it was written — of the tracker, which was the only surface. This checks
// all of them, and the switching between them, which is where a per-surface pool
// would leak if one did.
//
// It runs against fixtures so it needs no engine and cannot be perturbed by one.

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const MINUTES = Number(process.argv[2] || 3);
/** Bytes of growth per minute above which this is a leak rather than noise. */
const LEAK_PER_MIN = 200 * 1024;

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
await page.evaluate(() => window.__uni.reset());

const cdp = await page.context().newCDPSession(page);
const heap = async () => {
  await cdp.send('HeapProfiler.collectGarbage');
  return (await cdp.send('Runtime.getHeapUsage')).usedSize;
};

/** One pass over every surface, exercising what each actually does. */
const PASS = async () => page.evaluate(() => {
  const u = window.__uni;
  u.useMixedGrid();
  for (let i = 0; i < 40; i++) { u.step(); u.scrollTo(i * 7); }
  for (let z = 0; z < 6; z++) u.setZoom(z);

  u.useArrangeFixture();
  for (let i = 0; i < 40; i++) u.arrangeTo((i % 32) * 240000);
  for (let z = 0; z < 6; z++) u.arrangeZoom(z);

  u.usePianoFixture();
  for (let i = 0; i < 40; i++) u.pianoTo((i % 32) * 120000);
  for (let z = 0; z < 6; z++) u.pianoZoom(z);
  u.pianoSelect(0, 0, 400, 400);
  u.pianoFit();

  u.useMixerFixture();
  for (let i = 0; i < 40; i++) { u.setPan(i % 8, ((i * 37) % 2000) - 1000); u.redraw(); }

  u.usePatcherFixture();
  for (let i = 0; i < 20; i++) { u.patchSelect(i % 5); u.patchField(i % 4); }

  u.useDockFixture();
  for (let i = 0; i < 10; i++) u.run('state');
  u.useBrowserFixture();
  u.help(true); u.help(false);
  u.reset();
});

console.log(`\n  heap soak, ${MINUTES} min, all surfaces\n`);

await PASS();                                   // warm: first-touch pools grow once
const t0 = Date.now();
const base = await heap();
const samples = [];
let passes = 0;

while (Date.now() - t0 < MINUTES * 60_000) {
  await PASS();
  passes++;
  const mins = (Date.now() - t0) / 60_000;
  if (samples.length === 0 || mins - samples[samples.length - 1].min >= 0.5) {
    const used = await heap();
    samples.push({ min: mins, used });
    console.log(`  ${mins.toFixed(1)} min  ${(used / 1048576).toFixed(2)} MB`
              + `  ${(used - base >= 0 ? '+' : '')}${((used - base) / 1024).toFixed(0)} KB`
              + `  (${passes} passes)`);
  }
}

// Drift measured from the first SETTLED sample, not from `base` and not from
// sample zero. Pools reach their high-water mark over the first minute or so —
// the dock's log fills to its cap, each surface's element pools grow once — and
// that is one-off growth, not a leak. Measuring from `base` reported 238 KB/min
// and from sample zero 166, for a heap that was actually drifting at 37.
const settled = samples.find((s) => s.min >= 1);
if (!settled || samples.length < 3) {
  // Refuse rather than guess. A run too short to contain a settled sample has to
  // measure from the warm-up, which reports a rising heap as a leak — a 1-minute
  // run said 715 KB/min for a heap that drifts at 17.
  console.log(`\n  ${MINUTES} min is too short to separate pool growth from drift.`);
  console.log('  Run at least 3 minutes.\n');
  await br.close(); srv.close();
  process.exit(2);
}
const first = settled;
const last = samples[samples.length - 1];
const span = Math.max(last.min - first.min, 0.1);
const perMin = (last.used - first.used) / span;
const leak = perMin > LEAK_PER_MIN;

await br.close(); srv.close();

console.log(`\n  ${passes} passes over ${last.min.toFixed(1)} min`);
console.log(`  pools settled at ${((first.used - base) / 1024).toFixed(0)} KB above cold start`);
console.log(`  steady drift ${(perMin / 1024).toFixed(0)} KB/min (limit ${LEAK_PER_MIN / 1024} KB/min)`);
console.log(`\n${leak ? 'LEAKING' : 'NO LEAK'}`);
process.exit(leak ? 1 : 0);
