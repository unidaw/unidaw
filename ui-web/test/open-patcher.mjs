#!/usr/bin/env node
/**
 * THE OPEN BUTTON ON A PATCHER CARD OPENS THE PATCHER — it opened nothing.
 *
 * Reported from live use: "clicking the open button on a patcher device should open the patcher,
 * but doesn't."
 *
 * The card has two ways to open a device and they disagreed. Double-click branched on the device
 * KIND — kinds 0..2 are the patcher kinds — and sent a patcher to `onOpenPatcher` and everything
 * else to the plugin's own window. The BUTTON always called `onOpenEditor`. A patcher has no
 * plugin, so the engine skipped it and logged to its own stderr, and the button looked inert.
 *
 * The double-click had this exact bug once before and was fixed by adding the kind check. The
 * button was never brought along — a second copy of one rule, which is how it drifted.
 *
 * ── WHAT THIS ASSERTS ───────────────────────────────────────────────────────────────────────
 *
 * That the VIEW CHANGES to the patcher. Not that a callback fired — a callback that fires and
 * opens the wrong thing is precisely the bug. And it checks the button and the double-click
 * SEPARATELY, because they are separate code paths and fixing one is what happened last time.
 */

import { chromium } from 'playwright';
import { startStack } from './stack.mjs';

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

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
const view = () => page.evaluate(() => {
  const s = window.__uni.state();
  return { view: s.view, view2: s.view2 };
});
const showingPatcher = (v) => v.view === 'patcher' || v.view2 === 'patcher';

console.log('\nopening a patcher from its card\n');

await run('new openpatch');
await settle(1200);
await page.evaluate(() => window.__uni.addDevice(0, 'patcher event'));
await settle(1600);

// The rack has to be on screen for its card to be clickable.
await page.evaluate(() => window.__uni.setView('tracker'));
await settle(600);

const measure = () => page.evaluate(() => {
  const el = document.querySelector('.dv-card');
  if (!el) return null;
  const r = el.getBoundingClientRect();
  const btn = el.querySelector('.dv-open');
  const b = btn ? btn.getBoundingClientRect() : null;
  return {
    // THE NAME, not the centre. The card is 232px and its buttons sit in it; the dblclick
    // handler deliberately ignores presses that land on one (`.dv-add, .dv-open, .dv-del, …`),
    // so a double-click aimed at the middle can be swallowed by a control and look like the
    // gesture not working.
    card: { x: r.left + r.width * 0.35, y: r.top + 12 },
    button: b ? { x: b.left + b.width / 2, y: b.top + b.height / 2 } : null,
  };
});
const card = await measure();
check(!!card, 'the patcher card is on screen');
check(!!(card && card.button), 'and it has an open button',
      'no .dv-open — the button is what was reported broken, so its absence is a different bug');

// ── THE BUTTON, which is the reported failure ───────────────────────────────────────────────
if (card && card.button) {
  await page.evaluate(() => window.__uni.setView('tracker'));
  await settle(400);
  check(!showingPatcher(await view()), 'starting from a view that is NOT the patcher');

  await page.mouse.click(card.button.x, card.button.y);
  await settle(900);
  const after = await view();
  check(showingPatcher(after),
        'CLICKING THE OPEN BUTTON OPENS THE PATCHER',
        `view=${JSON.stringify(after)} — the button used to call the plugin-editor path, which `
        + 'the engine skips for a patcher, so nothing happened and nothing said so');
}

/*
 * THE DOUBLE-CLICK IS NOT ASSERTED HERE, and the reason is worth writing down because I got it
 * wrong twice on the way to it.
 *
 * Testing the second gesture needs a state where the patcher is NOT showing. `setView` changes
 * only the top pane and the patcher opens into the second, so it does not reset. Reloading does
 * — and then the rack was empty, which I reported as a bug: "refresh the tab and the rack claims
 * the track has no devices".
 *
 * IT IS NOT A BUG. Measured both ways:
 *     add a device, reload                     -> cards 2 -> 2   (survives)
 *     `new NAME`, add a device, reload         -> cards 1 -> 0   (gone)
 * `currentProject` persists across the reload, so the page loads that project FROM DISK — and
 * `new` wrote the file before the device existed. The device was never saved. Reloading discards
 * unsaved work, which is what every editor does.
 *
 * So this suite does the button only, on one page. The double-click path is not covered here and
 * that is stated rather than faked: getting to a clean precondition for it needs a way to close
 * the second pane that the test can drive, and inventing one for a test would be the wrong
 * reason to add API.
 *
 * (Worth its own question separately: reloading silently discards unsaved changes with no
 * warning. That is ordinary behaviour and also the kind of thing that loses somebody a song —
 * which happened tonight for a different reason.)
 */

check(errors.length === 0, 'nothing threw in the browser', errors.slice(0, 3).join(' | '));

await browser.close();
await stack.stop();
console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
