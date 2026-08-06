#!/usr/bin/env node
/* Measurement only: what does a node ADDED through the app carry, per type? */
import { chromium } from 'playwright';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { startStack } from '/Users/jak/src/daw-web/ui-web/test/stack.mjs';

const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
page.on('pageerror', (e) => console.log('PAGEERROR', e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);
const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);

await run('new probeadd');
await settle(1200);
await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
await settle(1500);
await page.evaluate(() => window.__uni.setView('patcher'));
await settle(800);
console.log('patchTarget', JSON.stringify(await page.evaluate(() => window.__uni.patchTarget())));

console.log('BEFORE', JSON.stringify(await page.evaluate(() => window.__uni.patcher().nodes)));

for (const t of ['euclidean', 'lfo', 'random', 'slice']) {
  console.log('addnode', t, await run('addnode ' + t).catch((e) => 'THREW ' + e.message));
  await settle(900);
}

console.log('AFTER  ', JSON.stringify(await page.evaluate(() => window.__uni.patcher().nodes)));
const unpub = await page.evaluate(() =>
  [...document.querySelectorAll('.pt-node')]
    .filter((el) => el.offsetParent !== null)
    .map((el) => {
      const e = el.querySelector('.pt-empty');
      return { id: Number(el.dataset.id), empty: !!(e && e.style.display !== 'none') };
    }));
console.log('DOM    ', JSON.stringify(unpub));

await run('save probeadd_a');
for (let i = 0; i < 40; i++) {
  const p = join(stack.dir, 'probeadd_a.uniproj.json');
  if (existsSync(p)) {
    try {
      const doc = JSON.parse(readFileSync(p, 'utf8'));
      const devs = (doc.tracks || []).flatMap((t) => t.device_chain || []);
      for (const d of devs) {
        if (d.patcher || d.graph) {
          console.log('SAVED  ', JSON.stringify((d.patcher || d.graph).nodes));
        }
      }
      break;
    } catch { /* mid-write */ }
  }
  await settle(150);
}

await browser.close();
await stack.stop();
