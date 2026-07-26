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
await page.evaluate(() => { for (let i = 0; i < 300; i++) window.__uni.step(); });  // warm

const s0 = await heap();
// NB: nothing inside the measured loop may allocate on the test's behalf.
// state() deep-clones via JSON and inflated this by 15 bytes/draw — the probe
// measuring itself, for the third time in this project. Read it once, outside.
await page.evaluate((n) => { const at = window.__uni.state().start;
  for (let i = 0; i < n; i++) window.__uni.scrollTo(at); }, N);
const s1 = await heap();
const statik = (s1 - s0) / N;

const c0 = await heap();
await page.evaluate((n) => { for (let i = 0; i < n; i++) window.__uni.step(); }, N);
const c1 = await heap();
const scrolling = (c1 - c0) / N;

await br.close(); srv.close();

let fail = 0;
const check = (v, max, label) => {
  const ok = v <= max;
  if (!ok) fail++;
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label.padEnd(34)} ${v.toFixed(1)} bytes/draw   (limit ${max})`);
};
console.log(`\n  allocation, ${N} draws each\n`);
check(statik, STATIC_MAX, 'at rest, nothing changes');
check(scrolling, SCROLL_MAX, 'scrolling, one row rebinds');
console.log(`\n${fail === 0 ? 'ALL PASS' : `${fail} FAILURES — see GUIDELINES.md section 3`}`);
process.exit(fail ? 1 : 0);
