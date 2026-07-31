#!/usr/bin/env node
/**
 * WHERE the palette's per-draw bytes come from — the same sampling profile alloc.mjs takes,
 * printed by call site instead of summed.
 *
 * alloc.mjs answers "how much" and that is what it is for. Four guesses at "where" were wrong
 * tonight (the layout read, the scroll write, the pool, the match array), and each one cost a
 * run. The profiler already knows; it was only ever being summed.
 */
import { chromium } from 'playwright';

// The same static server and the same `?engine=off` page alloc.mjs uses, so this profiles the
// thing that suite measures rather than a different app that happens to look like it.
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { join, extname, normalize, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
               '.css': 'text/css', '.json': 'application/json', '.woff2': 'font/woff2' };
const srv = createServer((q, r) => {
  const p2 = join(root, normalize(decodeURI(q.url.split('?')[0])));
  let b; try { b = readFileSync(p2); } catch { r.writeHead(404); return r.end(); }
  r.writeHead(200, { 'content-type': MIME[extname(p2)] || 'application/octet-stream' }); r.end(b);
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1500, height: 760 }, deviceScaleFactor: 2 });
await page.goto(`http://127.0.0.1:${srv.address().port}/index.html?engine=off`);
await page.waitForFunction(() => !!window.__uni);

const cdp = await page.context().newCDPSession(page);
await cdp.send('HeapProfiler.enable');

await page.evaluate(() => { window.__uni.palette(true); });
await page.evaluate((n) => {
  for (let i = 0; i < n; i++) window.__uni.paletteMove(i % 2 ? 1 : -1);
}, 300);

await cdp.send('HeapProfiler.startSampling', {
  samplingInterval: 512,
  includeObjectsCollectedByMinorGC: true,
  includeObjectsCollectedByMajorGC: true,
});
const N = 3000;
await page.evaluate((n) => {
  for (let i = 0; i < n; i++) window.__uni.paletteMove(i % 2 ? 1 : -1);
}, N);
const { profile } = await cdp.send('HeapProfiler.stopSampling');

// Flatten to (function, file:line) -> self bytes.
const rows = new Map();
const walk = (node) => {
  const f = node.callFrame || {};
  const key = `${f.functionName || '(anon)'}  ${String(f.url || '').split('/').pop()}:${f.lineNumber}`;
  rows.set(key, (rows.get(key) || 0) + (node.selfSize || 0));
  for (const k of node.children || []) walk(k);
};
walk(profile.head);

const total = [...rows.values()].reduce((a, b) => a + b, 0);
console.log(`\n  ${(total / N).toFixed(0)} B/draw total, by call site:\n`);
[...rows.entries()].sort((a, b) => b[1] - a[1]).slice(0, 14).forEach(([k, v]) => {
  if (v / N < 1) return;
  console.log(`    ${(v / N).toFixed(0).padStart(6)} B/draw   ${k}`);
});

await browser.close();
srv.close();
