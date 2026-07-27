#!/usr/bin/env node
// Frame-time regression test.
//
//   node test/frametime.mjs
//
// Requirement 2 is "the UI must not lag", and allocation is a proxy for it, not
// a measure of it. This measures the actual thing: how long one draw takes, per
// surface, against the frame budget.
//
// A dropped frame at 60 Hz is a frame whose WORK exceeds 16.6 ms. Measuring rAF
// intervals instead is a mistake I made earlier in this project and it hid a
// zoom sitting at 86% of budget — intervals look fine right up until they do not.
//
// Thresholds are per-surface because the surfaces are not comparable: the
// tracker binds ~1,700 cells, the mixer sixteen strips. What matters is that
// none of them is near the budget and that a regression moves one of them a lot.

import { chromium } from 'playwright';
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const BUDGET = 16.6;

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2' };
const srv = createServer((q, r) => {
  const p = join(root, normalize(decodeURI(q.url.split('?')[0])));
  let b; try { b = readFileSync(p); } catch { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': MIME[extname(p)] || 'application/octet-stream' }); r.end(b);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));

// A real browser with a real window. Headless has no vsync and no compositor
// under load, and reports p50 intervals that are impossible on a 60 Hz display.
const br = await chromium.launch({ channel: 'chrome', headless: false });
const page = await br.newPage({ viewport: { width: 1500, height: 760 }, deviceScaleFactor: 2 });
await page.goto(`http://127.0.0.1:${srv.address().port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);
await page.evaluate(() => document.fonts.ready);
// Pin the data source. Without this the numbers depend on whether a sidecar
// happens to be running: with an engine the tracker draws real notes, without one
// it runs contentAt (a Math.sin and string work PER CELL) and reports ~7x the
// cost. That is the goldens' lesson again — a measurement that changes with the
// weather is not a measurement. useMixedGrid installs a synthetic engine store,
// so this is deterministic AND exercises the path that ships.
await page.evaluate(() => window.__uni.useMixedGrid());
await page.waitForTimeout(300);

/**
 * Time `body` over `n` synchronous draws and return p50/p95/max in ms.
 * Synchronous on purpose — routing through schedule() would measure the browser's
 * frame scheduler rather than our draw.
 */
async function time(setup, body, n = 400) {
  if (setup) await page.evaluate(setup);
  await page.evaluate(body, 60);                       // warm
  return page.evaluate(([fn, count]) => {
    const f = new Function('i', fn);
    const t = new Float64Array(count);
    for (let i = 0; i < count; i++) {
      const a = performance.now();
      f(i);
      t[i] = performance.now() - a;
    }
    const s = Array.from(t).sort((x, y) => x - y);
    return { p50: s[Math.floor(count * 0.5)], p95: s[Math.floor(count * 0.95)], max: s[count - 1] };
  }, [body.toString().replace(/^\s*\(?i\)?\s*=>\s*/, 'return (function(){'). concat('})();'), n]);
}

const results = [];
let fail = 0;
const check = (label, r, limit) => {
  const ok = r.p95 <= limit;
  if (!ok) fail++;
  results.push({ label, ...r, limit, ok });
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label.padEnd(34)} p50 ${r.p50.toFixed(2)}  p95 ${r.p95.toFixed(2)}  max ${r.max.toFixed(2)} ms   (limit ${limit})`);
};

console.log(`\n  frame work per surface, budget ${BUDGET} ms\n`);

await page.evaluate(() => { window.__uni.useMixedGrid(); window.__uni.view('tracker'); });
check('tracker, cursor step', await time(null, (i) => { window.__uni.step(); }), BUDGET);
check('tracker, zoom', await time(null, (i) => { window.__uni.setZoom(i % 6); }), BUDGET);

check('arrange, scroll time', await time(
  () => { window.__uni.useArrangeFixture(); },
  (i) => { window.__uni.arrangeTo((i % 128) * 240000); }), BUDGET);

check('piano roll, scroll time', await time(
  () => { window.__uni.usePianoFixture(); },
  (i) => { window.__uni.pianoTo((i % 128) * 120000); }), BUDGET);

check('mixer, full redraw', await time(
  () => { window.__uni.useMixerFixture(); },
  (i) => { window.__uni.setPan(i % 8, ((i * 37) % 2000) - 1000); window.__uni.redraw(); }), BUDGET);

/**
 * A real project, playing — eight tracks and a few thousand notes rather than
 * useMixedGrid's four lanes and a hundred.
 *
 * Everything above it runs against a fixture small enough that the numbers say
 * little about the loaded case, which is the only case a user is ever in. The
 * last one sits three thousand bars in, where positions have outgrown V8's
 * small-integer range and every tick expression is a boxed double — a regime
 * that only exists in long songs and therefore in no short test.
 */
check('tracker, busy project playing', await time(
  () => { window.__uni.view('tracker'); window.__uni.useBusyEngine(8, 8);
          window.__uni.setZoom(1); window.__uni.scrollTo(0); },
  (i) => { window.__uni.tickBusy(1); }), BUDGET);

check('tracker, busy project scrolling', await time(
  () => { window.__uni.scrollTo(0); window.__uni.goto(0, 0); },
  (i) => { window.__uni.step(); }), BUDGET);

check('tracker, busy project, hour in', await time(
  () => { window.__uni.useBusyEngine(8, 8); window.__uni.goto(0, 0);
          window.__uni.setZoom(4); window.__uni.scrollTo(3000); },
  (i) => { window.__uni.tickBusy(1); }), BUDGET);

await br.close(); srv.close();

const worst = results.reduce((a, b) => (a.p95 > b.p95 ? a : b));
console.log(`\n  worst: ${worst.label} at ${(100 * worst.p95 / BUDGET).toFixed(0)}% of budget`);
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} OVER BUDGET`}`);
process.exit(fail ? 1 : 0);
