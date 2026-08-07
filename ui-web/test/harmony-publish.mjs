#!/usr/bin/env node
/**
 * FOUR KEY CHANGES WRITTEN, FOUR KEY CHANGES ON SCREEN.
 *
 * Reported from live use: four written through the AI, all four acknowledged, all four present in
 * the saved document AND in the published shared-memory region — and TWO in the lane. Reloading
 * the page did not help, because the stale copy was in the sidecar.
 *
 * ── THE MECHANISM, because it is the interesting part ────────────────────────────────────────
 *
 * `uiHarmonyVersion` in the header is bumped on the COMMAND thread the instant a write is
 * accepted. The region holding the events is refilled by the CONSUMER on its next pass. The
 * sidecar cached the events and stamped "read at <header version>" BEFORE reading them, so when
 * the header ran ahead it recorded a version the events had not reached, and the header never
 * moved again. Permanently two, through reloads.
 *
 * The fix is that the region now carries the version it corresponds to, written after the events
 * under the lock that guards them, and the sidecar compares against THAT. One consistent read
 * instead of an assumption about another thread's schedule.
 *
 * ── WHY THIS ASSERTS WHAT THE PAGE SEES ─────────────────────────────────────────────────────
 *
 * Not the document, and not the region — both were RIGHT while the lane was wrong. The only
 * reading that would have caught this is the one the browser actually has, so that is what this
 * takes: `__uni.harmony()`, the array the tracker draws its blocks from.
 *
 * AND IT WRITES THEM FAST, with no settle between, because the window is one consumer pass. A test
 * that pauses politely between writes never reproduces it.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

const Q = 960000;
const BAR = Q * 4;
const stack = await startStack({ keepDir: true });
const browser = await chromium.launch({ channel: 'chrome' });
const page = await browser.newPage({ viewport: { width: 1600, height: 950 } });
const errors = [];
page.on('pageerror', (e) => errors.push(e.message));
await page.goto(stack.url, { waitUntil: 'load' });
await page.waitForFunction(() => window.__uni && window.__uni.canSend(), null, { timeout: 20000 });
await page.waitForTimeout(1500);

const run = (c) => page.evaluate((x) => window.__uni.run(x), c);
const settle = (ms) => page.waitForTimeout(ms);
/** What the PAGE has — the array the harmony column is built from. */
const seen = () => page.evaluate(() => (window.__uni.harmony() || [])
  .map((h) => [h.tick, h.root, h.scaleId]).sort((a, b) => a[0] - b[0]));

console.log('\nfour key changes reach the page\n');

await run('new harmpub');
await settle(1200);

/*
 * WRITTEN ONE AT A TIME, and the reason is a SECOND bug found writing this test.
 *
 * Back to back, only the FIRST lands. The console's `harmony` verb stamps its base from the page's
 * copy of the harmony version, which updates a frame later, so four rapid writes all quote the
 * same stale base and the engine refuses three of them — silently, since the console reports
 * "key set" on the send rather than on the apply.
 *
 * That is a genuine defect on the WRITE side and it is NOT the one this file is about: the agent's
 * `set_harmony` waits for its own apply, which is why the reported case had all four in the
 * document and only two on screen. Filed separately; here the writes are spaced so that what is
 * being tested is the READ path.
 */
const roots = [0, 3, 6, 10];
for (let i = 0; i < roots.length; i++) {
  // `harmony <root> <scale> [tick]` — root FIRST.
  await run(`harmony ${roots[i]} minor ${i * BAR}`);
  await settle(400);
}
await settle(1500);

const got = await seen();
check(got.length === 4,
      'ALL FOUR KEY CHANGES REACH THE PAGE',
      `the page has ${got.length}: ${JSON.stringify(got)} — the document and the published region `
      + 'both held four while the lane showed two, so this is the only reading that catches it');
check(JSON.stringify(got.map((h) => h[0])) === JSON.stringify(roots.map((_, i) => i * BAR)),
      'at one-bar intervals, in order', JSON.stringify(got.map((h) => h[0])));
check(JSON.stringify(got.map((h) => h[1])) === JSON.stringify(roots),
      'with the roots that were asked for', JSON.stringify(got.map((h) => h[1])));

/* ── AND A FIFTH, LATER, still arrives ─────────────────────────────────────────────────────
 * The failure was a cache that stopped invalidating. A count check alone would pass a test that
 * only ever grows the timeline by one; this adds one AFTER a settled read, which is precisely
 * when the old code had declared itself current and stopped looking.
 */
await run(`harmony 5 minor ${4 * BAR}`);
await settle(2000);
const after = await seen();
check(after.length === 5,
      'a FIFTH change, added after the cache had settled, still arrives',
      `${JSON.stringify(after)} — the old failure was a cache that had stopped invalidating, so `
      + 'the interesting moment is the write that comes after everything looks quiet');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
