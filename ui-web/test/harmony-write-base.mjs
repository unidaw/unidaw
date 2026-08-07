#!/usr/bin/env node
/**
 * FOUR KEY CHANGES TYPED QUICKLY, FOUR KEY CHANGES ACCEPTED.
 *
 * Reported from live use: four scale changes asked for over four bars, one on the lane. This is
 * the WRITE side of that report — `harmony-publish.mjs` is the read side, and it had to space its
 * writes 400ms apart to get past this one, saying so in a comment and filing it separately. This
 * is that file.
 *
 * ── THE MECHANISM ───────────────────────────────────────────────────────────────────────────
 *
 * `WriteHarmony` is arbitrated against `harmonyTimeline.harmonyVersion` — a THIRD counter, not
 * either of the two clip counters. The page stamped its base from `engine.harmonyVersion`, which
 * is the page's mirror of that counter, refreshed from shared memory a frame after the engine
 * bumps it. So between a write and the next refresh the mirror is stale by one, and four writes
 * sent inside that window all quote the SAME base. The engine takes the first and refuses the
 * rest — silently, because the console answers "key set" on the send rather than on the apply.
 *
 * Two things were wrong and the second hid the first:
 *
 *   the page      stamped a base it could not keep current, from a mirror that lags by design
 *   the sidecar   `resolve_base` routed harmony to a CLIP counter, so sending NO base — the
 *                 correct thing to do — looked broken, which is what pushed the page into
 *                 stamping in the first place
 *
 * Fixing only the page would have handed harmony a clip version. Fixing only the sidecar would
 * have changed nothing, because an explicit base is honoured and the page was sending one.
 *
 * ── WHY BACK TO BACK, WITH NO SETTLE ────────────────────────────────────────────────────────
 *
 * The window IS the frame. A test that pauses between writes lets the mirror catch up and passes
 * against the bug — which is exactly what the read-side suite does deliberately, and why that
 * suite could not also cover this. Sending them with no await between is not stress-testing; it
 * is the ordinary speed of the AI writing a progression, which is how it was found.
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
const seen = () => page.evaluate(() => (window.__uni.harmony() || [])
  .map((h) => [h.tick, h.root]).sort((a, b) => a[0] - b[0]));

console.log('\nfour key changes written back to back\n');

await run('new harmwrite');
await settle(1200);

/* ── THE CASE THAT FAILED ─────────────────────────────────────────────────────────────────
 * All four issued without awaiting anything in between, so every one of them is sent inside the
 * window where the page's mirror still holds the pre-write value.
 */
const roots = [0, 3, 6, 10];
await page.evaluate((rs) => {
  const B = 960000 * 4;
  // Deliberately NOT awaited one by one: the point is that they leave together.
  rs.forEach((r, i) => window.__uni.run(`harmony ${r} minor ${i * B}`));
}, roots);
await settle(2500);

const got = await seen();
check(got.length === 4,
      'ALL FOUR ARE ACCEPTED, with no pause between them',
      `${got.length} landed: ${JSON.stringify(got)} — the page used to stamp a base from its own `
      + 'mirror of the harmony counter, so all four quoted the same stale version and three were '
      + 'refused without a word');
check(JSON.stringify(got.map((h) => h[1])) === JSON.stringify(roots),
      'with the roots that were asked for, in bar order', JSON.stringify(got));

/* ── THE REGRESSION THE STAMPING WAS ADDED TO FIX ─────────────────────────────────────────
 *
 * Before stamping, the page sent no base and every harmony write was refused AFTER A PROJECT
 * LOAD — because the sidecar then resolved a missing base to a clip counter. Removing the stamp
 * without fixing that would put this exact failure back, so it is asserted here rather than
 * trusted: save, load, and write once more.
 *
 * This is the half a page-only fix would have broken, and it is silent when it breaks.
 */
await run('save harmwrite');
await settle(1500);
await run('load harmwrite');
await settle(2500);

const beforeReload = (await seen()).length;
check(beforeReload === 4, 'the four survive a save and load', String(beforeReload));

await run(`harmony 5 minor ${4 * BAR}`);
await settle(2000);
const afterLoad = await seen();
check(afterLoad.length === 5,
      'AND A WRITE STRAIGHT AFTER A LOAD STILL LANDS',
      `${afterLoad.length}: ${JSON.stringify(afterLoad)} — this is the failure the base stamping `
      + 'was introduced to fix, so it is the one that a page-only fix would have reintroduced');

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
